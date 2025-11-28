#!/usr/bin/env node
// jank WASM Hot-Reload Server
//
// This server:
// 1. Serves static files (HTML, JS, WASM) for the browser
// 2. Accepts WebSocket connections from browsers at /repl
// 3. Accepts nREPL connections from editors (Emacs, etc.)
// 4. When code is evaluated, generates WASM patch and broadcasts to browsers
//
// Usage: node hot_reload_server.cjs
// Then open: http://localhost:8080/eita_hot_reload.html
// Connect nREPL from Emacs: M-x cider-connect localhost:7888

const http = require('http');
const fs = require('fs');
const path = require('path');
const { execSync, spawn } = require('child_process');
const net = require('net');

// Try to load WebSocket library
let WebSocketServer;
try {
  WebSocketServer = require('ws').Server;
} catch (e) {
  console.log('WebSocket library not found. Installing...');
  execSync('npm install ws', { stdio: 'inherit' });
  WebSocketServer = require('ws').Server;
}

// Simple bencode implementation for nREPL
const bencode = {
  encode: function(obj) {
    if (typeof obj === 'number') {
      return Buffer.from(`i${obj}e`);
    } else if (typeof obj === 'string') {
      const buf = Buffer.from(obj, 'utf-8');
      return Buffer.concat([Buffer.from(`${buf.length}:`), buf]);
    } else if (Buffer.isBuffer(obj)) {
      return Buffer.concat([Buffer.from(`${obj.length}:`), obj]);
    } else if (Array.isArray(obj)) {
      const parts = [Buffer.from('l')];
      for (const item of obj) {
        parts.push(this.encode(item));
      }
      parts.push(Buffer.from('e'));
      return Buffer.concat(parts);
    } else if (typeof obj === 'object' && obj !== null) {
      const parts = [Buffer.from('d')];
      const keys = Object.keys(obj).sort();
      for (const key of keys) {
        parts.push(this.encode(key));
        parts.push(this.encode(obj[key]));
      }
      parts.push(Buffer.from('e'));
      return Buffer.concat(parts);
    }
    throw new Error('Cannot encode: ' + typeof obj);
  },

  decode: function(buffer, start = 0) {
    if (start >= buffer.length) return null;
    const char = String.fromCharCode(buffer[start]);

    if (char === 'i') {
      // Integer
      const end = buffer.indexOf('e', start);
      if (end === -1) return null;
      const num = parseInt(buffer.slice(start + 1, end).toString(), 10);
      return { value: num, end: end + 1 };
    } else if (char === 'l') {
      // List
      const list = [];
      let pos = start + 1;
      while (pos < buffer.length && buffer[pos] !== 101) { // 'e' = 101
        const result = this.decode(buffer, pos);
        if (!result) return null;
        list.push(result.value);
        pos = result.end;
      }
      return { value: list, end: pos + 1 };
    } else if (char === 'd') {
      // Dictionary
      const dict = {};
      let pos = start + 1;
      while (pos < buffer.length && buffer[pos] !== 101) { // 'e' = 101
        const keyResult = this.decode(buffer, pos);
        if (!keyResult) return null;
        pos = keyResult.end;
        const valResult = this.decode(buffer, pos);
        if (!valResult) return null;
        dict[keyResult.value.toString()] = valResult.value;
        pos = valResult.end;
      }
      return { value: dict, end: pos + 1 };
    } else if (char >= '0' && char <= '9') {
      // String
      const colonPos = buffer.indexOf(':', start);
      if (colonPos === -1) return null;
      const len = parseInt(buffer.slice(start, colonPos).toString(), 10);
      const strEnd = colonPos + 1 + len;
      if (strEnd > buffer.length) return null;
      const str = buffer.slice(colonPos + 1, strEnd);
      return { value: str, end: strEnd };
    }
    return null;
  }
};

// Configuration
const HTTP_PORT = 8080;
const WS_PORT = 7888;
const NREPL_PORT = 7889;

// Paths
const BASE_DIR = __dirname;
const JANK_DIR = '/Users/pfeodrippe/dev/jank/compiler+runtime';
const WASM_BUILD_DIR = path.join(JANK_DIR, 'build-wasm');
const PATCH_GENERATOR = path.join(JANK_DIR, 'bin/generate-wasm-patch-auto');

// Connected browser clients
const browserClients = new Set();

// Statistics
const stats = {
  patchesGenerated: 0,
  patchesSent: 0,
  errors: 0,
  lastPatchTime: null
};

// ============== HTTP Server (Static Files) ==============

