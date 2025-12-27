# iOS JIT: Compiler Integration with Remote Compile Server

## The Problem

The current remote JIT implementation has a fundamental flaw:

```
[compile-server] Error (id=3, type=compile): analyze/unresolved-symbol: Unable to resolve symbol 'sdfx/engine_initialized'.
```

**Why this happens:**
1. iOS app has AOT-compiled namespaces (vybe.sdf.ui, etc.)
2. When nREPL evals `(defn new-frame ...)`, iOS sends code to macOS compile server
3. macOS compile server only has `clojure.core` loaded - it doesn't know about `sdfx`, `imgui-vk`, etc.
4. Analysis fails because macOS can't resolve app-specific symbols

**The root cause:** We're mixing AOT (on iOS) with remote JIT (on macOS), but they have different namespace contexts.

## The Correct Architecture

**Key Insight:** The iOS JIT build should NOT have AOT app code. Instead, the jank compiler on iOS should delegate ALL compilation to the macOS compile server.

### Flow for `(require 'vybe.sdf.ui)`:

```
iOS                                    macOS Compile Server
────                                   ────────────────────
1. (require 'vybe.sdf.ui)
   │
2. Find source file for vybe.sdf.ui
   │
3. Read source code ──────────────────► 4. Receive source code
                                        │
                                        5. Analyze (build namespace context)
                                        │
                                        6. Generate C++ code
                                        │
                                        7. Cross-compile to ARM64 .o
                                        │
8. Receive object file ◄─────────────── 9. Send object file (base64)
   │
9. Load .o via ORC JIT addObjectFile()
   │
10. Execute module init (intern vars, etc.)
```

### Why This Works

- **Both iOS and macOS have the same namespace context** - macOS loads namespaces as iOS requests them
- **macOS can resolve all symbols** - because it has analyzed all the same code
- **No AOT duplication** - iOS only has runtime + clojure.core, everything else is JIT-loaded
- **Single source of truth** - macOS compile server manages all compilation state

## Implementation Plan

### Phase 1: Protocol Extension

Extend the compile server protocol to support namespace loading:

```json
// Request: Load namespace
{
  "op": "require",
  "id": 1,
  "ns": "vybe.sdf.ui",
  "source": "(ns vybe.sdf.ui ...)\n(defn new-frame! [] ...)"
}

// Response: Compiled namespace
{
  "op": "required",
  "id": 1,
  "modules": [
    {
      "name": "vybe.sdf.ui$loading__",
      "symbol": "_vybe_sdf_ui__loading____0",
      "object": "base64-encoded-object-file"
    }
  ]
}
```

### Phase 2: Compiler Hook on iOS

Modify jank's compiler on iOS to delegate to the compile server:

**File: `src/cpp/jank/jit/processor.cpp`** (or equivalent)

When `compilation_target::module` is requested on iOS:
1. Check if remote compile client is available
2. If yes, send source to compile server instead of using CppInterOp
3. Receive object file(s) and load via ORC JIT

```cpp
// Pseudocode
object_ref jit_processor::compile_module(native_persistent_string const &ns_name,
                                          native_persistent_string const &source)
{
  if(compile_client_ && compile_client_->is_connected())
  {
    // Remote compilation path
    auto result = compile_client_->require_ns(ns_name, source);
    if(result.success)
    {
      for(auto const &module : result.modules)
      {
        load_object_file(module.object_data, module.entry_symbol);
      }
      return /* ns object */;
    }
    // Fall back to local compilation on error?
  }

  // Local compilation path (existing code)
  // ...
}
```

### Phase 3: Namespace Resolution

The compile server needs to know where to find source files:

**Option A: iOS sends source**
- iOS reads source files and sends them to macOS
- Simpler protocol, no filesystem access needed on macOS
- iOS controls what gets compiled

**Option B: Shared source path**
- macOS has access to the same source directory (via network mount, etc.)
- Compile server resolves and reads sources itself
- More like a traditional build setup

**Recommendation: Option A** - iOS sends source code. This keeps the compile server stateless regarding the filesystem.

### Phase 4: iOS Build Configuration

Create a new iOS build mode: **JIT-only** (no AOT app code)

