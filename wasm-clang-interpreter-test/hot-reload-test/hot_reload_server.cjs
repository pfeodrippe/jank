#!/usr/bin/env node
// jank WASM Hot-Reload Server
//
// This server is a thin proxy that:
// 1. Serves static files (HTML, JS, WASM) for the browser
// 2. Accepts WebSocket connections from browsers at /repl
// 3. Accepts nREPL connections from editors (Emacs, etc.)
// 4. Forwards all nREPL ops to jank's native nREPL server
// 5. When a defn is evaluated, asks jank to generate C++ code, compiles to WASM, and broadcasts to browsers
//
// IMPORTANT: jank must be running with --server flag for this to work:
//   ./build/jank repl --server
//
// Usage: node hot_reload_server.cjs
// Then open: http://localhost:8080/eita_hot_reload.html
// Connect nREPL from Emacs: M-x cider-connect localhost:7889

const http = require('http');
const fs = require('fs');
const path = require('path');
const { execSync } = require('child_process');
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
const JANK_NREPL_PORT = process.env.JANK_NREPL_PORT || 5555;

// Paths
const BASE_DIR = __dirname;
const JANK_DIR = '/Users/pfeodrippe/dev/jank/compiler+runtime';
const WASM_BUILD_DIR = path.join(JANK_DIR, 'build-wasm');

// emcc compilation flags for WASM patches
const EMCC_FLAGS = [
  '-sSIDE_MODULE=1',
  '-O2',
  '-fPIC',
  '-std=c++20',
  '-I' + path.join(JANK_DIR, 'include/cpp'),
  '-I' + path.join(JANK_DIR, 'third-party/jtl/include'),
  '-I' + path.join(JANK_DIR, 'third-party/bpptree/include'),
  '-I' + path.join(JANK_DIR, 'third-party/immer'),
  '-I' + path.join(JANK_DIR, 'third-party/magic_enum/include')
];

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

// ============== jank nREPL Client ==============

// Connection to jank native nREPL server
let jankConnection = null;
let jankMessageId = 0;
let jankPendingCallbacks = new Map();
let jankSessionId = null;

function connectToJank() {
  if (jankConnection && !jankConnection.destroyed) {
    return Promise.resolve(jankConnection);
  }

  return new Promise((resolve, reject) => {
    console.log(`[JANK] Connecting to jank nREPL at localhost:${JANK_NREPL_PORT}...`);

    const socket = net.createConnection({ port: JANK_NREPL_PORT, host: 'localhost' }, () => {
      console.log('[JANK] Connected to jank nREPL server');
      jankConnection = socket;

      // Clone a session first
      sendJankMessage({ op: 'clone' }, (response) => {
        jankSessionId = response['new-session']?.toString() || 'default-session';
        console.log(`[JANK] Got session: ${jankSessionId}`);
        resolve(socket);
      });
    });

    let buffer = Buffer.alloc(0);

    socket.on('data', (data) => {
      buffer = Buffer.concat([buffer, data]);

      // Try to decode bencode messages
      while (buffer.length > 0) {
        const result = bencode.decode(buffer);
        if (!result) break;

        buffer = buffer.slice(result.end);
        handleJankResponse(result.value);
      }
    });

    socket.on('error', (err) => {
      console.error(`[JANK] Connection error: ${err.message}`);
      jankConnection = null;
      reject(err);
    });

    socket.on('close', () => {
      console.log('[JANK] Connection closed');
      jankConnection = null;
    });
  });
}

function sendJankMessage(msg, callback) {
  const id = `msg-${++jankMessageId}`;
  msg.id = id;
  if (jankSessionId && !msg.session) {
    msg.session = jankSessionId;
  }

  jankPendingCallbacks.set(id, callback);

  const encoded = bencode.encode(msg);
  jankConnection.write(encoded);
}

function handleJankResponse(msg) {
  const id = msg.id?.toString();
  if (!id) return;

  const callback = jankPendingCallbacks.get(id);
  if (callback) {
    // Check if response is complete (has status with done)
    const status = msg.status;
    if (status && Array.isArray(status) && status.some(s => s.toString() === 'done')) {
      jankPendingCallbacks.delete(id);
    }
    callback(msg);
  }
}

