# iOS JIT-Only Mode: Remote Eval Fix Plan

## The Problem

On iOS JIT-only mode, `eval` and `native-source` fail when C++ interop is involved:

```clojure
;; Works (direct REPL input - routed to compile server)
(sdfx/engine_initialized)

;; FAILS (eval runs locally on iOS, no headers)
(eval '(sdfx/engine_initialized))

;; FAILS (native-source runs locally on iOS, no headers)
(jc/native-source '(sdfx/engine_initialized))
```

## Root Cause Analysis

### How iOS JIT-Only Mode Works Currently

1. **Direct REPL input**: `(sdfx/engine_initialized)`
   - iOS nREPL receives string
   - Sends to compile server (port 5570)
   - Compile server analyzes (HAS headers via CppInterOp)
   - Generates C++ code with `sdfx::engine_initialized()`
   - Cross-compiles to ARM64 object
   - Sends back to iOS
   - iOS loads and executes
   - **WORKS** ✓

2. **eval call**: `(eval '(sdfx/engine_initialized))`
   - iOS nREPL receives string
   - Sends to compile server
   - Compile server compiles THE CALL to `eval` (not the inner form!)
   - Generated code: `eval(quote(sdfx/engine_initialized))`
   - Sends ARM64 object back to iOS
   - iOS loads and executes
   - `eval` function runs **LOCALLY on iOS**
   - Local `eval` tries to analyze `(sdfx/engine_initialized)`
   - Local CppInterOp has NO headers
   - **FAILS** ✗

### The Key Insight

The `eval` function does runtime compilation. On iOS JIT-only mode, ANY runtime compilation must go through the compile server because:
- iOS doesn't have C++ headers bundled (too large)
- CppInterOp on iOS can't resolve C++ scopes
- Only the compile server has headers loaded

This affects:
- `eval`
- `native-source` (and all `jank.compiler-native` functions)
- `load-file` (when loading .jank files at runtime)
- Dynamic `require` beyond what's AOT-compiled

## Solution Architecture

### Option A: Hook at JIT/Eval Level (Recommended)

Modify the core evaluation path to detect remote compile mode and delegate:

```
┌─────────────────────────────────────────────────────────────┐
│                     iOS App (JIT-Only)                       │
├─────────────────────────────────────────────────────────────┤
│  nREPL Server                                                │
│    │                                                         │
│    ▼                                                         │
│  eval(form)                                                  │
│    │                                                         │
│    ├── if remote_compile_enabled?                           │
│    │     │                                                   │
│    │     ▼                                                   │
│    │   send_to_compile_server(form)  ──────────────────────┼──► Compile Server
│    │     │                                                   │      │
│    │     ▼                                                   │      ▼
│    │   receive_object_code() ◄─────────────────────────────┼─── Cross-compile
│    │     │                                                   │
│    │     ▼                                                   │
│    │   load_and_execute()                                   │
│    │                                                         │
│    └── else (normal JIT mode)                               │
│          │                                                   │
│          ▼                                                   │
│        local_jit_compile()                                  │
└─────────────────────────────────────────────────────────────┘
```

### Where to Hook

Looking at jank's architecture:

1. **`jank/evaluate.cpp`** - Contains `eval` implementation
2. **`jank/jit/processor.cpp`** - JIT compilation
3. **`jank/runtime/context.cpp`** - Runtime context with compile server settings

The cleanest hook point is in `evaluate.cpp` where `eval` is implemented.

## Implementation Plan

### Step 1: Add Remote Eval Support to Runtime Context

File: `include/cpp/jank/runtime/context.hpp`

```cpp
struct context {
  // ... existing members ...

  // Remote compile server settings (already exist for nREPL routing)
  std::string remote_compile_host;
  uint16_t remote_compile_port{0};
  bool remote_compile_enabled{false};

  // Connection to compile server (reuse existing)
  std::unique_ptr<compile_server::client> compile_client;
};
```

### Step 2: Add "eval" Operation to Compile Server Protocol

File: `include/cpp/jank/compile_server/protocol.hpp`