```bash
# Build iOS runtime + clojure.core only
./bin/ios-bundle --jit-only simulator

# This produces:
# - libjank.a (runtime)
# - libclojure_core_aot.a (clojure.core AOT)
# - NO libvybe_aot.a (app namespaces)
```

The Xcode project would link only the runtime, not the app AOT library.

### Phase 5: Startup Sequence

When iOS app starts in JIT mode:

1. Initialize jank runtime
2. Load AOT clojure.core (for bootstrap)
3. Connect to macOS compile server
4. App calls `(require 'vybe.sdf.ios)` which:
   - Reads source from bundle
   - Sends to compile server
   - Loads returned object files
5. App namespace is now loaded and ready

## Critical Implementation Details

### 1. Module Dependencies

When compiling `vybe.sdf.ui`, it may `(require 'vybe.sdf.shader)`. The compile server needs to:
- Track which namespaces are already loaded
- Request missing namespace sources from iOS
- Or: iOS pre-sends all transitive dependencies

**Protocol for dependency request:**
```json
// Server to client: Need more source
{
  "op": "need-source",
  "id": 1,
  "ns": "vybe.sdf.shader"
}

// Client response
{
  "op": "source",
  "id": 1,
  "ns": "vybe.sdf.shader",
  "source": "..."
}
```

### 2. State Synchronization

The compile server becomes stateful - it accumulates namespace definitions. Need to handle:
- Server restart (iOS needs to re-send all namespaces)
- Multiple iOS clients (each gets separate session/context)
- Var redefinitions during REPL development

### 3. Native Interop

Code like `(imgui/NewFrame)` calls native C++ functions. These are:
- Declared in headers (iOS has headers bundled)
- Implemented in native libs linked into iOS app

The compile server needs:
- Same headers for C++ generation
- Knowledge of native function signatures
- But NOT the native implementations (those run on iOS)

### 4. Multiple Object Files per Namespace

A namespace might compile to multiple object files:
- One for the `ns` form / loading code
- One per `defn` (for incremental recompilation)
- Or one monolithic file per namespace

**Recommendation:** Start with one object file per namespace, optimize later.

## Files to Modify

1. **`include/cpp/jank/compile_server/protocol.hpp`**
   - Add `require` operation
   - Add `need-source` response type

2. **`include/cpp/jank/compile_server/server.hpp`**
   - Add `require_ns()` method
   - Track loaded namespaces
   - Handle dependency resolution

3. **`include/cpp/jank/compile_server/client.hpp`**
   - Add `require_ns()` client method
   - Handle `need-source` requests

4. **`src/cpp/jank/jit/processor.cpp`** (or equivalent)
   - Add remote compilation path
   - Integrate compile client

5. **`bin/ios-bundle`**
   - Add `--jit-only` mode
   - Skip AOT compilation of app namespaces

6. **iOS Xcode project**
   - Create JIT-only build configuration
   - Link only runtime, not app AOT

## Testing Strategy

1. **Unit test:** Compile server handles `require` op correctly
2. **Integration test:** iOS can load a simple namespace via remote compile
3. **Full test:** iOS app boots with JIT-only, loads all namespaces remotely
4. **REPL test:** Can eval `defn` forms after remote namespace loading

## Open Questions

1. **Clojure.core AOT or JIT?**
   - AOT is faster startup
   - JIT would mean even clojure.core goes through compile server
   - Recommendation: Keep clojure.core as AOT for now

2. **Error recovery?**
   - If compile server dies mid-session, can iOS recover?
   - Maybe: cache compiled objects locally for restart

3. **Performance?**
   - Network latency for each namespace load
   - Could batch multiple namespaces
   - Could pre-compile frequently used namespaces

4. **Development workflow?**
   - How to iterate on native code that JIT code calls?
   - May need to rebuild iOS app for native changes, but JIT for Clojure changes

## Summary

The key insight is: **iOS should not have AOT app code in JIT mode.** Instead:

1. iOS runtime connects to macOS compile server at startup
2. ALL namespace loading goes through the compile server
3. Both sides build up the same namespace context
4. REPL eval works because macOS has full symbol knowledge

This is a significant architectural change but creates a clean separation:
- **macOS:** Compilation (analysis, codegen, cross-compile)
- **iOS:** Execution (load objects, run code)