// ============== Patch Generation (via jank nREPL) ==============

// Patch counter to ensure unique filenames
let patchCounter = 0;

// Generate WASM patch using jank's compiler via nREPL
function generatePatch(code, callback) {
  const startTime = Date.now();
  const patchId = ++patchCounter;

  console.log(`[PATCH] Generating patch via jank (patch ${patchId})...`);

  connectToJank()
    .then(() => {
      // Send wasm-compile-patch op to jank
      sendJankMessage({
        op: 'wasm-compile-patch',
        code: code,
        'patch-id': patchId.toString()
      }, (response) => {
        // Check for errors
        if (response.err) {
          console.error(`[PATCH] jank error: ${response.err}`);
          stats.errors++;
          callback(response.err.toString(), null);
          return;
        }

        // We may get multiple responses, wait for the one with cpp-code
        if (!response['cpp-code']) {
          return; // Wait for the next response
        }

        const cppCode = response['cpp-code'].toString();
        const varName = response['var-name']?.toString() || 'unknown';
        const nsName = response['ns-name']?.toString() || 'user';
        const responsePatchId = response['patch-id']?.toString() || patchId.toString();

        console.log(`[PATCH] Got C++ from jank (${cppCode.length} bytes) for ${nsName}/${varName}`);

        // Compile C++ to WASM using emcc
        compileCppToWasm(cppCode, nsName, responsePatchId, (err, wasmData) => {
          if (err) {
            stats.errors++;
            callback(err, null);
            return;
          }

          const genTime = Date.now() - startTime;
          stats.patchesGenerated++;
          stats.lastPatchTime = genTime;

          const base64Data = wasmData.toString('base64');
          const symbolName = `jank_patch_symbols_${responsePatchId}`;

          console.log(`[PATCH] Generated WASM (${wasmData.length} bytes) in ${genTime}ms`);

          callback(null, {
            data: base64Data,
            symbols: [{ name: varName, arity: '?' }],
            symbolName,
            size: wasmData.length,
            time: genTime
          });
        });
      });
    })
    .catch((err) => {
      console.error(`[PATCH] Failed to connect to jank: ${err.message}`);
      console.error('[PATCH] Make sure jank is running with --server flag:');
      console.error('[PATCH]   ./build/jank repl --server');
      stats.errors++;
      callback(`jank nREPL not available: ${err.message}`, null);
    });
}

// Compile C++ code to WASM using emcc (build step, not code generation)
function compileCppToWasm(cppCode, nsName, patchId, callback) {
  const outputDir = path.join(BASE_DIR, 'generated_patches', `patch_${patchId}`);
  const cppPath = path.join(outputDir, `${nsName}_patch.cpp`);
  const wasmPath = path.join(outputDir, `${nsName}_patch.wasm`);

  // Create output directory
  if (fs.existsSync(outputDir)) {
    fs.rmSync(outputDir, { recursive: true });
  }
  fs.mkdirSync(outputDir, { recursive: true });

  // Write C++ code
  fs.writeFileSync(cppPath, cppCode);
  console.log(`[PATCH] Wrote C++ to ${cppPath}`);

  // Compile with emcc
  const emccCmd = [
    'emcc',
    cppPath,
    '-o', wasmPath,
    ...EMCC_FLAGS
  ].join(' ');

  console.log(`[PATCH] Compiling with emcc...`);

  try {
    execSync(emccCmd, { encoding: 'utf-8', timeout: 60000, stdio: 'pipe' });

    if (!fs.existsSync(wasmPath)) {
      callback('emcc produced no output', null);
      return;
    }

    const wasmData = fs.readFileSync(wasmPath);
    callback(null, wasmData);
  } catch (err) {
    console.error(`[PATCH] emcc error: ${err.message}`);
    if (err.stderr) {
      console.error(`[PATCH] emcc stderr: ${err.stderr}`);
    }
    callback(`emcc compilation failed: ${err.message}`, null);
  }
}

