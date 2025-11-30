# jank WASM Hot-Reload Integration Plan

**Goal:** Enable REPL-like hot-reload for jank running in WebAssembly

**Status:** In Progress

---

## 4-Step Integration Plan

### Step 1: Rebuild jank with MAIN_MODULE support ✅

**What:** Rebuild the jank WASM bundle with dynamic linking support

**Why:** MAIN_MODULE enables `dlopen()` which is required to load function patches at runtime

**Changes to:** `compiler+runtime/bin/emscripten-bundle`

**Implementation:**
- Added `HOT_RELOAD=1` environment variable mode
- Automatically enables `-sMAIN_MODULE=2` (optimized dynamic linking)
- Adds `-fPIC` to all compiled objects
- Enables required runtime methods: `FS`, `stringToNewUTF8`, `ccall`, `cwrap`

**Usage:**
```bash
# Build with hot-reload support
HOT_RELOAD=1 ./bin/emscripten-bundle your_code.jank

# Or for just the runtime
HOT_RELOAD=1 ./bin/emscripten-bundle
```

**Flags added:**
- `-sMAIN_MODULE=2` - Optimized dynamic linking
- `-sALLOW_TABLE_GROWTH=1` - Allow adding functions to indirect call table
- `-sEXPORTED_RUNTIME_METHODS=FS,stringToNewUTF8,ccall,cwrap,UTF8ToString`
- `-sFILESYSTEM=1` - Required for dlopen to work
- `-fPIC` - Position-independent code for all compiled objects
- `-sEXPORT_ALL=1` - Export all symbols for dlsym

**Impact:** Bundle size increases from ~60MB to ~100-150MB (acceptable for dev mode)

**Status:** ✅ Complete (Nov 27, 2025)

---

### Step 2: Implement var registry ⏳

**What:** Replace direct function calls with indirect calls through function pointers

**Why:** Allows hot-swapping function implementations without restarting the WASM module

**Implementation:**

```cpp
// In jank runtime (analyze.cpp or runtime.hpp)

namespace jank::runtime {

  // Var registry: maps symbol -> function pointer
  struct var_entry {
    void* fn_ptr;          // Current implementation
    std::string signature; // e.g. "ii" for int(int)
  };

  std::unordered_map<std::string, var_entry> var_registry;

  // Register a new function
  void register_var(const std::string& name, void* fn_ptr, const std::string& sig) {
    var_registry[name] = {fn_ptr, sig};
  }

  // Get function pointer for calling
  void* get_var(const std::string& name) {
    auto it = var_registry.find(name);
    return it != var_registry.end() ? it->second.fn_ptr : nullptr;
  }

  // Load a side module and register its symbols
  int load_patch(const std::string& module_path) {
    void* handle = dlopen(module_path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!handle) return -1;

    // Extract symbols and register them
    // (Implementation depends on patch metadata format)

    return 0;
  }
}
```

**Code generation changes:**

Before (direct call):
```cpp
// Generated code for: (ggg 10)
jank_user_ggg(10);
```

After (indirect call through var registry):
```cpp
// Generated code for: (ggg 10)
auto fn = (int(*)(int))jank::runtime::get_var("user/ggg");
fn(10);
```

**Status:** Not started

---

### Step 3: WebSocket bridge for nREPL ⏳

**What:** Establish bidirectional communication between native jank nREPL and browser

**Architecture:**

```
┌─────────────────────────────────────────────────────────────┐
│                      Browser (WASM)                          │
│  ┌────────────────────────────────────────────────────────┐  │
│  │  WebSocket Client                                      │  │
│  │    ws://localhost:7888/repl                            │  │
│  │                                                        │  │
│  │  Messages:                                             │  │
│  │    → { type: "eval", code: "(defn ggg [v] (+ v 49))" }│  │
│  │    ← { type: "patch", data: <Uint8Array> }            │  │
│  └────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
                           ▲
                           │ WebSocket
                           ▼
┌─────────────────────────────────────────────────────────────┐
│                  Native jank nREPL Server                    │
│  ┌────────────────────────────────────────────────────────┐  │
│  │  WebSocket Server (port 7888)                          │  │
│  │                                                        │  │
│  │  On eval request:                                      │  │
│  │    1. Parse code                                       │  │
│  │    2. Compile to C++                                   │  │
│  │    3. emcc to WASM side module (~180ms)               │  │
│  │    4. Send patch over WebSocket                        │  │
│  └────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

**JavaScript client (browser):**
```javascript
// hot-reload-client.js
class JankReplClient {
  constructor() {
    this.ws = new WebSocket('ws://localhost:7888/repl');
    this.ws.onmessage = (evt) => this.handleMessage(evt);
  }

