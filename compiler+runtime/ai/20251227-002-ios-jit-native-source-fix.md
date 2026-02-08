# iOS JIT-Only Mode: native-source Fix

## Problem

On iOS JIT-only mode, `jank.compiler-native/native-source` failed when C++ interop symbols were involved:

```clojure
;; FAILED with: "Unable to find 'sdfx' within namespace ''"
(jc/native-source '(when (sdfx/engine_initialized) :foo))
```

## Root Cause

The `native-source` function in `compiler_native.cpp` was always doing local analysis, even on iOS JIT-only mode. On iOS, CppInterOp doesn't have C++ headers loaded, so it can't resolve C++ scopes like `sdfx`.

## Solution

Added remote compile support for `native-source`:

1. **Added `native_source_response` struct to `protocol.hpp`** - Protocol struct for native-source operation

2. **Added `native_source()` method to `client.hpp`** - Client method to send native-source requests to compile server

3. **Added `native-source` op handler to `server.hpp`** - Server handler that analyzes code with proper namespace bindings and generates C++ source

4. **Added `remote_native_source()` to `remote_compile.hpp`** - Convenience function similar to `remote_compile()`

5. **Modified `native_source()` in `compiler_native.cpp`** - Check for remote compile mode at the beginning and delegate to compile server if enabled

## Key Code Changes

### compiler_native.cpp
```cpp
static object_ref native_source(object_ref const form)
{
#ifdef JANK_IOS_JIT
    if(compile_server::is_remote_compile_enabled())
    {
      auto const code = runtime::to_code_string(form);
      auto const current_ns = __rt_ctx->current_ns();
      auto const ns_str = std::string(current_ns->name->name.data(), current_ns->name->name.size());
      auto const response = compile_server::remote_native_source(code, ns_str);
      if(response.success)
      {
        auto formatted(util::format_cpp_source(response.source).expect_ok());
        forward_string(std::string_view{ formatted.data(), formatted.size() });
      }
      else
      {
        throw std::runtime_error("Remote native-source failed: " + response.error);
      }
      return jank_nil;
    }
#endif
    // ... existing local path ...
}
```

## Testing

```clojure
;; Simple expression - works
(jc/native-source '(+ 1 2))

;; C++ interop - NOW WORKS
(jc/native-source '(sdfx/engine_initialized))
;; Generates: auto &&vybe_sdf_ui_cpp_call_976{ ::sdfx::engine_initialized() };

;; Complex form with C++ interop - NOW WORKS
(jc/native-source '(when (sdfx/engine_initialized) :foo))
;; Generates proper if statement with ::sdfx::engine_initialized() call
```

## Related Changes

This fix builds on the earlier `eval` fix (see `20251227-001-ios-jit-remote-eval-fix.md`) which added remote compile support to `context::eval(object_ref)`.

## Files Modified

- `include/cpp/jank/compile_server/protocol.hpp` - Added `native_source_response` struct
- `include/cpp/jank/compile_server/client.hpp` - Added `native_source()` method
- `include/cpp/jank/compile_server/server.hpp` - Added `native_source_code()` and handler
- `include/cpp/jank/compile_server/remote_compile.hpp` - Added `remote_native_source()`
- `src/cpp/jank/compiler_native.cpp` - Added remote compile check to `native_source()`
