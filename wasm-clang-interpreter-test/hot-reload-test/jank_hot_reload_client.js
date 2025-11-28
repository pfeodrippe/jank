/**
 * jank Hot-Reload Client
 *
 * Browser-side WebSocket client for jank WASM hot-reload.
 * Automatically included when building with HOT_RELOAD=1.
 *
 * Features:
 * - Connects to jank nREPL hot-reload server (ws://localhost:7888/repl)
 * - Receives WASM patch binaries
 * - Loads patches via jank_hot_reload_load_patch()
 * - Updates vars in real-time (~180-210ms total)
 *
 * Usage:
 *   // Automatically starts when jank WASM module loads (if HOT_RELOAD=1)
 *   // Or manually:
 *   const client = new JankHotReloadClient(Module);
 *   client.connect();
 *
 *   // From browser devtools:
 *   jankEval('(defn foo [x] (+ x 1))');
 *
 * @version 1.0.0
 * @author jank hot-reload system
 */

class JankHotReloadClient {
  constructor(wasmModule, options = {}) {
    this.module = wasmModule;
    this.serverUrl = options.serverUrl || 'ws://localhost:7888/repl';
    this.autoReconnect = options.autoReconnect !== false;
    this.reconnectDelay = options.reconnectDelay || 2000;
    this.debug = options.debug || false;

    this.ws = null;
    this.evalQueue = [];
    this.patchCounter = 0;
    this.connected = false;
    this.stats = {
      patchesLoaded: 0,
      totalBytes: 0,
      errors: 0,
      lastPatchTime: null
    };
  }

  /**
   * Connect to the hot-reload server.
   * Automatically reconnects on disconnect if autoReconnect is true.
   */
  connect() {
    if (this.connected) {
      this.log('Already connected to hot-reload server');
      return;
    }

    this.log(`Connecting to ${this.serverUrl}...`);

    try {
      this.ws = new WebSocket(this.serverUrl);
      this.setupEventHandlers();
    } catch (err) {
      this.error('Failed to create WebSocket:', err);
      if (this.autoReconnect) {
        setTimeout(() => this.connect(), this.reconnectDelay);
      }
    }
  }

  /**
   * Set up WebSocket event handlers.
   * @private
   */
  setupEventHandlers() {
    this.ws.onopen = () => {
      this.connected = true;
      this.log('Connected to hot-reload server!');
      this.processQueue();

      // Send initial handshake
      this.send({
        type: 'handshake',
        client: 'jank-wasm-browser',
        version: '1.0.0'
      });
    };

    this.ws.onmessage = (evt) => {
      this.handleMessage(evt.data);
    };

    this.ws.onerror = (err) => {
      this.error('WebSocket error:', err);
      this.stats.errors++;
    };

    this.ws.onclose = () => {
      this.connected = false;
      this.log('Disconnected from hot-reload server');

      if (this.autoReconnect) {
        this.log(`Reconnecting in ${this.reconnectDelay}ms...`);
        setTimeout(() => this.connect(), this.reconnectDelay);
      }
    };
  }

  /**
   * Handle incoming WebSocket messages.
   * @private
   */
  async handleMessage(data) {
    try {
      const msg = JSON.parse(data);

      switch (msg.type) {
        case 'patch':
          await this.loadPatch(msg.data, msg.symbols);
          break;

        case 'eval_result':
          this.log('Eval result:', msg.result);
          break;

        case 'error':
          this.error('Server error:', msg.error);
          this.stats.errors++;
          break;

        case 'handshake_ack':
          this.log('Handshake acknowledged by server');
          break;

        default:
          this.log('Unknown message type:', msg.type, msg);
      }
    } catch (err) {
      this.error('Failed to handle message:', err, data);
    }
  }

  /**
   * Load a WASM patch into the runtime.
   * @private
   */
  async loadPatch(patchBase64, symbols) {
    const startTime = performance.now();

    try {
      this.log('Loading patch...');

      // Convert base64 to Uint8Array
      const binaryString = atob(patchBase64);
      const bytes = new Uint8Array(binaryString.length);
      for (let i = 0; i < binaryString.length; i++) {
        bytes[i] = binaryString.charCodeAt(i);
      }

      // Write to virtual filesystem
      const patchPath = `/tmp/jank_patch_${this.patchCounter++}.wasm`;
      this.module.FS.writeFile(patchPath, bytes);

      this.log(`Patch written to ${patchPath} (${bytes.length} bytes)`);

      // Load via jank hot-reload API
      const pathPtr = this.module.stringToNewUTF8(patchPath);
      const result = this.module.ccall(
        'jank_hot_reload_load_patch',
        'number',
        ['string'],
        [patchPath]
      );
      this.module._free(pathPtr);

      const loadTime = performance.now() - startTime;

      if (result === 0) {
        this.stats.patchesLoaded++;
        this.stats.totalBytes += bytes.length;
        this.stats.lastPatchTime = loadTime;

        this.log(`✅ Patch loaded successfully! (${loadTime.toFixed(1)}ms)`);
        if (symbols && symbols.length > 0) {
          this.log('Updated symbols:', symbols.map(s => s.name || s).join(', '));
        }

        // Emit custom event for applications to listen to
        if (typeof window !== 'undefined') {
          window.dispatchEvent(new CustomEvent('jank-hot-reload', {
            detail: {
              symbols,
              bytes: bytes.length,
              loadTime
            }
          }));
        }
      } else {
        throw new Error(`jank_hot_reload_load_patch returned ${result}`);
      }
    } catch (err) {
      this.error('Failed to load patch:', err);
      this.stats.errors++;
    }
  }