```cpp
// Eval request - execute form and return result
struct eval_request
{
  int64_t id{0};
  std::string code;      // The form to evaluate (as string)
  std::string ns;        // Namespace context
};

struct eval_response
{
  int64_t id{0};
  bool success{false};

  // On success:
  std::string result;           // Printed result
  std::vector<uint8_t> object;  // Compiled object (if expression returns value)
  std::string entry_symbol;

  // On error:
  std::string error;
};
```

### Step 3: Handle "eval" Op in Compile Server

File: `include/cpp/jank/compile_server/server.hpp`

Add handler in `handle_message()`:
```cpp
else if(op == "eval")
{
  auto code = get_json_string(msg, "code");
  auto ns = get_json_string(msg, "ns");
  return eval_code(id, code, ns.empty() ? "user" : ns);
}
```

Add `eval_code()` function:
```cpp
std::string eval_code(int64_t id, std::string const &code, std::string const &ns)
{
  // Similar to compile_code but:
  // 1. Analyze the form
  // 2. Generate code
  // 3. Cross-compile to ARM64
  // 4. Return object + entry symbol
  // The iOS side will load and execute
}
```

### Step 4: Modify eval() to Use Remote Compile

File: `src/cpp/jank/evaluate.cpp`

```cpp
object_ref eval(object_ref const form)
{
  // Check if remote compile is enabled
  if(__rt_ctx->remote_compile_enabled && __rt_ctx->compile_client)
  {
    return remote_eval(form);
  }

  // Original local eval path
  return local_eval(form);
}

object_ref remote_eval(object_ref const form)
{
  // 1. Serialize form to string
  auto const code = runtime::to_string(form);
  auto const ns = __rt_ctx->current_ns()->to_string();

  // 2. Send to compile server
  auto const response = __rt_ctx->compile_client->eval(code, ns);

  // 3. Load returned object code
  if(response.success)
  {
    load_object(response.object, response.entry_symbol);
    // Call the entry function to get result
    return call_entry(response.entry_symbol);
  }
  else
  {
    throw_eval_error(response.error);
  }
}
```

### Step 5: Fix native-source Similarly

File: `src/cpp/jank/compiler_native.cpp`

```cpp
static object_ref native_source(object_ref const form)
{
  // Check if remote compile is needed (when C++ interop might be involved)
  if(__rt_ctx->remote_compile_enabled && __rt_ctx->compile_client)
  {
    return remote_native_source(form);
  }

  // Original local path
  return with_pipeline(form, ...);
}
```

## Alternative: Simpler Approach

Instead of modifying `eval`, we could:

1. Keep `eval` as-is (it will fail on C++ interop)
2. Document that on iOS JIT-only mode, C++ interop only works via direct REPL input
3. Add a `remote-eval` function for cases where you need runtime eval with C++ interop

This is simpler but less seamless.

## Testing Plan

1. Test `(eval '(+ 1 2))` - should work (no C++ interop)
2. Test `(eval '(sdfx/engine_initialized))` - should work after fix
3. Test `(jc/native-source '(sdfx/engine_initialized))` - should work after fix
4. Test nested eval: `(eval '(eval '(sdfx/engine_initialized)))`
5. Test `load-file` with C++ interop code

## Files to Modify

1. `include/cpp/jank/compile_server/protocol.hpp` - Add eval protocol structs
2. `include/cpp/jank/compile_server/server.hpp` - Add eval handler
3. `src/cpp/jank/evaluate.cpp` - Add remote eval path
4. `src/cpp/jank/compiler_native.cpp` - Add remote native-source path
5. `include/cpp/jank/runtime/context.hpp` - Ensure remote compile state is accessible

## Questions to Resolve

1. How is the compile server client currently connected? Need to find existing code.
2. Is there already a "compile and return object" path we can reuse?
3. How does the iOS app currently route nREPL input to compile server? Can we reuse that?

## Next Steps

1. Find existing compile server client code in iOS app
2. Understand current nREPL -> compile server routing
3. Implement eval op in compile server
4. Hook eval() to use remote compile when enabled
5. Test on iOS simulator