const MIME_TYPES = {
  '.html': 'text/html',
  '.js': 'application/javascript',
  '.wasm': 'application/wasm',
  '.css': 'text/css',
  '.json': 'application/json',
  '.png': 'image/png',
  '.ico': 'image/x-icon'
};

const httpServer = http.createServer((req, res) => {
  // Handle POST /eval endpoint
  if (req.method === 'POST' && req.url === '/eval') {
    let body = '';
    req.on('data', chunk => { body += chunk.toString(); });
    req.on('end', () => {
      console.log(`[HTTP] POST /eval: ${body.substring(0, 100)}`);
      handleEval(body, (result, error) => {
        res.writeHead(200, {
          'Content-Type': 'application/json',
          'Access-Control-Allow-Origin': '*'
        });
        res.end(JSON.stringify({ result, error }));
      });
    });
    return;
  }

  // Handle CORS preflight
  if (req.method === 'OPTIONS') {
    res.writeHead(200, {
      'Access-Control-Allow-Origin': '*',
      'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
      'Access-Control-Allow-Headers': 'Content-Type'
    });
    res.end();
    return;
  }

  let filePath = req.url === '/' ? '/eita_hot_reload.html' : req.url;

  // Remove query string
  filePath = filePath.split('?')[0];

  // Security: prevent directory traversal
  filePath = filePath.replace(/\.\./g, '');

  // Try to find file in multiple locations
  const possiblePaths = [
    path.join(BASE_DIR, filePath),
    path.join(WASM_BUILD_DIR, filePath),
    path.join(JANK_DIR, filePath)
  ];

  let fullPath = null;
  for (const p of possiblePaths) {
    if (fs.existsSync(p)) {
      fullPath = p;
      break;
    }
  }

  if (!fullPath) {
    res.writeHead(404, { 'Content-Type': 'text/plain' });
    res.end(`Not found: ${filePath}`);
    console.log(`[HTTP] 404 ${filePath}`);
    return;
  }

  const ext = path.extname(fullPath).toLowerCase();
  const contentType = MIME_TYPES[ext] || 'application/octet-stream';

  // Read and serve file
  fs.readFile(fullPath, (err, data) => {
    if (err) {
      res.writeHead(500, { 'Content-Type': 'text/plain' });
      res.end(`Error reading file: ${err.message}`);
      return;
    }

    res.writeHead(200, {
      'Content-Type': contentType,
      'Access-Control-Allow-Origin': '*'
    });
    res.end(data);
    console.log(`[HTTP] 200 ${filePath} (${data.length} bytes)`);
  });
});

// ============== WebSocket Server (Browser Clients) ==============

const wsServer = new WebSocketServer({ port: WS_PORT });

wsServer.on('connection', (ws, req) => {
  console.log(`[WS] Browser connected from ${req.socket.remoteAddress}`);
  browserClients.add(ws);

  ws.on('message', (data) => {
    try {
      const msg = JSON.parse(data.toString());
      handleBrowserMessage(ws, msg);
    } catch (e) {
      console.error('[WS] Invalid message:', e.message);
    }
  });

  ws.on('close', () => {
    browserClients.delete(ws);
    console.log('[WS] Browser disconnected');
  });

  ws.on('error', (err) => {
    console.error('[WS] Error:', err.message);
    browserClients.delete(ws);
  });

  // Send handshake acknowledgment
  ws.send(JSON.stringify({
    type: 'handshake_ack',
    server: 'jank-hot-reload-server',
    version: '1.0.0'
  }));
});

function handleBrowserMessage(ws, msg) {
  console.log(`[WS] Received: ${msg.type}`);

  switch (msg.type) {
    case 'handshake':
      console.log(`[WS] Client: ${msg.client} v${msg.version}`);
      break;

    case 'eval':
      // Browser wants to eval code
      handleEval(msg.code, (result, error) => {
        if (error) {
          ws.send(JSON.stringify({ type: 'error', error }));
        } else {
          ws.send(JSON.stringify({ type: 'eval_result', result }));
        }
      });
      break;

    default:
      console.log(`[WS] Unknown message type: ${msg.type}`);
  }
}

// ============== Patch Generation ==============

// Patch counter to ensure unique filenames
let patchCounter = 0;