  /**
   * Send an eval request to the server.
   * Code will be compiled to WASM and hot-reloaded.
   */
  eval(code) {
    const msg = {
      type: 'eval',
      code: code.trim(),
      timestamp: Date.now()
    };

    if (this.connected && this.ws.readyState === WebSocket.OPEN) {
      this.send(msg);
    } else {
      this.log('Not connected, queueing eval:', code);
      this.evalQueue.push(msg);
    }
  }

  /**
   * Send a message to the server.
   * @private
   */
  send(msg) {
    try {
      this.ws.send(JSON.stringify(msg));
    } catch (err) {
      this.error('Failed to send message:', err, msg);
    }
  }

  /**
   * Process queued eval requests.
   * @private
   */
  processQueue() {
    while (this.evalQueue.length > 0 && this.connected) {
      const msg = this.evalQueue.shift();
      this.send(msg);
    }
  }

  /**
   * Get statistics about hot-reload activity.
   */
  getStats() {
    // Get stats from C++ side
    let cppStats = null;
    try {
      const statsJson = this.module.ccall(
        'jank_hot_reload_get_stats',
        'string',
        [],
        []
      );
      cppStats = JSON.parse(statsJson);
    } catch (err) {
      this.log('Could not fetch C++ stats:', err);
    }

    return {
      client: this.stats,
      server: cppStats,
      connected: this.connected,
      serverUrl: this.serverUrl
    };
  }

  /**
   * Disconnect from the server.
   */
  disconnect() {
    this.autoReconnect = false;
    if (this.ws) {
      this.ws.close();
      this.ws = null;
    }
    this.connected = false;
    this.log('Disconnected');
  }

  /**
   * Log a message (if debug is enabled).
   * @private
   */
  log(...args) {
    if (this.debug || typeof window !== 'undefined' && window.JANK_HOT_RELOAD_DEBUG) {
      console.log('[jank-hot-reload]', ...args);
    }
  }

  /**
   * Log an error.
   * @private
   */
  error(...args) {
    console.error('[jank-hot-reload]', ...args);
  }
}

/**
 * Global convenience function for eval from devtools.
 * Usage: jankEval('(defn foo [x] (+ x 1))')
 */
if (typeof window !== 'undefined') {
  window.jankEval = function(code) {
    if (!window._jankHotReloadClient) {
      console.error('[jank-hot-reload] Client not initialized. Call initJankHotReload(Module) first.');
      return;
    }
    window._jankHotReloadClient.eval(code);
  };

  window.jankHotReloadStats = function() {
    if (!window._jankHotReloadClient) {
      console.error('[jank-hot-reload] Client not initialized.');
      return null;
    }
    const stats = window._jankHotReloadClient.getStats();
    console.table(stats.client);
    if (stats.server) {
      console.log('Server stats:', stats.server);
    }
    return stats;
  };
}

/**
 * Auto-initialize when jank WASM module loads (if in HOT_RELOAD mode).
 * This is called from the generated jank.js file.
 */
function initJankHotReload(Module, options = {}) {
  if (!Module || typeof Module.ccall !== 'function') {
    console.error('[jank-hot-reload] Invalid WASM module provided');
    return null;
  }

  // Check if hot-reload functions are available
  if (typeof Module.jank_hot_reload_load_patch !== 'function') {
    console.warn('[jank-hot-reload] Hot-reload functions not available. Build with HOT_RELOAD=1');
    return null;
  }

  console.log('[jank-hot-reload] Initializing hot-reload client...');

  const client = new JankHotReloadClient(Module, {
    debug: options.debug || false,
    serverUrl: options.serverUrl,
    autoReconnect: options.autoReconnect
  });

  // Store globally for devtools access
  if (typeof window !== 'undefined') {
    window._jankHotReloadClient = client;
  }

  // Auto-connect
  client.connect();

  console.log('[jank-hot-reload] Client initialized. Use jankEval("code") from devtools.');

  return client;
}

// Export for ES modules
if (typeof module !== 'undefined' && module.exports) {
  module.exports = {
    JankHotReloadClient,
    initJankHotReload
  };
}

// Export for browser globals
if (typeof window !== 'undefined') {
  window.JankHotReloadClient = JankHotReloadClient;
  window.initJankHotReload = initJankHotReload;
}

export { JankHotReloadClient, initJankHotReload };
