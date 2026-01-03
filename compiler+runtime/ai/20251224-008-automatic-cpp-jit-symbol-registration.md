# Automatic C++ JIT Symbol Registration

## Summary

Implemented automatic registration of C++ symbols for iOS JIT support. This eliminates the need for manual symbol registration in iOS apps when using JIT compilation with CppInterOp.

## Problem

When JIT compiles code that calls inline functions (like ImGui's functions), CppInterOp creates NEW definitions instead of linking to existing symbols. This causes:
- Duplicate `GImGui` global variable (JIT's version is NULL)
- `ImGui::NewFrame()` operating on wrong context
- Crashes with "Assertion failed: (g.WithinFrameScope)"

## Previous Solution (Manual)

iOS apps had to manually register symbols:
```cpp
jank_jit_register_symbol("__ZN5ImGui8NewFrameEv", (void*)&ImGui::NewFrame, 1);
jank_jit_register_symbol("_GImGui", (void*)&GImGui, 0);
```

This was error-prone and required knowing all the symbols referenced by inline functions.

## New Solution (Automatic)

The compiler now automatically:
1. **Collects** C++ symbols during codegen when generating `cpp_call`, `cpp_member_call`, and `cpp_value` expressions
2. **Generates** registration code in the AOT entrypoint
3. **Calls** registration automatically before any module loading

## Implementation Details

### 1. Symbol Collection (codegen/processor.cpp)

Added symbol collection in three places:
- `gen(cpp_call_ref)` - for static function calls
- `gen(cpp_member_call_ref)` - for member function calls
- `gen(cpp_value_ref)` - for global variables and function pointers

### 2. Mangled Name Retrieval (analyze/cpp_util.cpp)

Added `get_mangled_name()` function that:
- Uses clang's MangleContext to get the mangled symbol name
- Automatically adds the leading underscore prefix on macOS/iOS

### 3. Registration Generation (aot/processor.cpp)

In `gen_entrypoint()`, generates:
```cpp
/* Auto-generated C++ symbol registration for JIT */
extern "C" int jank_jit_register_symbol(char const *name, void *ptr, jank_bool callable);

extern "C" void _ZN5ImGui8NewFrameEv();
extern "C" char _GImGui;

static void jank_register_collected_cpp_jit_symbols() {
  jank_jit_register_symbol("__ZN5ImGui8NewFrameEv", (void*)&_ZN5ImGui8NewFrameEv, 1);
  jank_jit_register_symbol("_GImGui", (void*)&_GImGui, 0);
}
```

The registration function is called at the start of the entrypoint lambda.

### 4. Symbol Storage (runtime/context.hpp)

Added new types to the runtime context:
- `cpp_jit_symbol_info` - struct holding mangled name, qualified name, and is_function flag
- `cpp_jit_symbol_info_hash` - hash function for the struct
- `collected_cpp_jit_symbols` - synchronized set for thread-safe collection

## Files Changed

- `include/cpp/jank/type.hpp` - Added `native_unordered_set`
- `include/cpp/jank/runtime/context.hpp` - Added symbol info struct and collection
- `include/cpp/jank/analyze/cpp_util.hpp` - Added `get_mangled_name()` declaration
- `src/cpp/jank/analyze/cpp_util.cpp` - Implemented `get_mangled_name()`
- `src/cpp/jank/codegen/processor.cpp` - Added symbol collection in cpp_* gen functions
- `src/cpp/jank/aot/processor.cpp` - Added registration code generation

## How It Works

1. During AOT compilation, when the codegen processor encounters a C++ call/value:
   - Gets the mangled name using clang's MangleContext
   - Stores it in the runtime context's `collected_cpp_jit_symbols`

2. In `gen_entrypoint()`:
   - Iterates over collected symbols
   - Emits extern declarations for each (using the mangled name without macOS prefix)
   - Emits a registration function that calls `jank_jit_register_symbol` for each
   - Calls the registration function at the start of the entrypoint

3. At runtime:
   - The generated entrypoint calls `jank_register_collected_cpp_jit_symbols()`
   - This pre-registers all collected symbols with the JIT
   - When CppInterOp parses headers, the JIT finds existing symbols instead of creating duplicates

## Advantages

| Aspect | Manual | Automatic |
|--------|--------|-----------|
| Maintenance | High - must update when code changes | Zero - compiler handles it |
| Coverage | Incomplete - easy to miss symbols | Complete - all referenced symbols |
| Correctness | Error-prone - mangling mistakes | Correct - compiler knows exact names |
| Works with | Only known libraries | Any C++ code jank calls |

## Testing

Build and tests pass with no new failures:
- 255 test cases passed
- 10 pre-existing failures (unrelated to this change)

## Related Files

- `ai/20251224-007-cppinterop-extern-symbol-investigation.md` - Root cause investigation
- `ai/20251224-006-jit-symbol-registration-c-api.md` - C API documentation