function generatePatch(code, callback) {
  const startTime = Date.now();
  const patchId = ++patchCounter;

  // Extract namespace from code, or default to 'eita'
  let namespace = 'eita';
  const nsMatch = code.match(/\(ns\s+([a-zA-Z0-9._-]+)/);
  if (nsMatch) {
    namespace = nsMatch[1];
  } else {
    // No namespace found, prepend it
    code = `(ns ${namespace})\n\n${code}`;
  }

  // Use unique directory per patch to completely avoid caching
  const outputDir = path.join(BASE_DIR, 'generated_patches', `patch_${patchId}`);
  const tempFile = path.join(outputDir, `${namespace}_input.jank`);

  // Create fresh output directory
  if (fs.existsSync(outputDir)) {
    fs.rmSync(outputDir, { recursive: true });
  }
  fs.mkdirSync(outputDir, { recursive: true });

  fs.writeFileSync(tempFile, code);

  console.log(`[PATCH] Generating patch for:\n${code.substring(0, 100)}...`);

  // Run patch generator with unique patch ID
  try {
    const result = execSync(
      `"${PATCH_GENERATOR}" "${tempFile}" --output-dir "${outputDir}" --patch-id "${patchId}"`,
      { encoding: 'utf-8', timeout: 30000 }
    );

    console.log(`[PATCH] Generator output: ${result}`);

    // Extract the patch ID from the output (should match what we passed)
    const patchIdMatch = result.match(/PATCH_ID=(\d+)/);
    const actualPatchId = patchIdMatch ? patchIdMatch[1] : patchId;
    const symbolName = `jank_patch_symbols_${actualPatchId}`;
    console.log(`[PATCH] Using symbol name: ${symbolName}`);

    // Find generated .wasm file
    const wasmFiles = fs.readdirSync(outputDir)
      .filter(f => f.endsWith('_patch.wasm'))
      .map(f => ({
        name: f,
        time: fs.statSync(path.join(outputDir, f)).mtime.getTime()
      }))
      .sort((a, b) => b.time - a.time);

    if (wasmFiles.length === 0) {
      throw new Error('No .wasm file generated');
    }

    const wasmPath = path.join(outputDir, wasmFiles[0].name);
    const wasmData = fs.readFileSync(wasmPath);
    const base64Data = wasmData.toString('base64');

    const genTime = Date.now() - startTime;
    stats.patchesGenerated++;
    stats.lastPatchTime = genTime;

    console.log(`[PATCH] Generated ${wasmFiles[0].name} (${wasmData.length} bytes) in ${genTime}ms`);

    // Extract symbol info from corresponding .cpp file
    const cppPath = wasmPath.replace('.wasm', '.cpp');
    let symbols = [];
    if (fs.existsSync(cppPath)) {
      const cppContent = fs.readFileSync(cppPath, 'utf-8');
      const match = cppContent.match(/\{ "([^"]+)", "(\d+)"/);
      if (match) {
        symbols = [{ name: match[1], arity: match[2] }];
      }
    }

    callback(null, {
      data: base64Data,
      symbols,
      symbolName,  // The unique symbol name to look for
      size: wasmData.length,
      time: genTime
    });

  } catch (err) {
    stats.errors++;
    console.error(`[PATCH] Error: ${err.message}`);
    callback(err.message, null);
  } finally {
    // Cleanup temp file
    if (fs.existsSync(tempFile)) {
      fs.unlinkSync(tempFile);
    }
  }
}

function broadcastPatch(patchData) {
  const msg = JSON.stringify({
    type: 'patch',
    data: patchData.data,
    symbols: patchData.symbols,
    symbolName: patchData.symbolName,  // Unique symbol name for dlsym
    timestamp: Date.now()
  });

  let sent = 0;
  for (const client of browserClients) {
    if (client.readyState === 1) { // WebSocket.OPEN
      client.send(msg);
      sent++;
    }
  }

  stats.patchesSent += sent;
  console.log(`[PATCH] Broadcast to ${sent} browser(s)`);
}

function handleEval(code, callback) {
  // Check if this is a defn form
  if (code.includes('(defn ') || code.includes('(defn^')) {
    generatePatch(code, (err, patchData) => {
      if (err) {
        callback(null, `Error generating patch: ${err}`);
      } else {
        broadcastPatch(patchData);
        callback(`Patch generated and sent (${patchData.size} bytes, ${patchData.time}ms)`, null);
      }
    });
  } else {
    // Not a defn - just acknowledge
    callback(`Received: ${code.substring(0, 50)}...`, null);
  }
}

// ============== nREPL Server ==============

const nreplSessions = new Map();
let sessionCounter = 0;

const nreplServer = net.createServer((socket) => {
  const sessionId = `session-${++sessionCounter}`;
  console.log(`[nREPL] Client connected: ${sessionId}`);

  let buffer = Buffer.alloc(0);

  socket.on('data', (data) => {
    buffer = Buffer.concat([buffer, data]);

    // Try to decode bencode messages
    try {
      while (buffer.length > 0) {
        const result = bencode.decode(buffer);
        if (!result) break;

        // Slice the consumed bytes
        buffer = buffer.slice(result.end);

        handleNreplMessage(socket, result.value, sessionId);
      }
    } catch (e) {
      // Incomplete message, wait for more data
      console.error('[nREPL] Parse error:', e.message);
    }
  });

  socket.on('close', () => {
    console.log(`[nREPL] Client disconnected: ${sessionId}`);
    nreplSessions.delete(sessionId);
  });

  socket.on('error', (err) => {
    console.error(`[nREPL] Socket error: ${err.message}`);
  });
});

function handleNreplMessage(socket, msg, sessionId) {
  const op = msg.op?.toString();
  const id = msg.id?.toString() || '0';

  console.log(`[nREPL] op=${op} id=${id}`);

  switch (op) {
    case 'clone':
      sendNreplResponse(socket, {
        id,
        'new-session': sessionId,
        status: ['done']
      });
      break;

    case 'describe':
      sendNreplResponse(socket, {
        id,
        session: sessionId,
        ops: {
          clone: {},
          close: {},
          describe: {},
          eval: {},
          'load-file': {}
        },
        versions: {
          'jank': '0.1.0',
          'hot-reload': '1.0.0'
        },
        status: ['done']
      });
      break;

    case 'eval':
      let code = msg.code?.toString() || '';
      const evalNs = msg.ns?.toString() || 'eita';
      console.log(`[nREPL] Eval (ns=${evalNs}): ${code.substring(0, 100)}`);

      // Prepend namespace if not already in code
      if (!code.includes('(ns ')) {
        code = `(ns ${evalNs})\n\n${code}`;
      }

      handleEval(code, (result, error) => {
        if (error) {
          sendNreplResponse(socket, {
            id,
            session: sessionId,
            err: error,
            status: ['done']
          });
        } else {
          sendNreplResponse(socket, {
            id,
            session: sessionId,
            value: result,
            status: ['done']
          });
        }
      });
      break;

    case 'load-file':
      const fileContent = msg.file?.toString() || '';
      const fileName = msg['file-name']?.toString() || 'unknown.jank';
      console.log(`[nREPL] Load file: ${fileName}`);

      handleEval(fileContent, (result, error) => {
        sendNreplResponse(socket, {
          id,
          session: sessionId,
          value: error || result,
          status: ['done']
        });
      });
      break;

    case 'close':
      sendNreplResponse(socket, {
        id,
        session: sessionId,
        status: ['done']
      });
      nreplSessions.delete(sessionId);
      break;

    default:
      sendNreplResponse(socket, {
        id,
        session: sessionId,
        status: ['error', 'unknown-op', 'done']
      });
  }
}

function sendNreplResponse(socket, msg) {
  try {
    const encoded = bencode.encode(msg);
    socket.write(encoded);
  } catch (e) {
    console.error('[nREPL] Failed to send response:', e.message);
  }
}

// ============== Start Servers ==============

console.log('');
console.log('╔════════════════════════════════════════════════════════════╗');
console.log('║         jank WASM Hot-Reload Server                        ║');
console.log('╠════════════════════════════════════════════════════════════╣');
console.log('║                                                            ║');
console.log(`║  HTTP Server:     http://localhost:${HTTP_PORT}                    ║`);
console.log(`║  WebSocket:       ws://localhost:${WS_PORT}/repl                   ║`);
console.log(`║  nREPL:           localhost:${NREPL_PORT}                          ║`);
console.log('║                                                            ║');
console.log('║  Open in browser: http://localhost:8080/eita_hot_reload.html ║');
console.log('║                                                            ║');
console.log('║  Connect from Emacs:                                       ║');
console.log('║    M-x cider-connect RET localhost RET 7889 RET            ║');
console.log('║                                                            ║');
console.log('║  Or send code directly:                                    ║');
console.log('║    curl -X POST -d "(defn ggg [v] (+ 49 v))" \\             ║');
console.log('║         http://localhost:8080/eval                         ║');
console.log('║                                                            ║');
console.log('╚════════════════════════════════════════════════════════════╝');
console.log('');

httpServer.listen(HTTP_PORT, () => {
  console.log(`[HTTP] Server running on port ${HTTP_PORT}`);
});

nreplServer.listen(NREPL_PORT, () => {
  console.log(`[nREPL] Server running on port ${NREPL_PORT}`);
});

console.log(`[WS] Server running on port ${WS_PORT}`);
console.log('');
console.log('Waiting for connections...');
console.log('');

// Handle graceful shutdown
process.on('SIGINT', () => {
  console.log('\nShutting down...');
  httpServer.close();
  wsServer.close();
  nreplServer.close();
  process.exit(0);
});