function broadcastPatch(patchData) {
  const msg = JSON.stringify({
    type: 'patch',
    data: patchData.data,
    symbols: patchData.symbols,
    symbolName: patchData.symbolName,
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

// ============== nREPL Server (for editors) ==============

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

        buffer = buffer.slice(result.end);
        handleNreplMessage(socket, result.value, sessionId);
      }
    } catch (e) {
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

// Forward all nREPL messages to jank's native nREPL server
function handleNreplMessage(socket, msg, sessionId) {
  const op = msg.op?.toString();
  const id = msg.id?.toString() || '0';
  const code = msg.code?.toString() || '';

  console.log(`[nREPL] op=${op} id=${id}`);
  if (code) {
    console.log(`[nREPL] code: ${code.substring(0, 80)}${code.length > 80 ? '...' : ''}`);
  }

  // Check if this is a defn that should trigger hot-reload
  const isDefn = code.includes('(defn ') || code.includes('(defn^');

  // Forward the message to jank's nREPL
  forwardToJank(socket, msg, sessionId, (response) => {
    // If this was an eval with a defn, also generate a WASM patch
    if ((op === 'eval' || op === 'load-file') && isDefn) {
      console.log(`[nREPL] Detected defn, generating WASM patch...`);

      generatePatch(code, (err, patchData) => {
        if (err) {
          console.error(`[nREPL] Patch generation failed: ${err}`);
        } else {
          broadcastPatch(patchData);
        }
      });
    }
  });
}

// Forward a message to jank's native nREPL and relay responses back
function forwardToJank(editorSocket, msg, sessionId, onComplete) {
  connectToJank()
    .then(() => {
      const id = msg.id?.toString() || `fwd-${++jankMessageId}`;

      // Store callback to relay responses back to editor
      jankPendingCallbacks.set(id, (response) => {
        // Relay the response to the editor
        sendNreplResponse(editorSocket, response);

        // Check if this is the final response
        const status = response.status;
        if (status && Array.isArray(status) && status.some(s => s.toString() === 'done')) {
          if (onComplete) onComplete(response);
        }
      });

      // Forward the message to jank
      const encoded = bencode.encode(msg);
      jankConnection.write(encoded);
    })
    .catch((err) => {
      console.error(`[nREPL] Failed to forward to jank: ${err.message}`);
      console.error('[nREPL] Make sure jank is running with --server flag:');
      console.error('[nREPL]   ./build/jank repl --server');
      // Send error back to editor
      sendNreplResponse(editorSocket, {
        id: msg.id?.toString() || '0',
        session: sessionId,
        err: `jank nREPL not available. Start with: ./build/jank repl --server`,
        status: ['error', 'done']
      });
    });
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
console.log(`║  nREPL (editor):  localhost:${NREPL_PORT}                          ║`);
console.log(`║  jank nREPL:      localhost:${JANK_NREPL_PORT} (REQUIRED)                ║`);
console.log('║                                                            ║');
console.log('║  Open in browser: http://localhost:8080/eita_hot_reload.html ║');
console.log('║                                                            ║');
console.log('║  Connect from Emacs:                                       ║');
console.log('║    M-x cider-connect RET localhost RET 7889 RET            ║');
console.log('║                                                            ║');
console.log('║  IMPORTANT: jank must be running with --server:            ║');
console.log('║    cd compiler+runtime && ./build/jank repl --server       ║');
console.log('║                                                            ║');
console.log('╚════════════════════════════════════════════════════════════╝');
console.log('');

wsServer.on('listening', () => {
  console.log(`[WS] Server running on port ${WS_PORT}`);
});

httpServer.listen(HTTP_PORT, () => {
  console.log(`[HTTP] Server running on port ${HTTP_PORT}`);
});

nreplServer.listen(NREPL_PORT, () => {
  console.log(`[nREPL] Server running on port ${NREPL_PORT}`);
});

console.log('');
console.log('Waiting for connections...');
console.log('');