  eval(code) {
    this.ws.send(JSON.stringify({ type: 'eval', code }));
  }

  async handleMessage(evt) {
    const msg = JSON.parse(evt.data);

    if (msg.type === 'patch') {
      // Write patch to virtual FS
      const patchData = new Uint8Array(msg.data);
      const patchPath = `/tmp/patch_${Date.now()}.wasm`;
      Module.FS.writeFile(patchPath, patchData);

      // Load the patch
      const result = Module._load_patch(
        Module.stringToNewUTF8(patchPath)
      );

      console.log(result === 0 ? 'Patch loaded!' : 'Patch failed');
    }
  }
}

// Initialize
const repl = new JankReplClient();
```

**Native server (C++ in jank nREPL):**
- Use library: `websocketpp` or `uWebSockets`
- Listen on port 7888
- On eval message: compile → send binary patch

**Status:** Not started

---

### Step 4: Server-side compilation ⏳

**What:** Generate and compile side modules on the native server

**Implementation:**

```cpp
// In jank nREPL eval handler
namespace jank::nrepl {

  std::vector<uint8_t> compile_to_wasm_patch(const std::string& code) {
    // 1. Parse jank code
    auto expr = jank::read::parse(code);

    // 2. Compile to C++
    auto cpp_code = jank::codegen::generate(expr);

    // 3. Write to temp file
    std::string temp_cpp = "/tmp/patch_" + generate_uuid() + ".cpp";
    std::ofstream(temp_cpp) << cpp_code;

    // 4. Compile with emcc
    std::string temp_wasm = "/tmp/patch_" + generate_uuid() + ".wasm";
    std::string cmd = "emcc " + temp_cpp + " -o " + temp_wasm +
                      " -sSIDE_MODULE=1 -O2 -fPIC";
    system(cmd.c_str());

    // 5. Read binary
    std::ifstream wasm_file(temp_wasm, std::ios::binary);
    return std::vector<uint8_t>(
      std::istreambuf_iterator<char>(wasm_file),
      std::istreambuf_iterator<char>()
    );
  }

  void handle_eval_request(const std::string& code, WebSocketConnection* conn) {
    auto patch_data = compile_to_wasm_patch(code);

    // Send patch to browser
    nlohmann::json response = {
      {"type", "patch"},
      {"data", patch_data}
    };
    conn->send(response.dump());
  }
}
```

**Optimization opportunities:**
- Cache emcc process (keep alive between evals)
- Incremental compilation (only recompile changed functions)
- Parallel compilation for multiple patches

**Status:** Not started

---

## Current Status Summary

| Step | Status | Files Changed | Estimated Time |
|------|--------|---------------|----------------|
| 1. MAIN_MODULE rebuild | ⏳ Not started | bin/emscripten-bundle, CMakeLists.txt | 2-3 hours |
| 2. Var registry | ⏳ Not started | analyze.cpp, runtime.hpp, codegen | 4-5 hours |
| 3. WebSocket bridge | ⏳ Not started | nrepl server, new JS client | 3-4 hours |
| 4. Server compilation | ⏳ Not started | nrepl eval handler | 2-3 hours |

**Total estimated:** 11-15 hours

---

## Testing Plan

1. **Unit test:** Var registry registration and lookup
2. **Integration test:** Load a single-function patch
3. **E2E test:** Edit code in browser → eval → hot-reload
4. **Performance test:** Measure total eval-to-execution time

---

## Next Actions

- [x] Create proof of concept (hot-reload-test/)
- [ ] Update bin/emscripten-bundle with MAIN_MODULE flags
- [ ] Implement var registry in jank runtime
- [ ] Add WebSocket server to nREPL
- [ ] Implement server-side patch compilation
- [ ] Create browser demo page
- [ ] Write integration tests

---

*Last Updated: Nov 27, 2025*
