// Example: WebSocket Hot-Reload Client (Browser-side)
// This implements Step 3: WebSocket bridge
//
// Location in jank: public/hot-reload-client.js
//                   Or embed in generated HTML

class JankHotReloadClient {
  constructor(wasmModule, serverUrl = 'ws://localhost:7888/repl') {
    this.module = wasmModule;
    this.serverUrl = serverUrl;
    this.ws = null;
    this.evalQueue = [];
    this.patchCounter = 0;
  }

  connect() {
    console.log('[jank-hr] Connecting to hot-reload server:', this.serverUrl);

    this.ws = new WebSocket(this.serverUrl);

    this.ws.onopen = () => {
      console.log('[jank-hr] Connected to server!');
      this.processQueue();
    };

    this.ws.onmessage = (evt) => {
      this.handleMessage(evt.data);
    };

    this.ws.onerror = (err) => {
      console.error('[jank-hr] WebSocket error:', err);
    };

    this.ws.onclose = () => {
      console.log('[jank-hr] Disconnected from server');
      // Auto-reconnect after 2 seconds
      setTimeout(() => this.connect(), 2000);
    };
  }

  async handleMessage(data) {
    const msg = JSON.parse(data);

    switch (msg.type) {
      case 'patch':
        await this.loadPatch(msg.data, msg.symbols);
        break;

      case 'eval_result':
        console.log('[jank-hr] Eval result:', msg.result);
        break;

      case 'error':
        console.error('[jank-hr] Server error:', msg.error);
        break;

      default:
        console.warn('[jank-hr] Unknown message type:', msg.type);
    }
  }

  async loadPatch(patchBase64, symbols) {
    try {
      console.log('[jank-hr] Loading patch...');

      // Convert base64 to Uint8Array
      const binaryString = atob(patchBase64);
      const bytes = new Uint8Array(binaryString.length);
      for (let i = 0; i < binaryString.length; i++) {
        bytes[i] = binaryString.charCodeAt(i);
      }

      // Write to virtual filesystem
      const patchPath = `/tmp/patch_${this.patchCounter++}.wasm`;
      this.module.FS.writeFile(patchPath, bytes);

      console.log('[jank-hr] Patch written to:', patchPath, `(${bytes.length} bytes)`);

      // Load via jank runtime
      const pathPtr = this.module.stringToNewUTF8(patchPath);
      const result = this.module._jank_load_patch(pathPtr);
      this.module._free(pathPtr);

      if (result === 0) {
        console.log('[jank-hr] Patch loaded successfully!');
        if (symbols) {
          console.log('[jank-hr] Updated symbols:', symbols);
        }
      } else {
        console.error('[jank-hr] Failed to load patch (error code:', result, ')');
      }
    } catch (err) {
      console.error('[jank-hr] Error loading patch:', err);
    }
  }

  eval(code) {
    const msg = {
      type: 'eval',
      code: code,
      timestamp: Date.now()
    };

    if (this.ws && this.ws.readyState === WebSocket.OPEN) {
      this.ws.send(JSON.stringify(msg));
    } else {
      console.warn('[jank-hr] Not connected, queueing eval');
      this.evalQueue.push(msg);
    }
  }

  processQueue() {
    while (this.evalQueue.length > 0 && this.ws.readyState === WebSocket.OPEN) {
      const msg = this.evalQueue.shift();
      this.ws.send(JSON.stringify(msg));
    }
  }

  disconnect() {
    if (this.ws) {
      this.ws.close();
      this.ws = null;
    }
  }
}

// USAGE in jank HTML:
//
// <script type="module">
//   import createJankModule from './jank.js';
//
//   const Module = await createJankModule();
//   const hotReload = new JankHotReloadClient(Module);
//   hotReload.connect();
//
//   // Now you can eval code from devtools or UI:
//   hotReload.eval('(defn ggg [v] (+ v 49))');
// </script>

// Make it globally available for devtools
if (typeof window !== 'undefined') {
  window.JankHotReloadClient = JankHotReloadClient;
}

export default JankHotReloadClient;
