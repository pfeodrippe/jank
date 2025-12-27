# iOS AOT C++ JIT Symbol Registration

## Problem

When redefining functions via nREPL on iOS that call C++ inline functions (like ImGui), the app crashes with:
```
Assertion failed: (g.WithinFrameScope), function Begin, file imgui.cpp, line 7303
```

The root cause is that CppInterOp creates NEW definitions for inline functions instead of linking to existing symbols that were compiled into the AOT bundle.

For example, when ImGui functions like `ImGui::NewFrame()` are called from jank code:
1. During AOT compilation, the ImGui inline functions get compiled into the iOS binary
2. When code is evaluated via nREPL, CppInterOp parses the ImGui headers again
3. CppInterOp creates new definitions of the inline functions with their own static variables
4. This causes issues like `g.WithinFrameScope` being different between AOT and JIT code

## Solution Attempts

### Attempt 1: dlsym-based Registration

Tried to automatically collect C++ function symbols during AOT compilation and generate registration code using `dlsym(RTLD_DEFAULT, ...)` to find symbol addresses.

**Result: FAILED**

Reason: ImGui functions are NOT inline but are internal symbols. On iOS, the linker strips all non-exported symbols, so `dlsym` returns NULL for ImGui functions.

### Attempt 2: Address-of Operator Registration

Changed to use `(void*)&ImGui::NewFrame` instead of dlsym. The idea was that taking the address of a function forces the compiler to emit a symbol.

**Result: FAILED**

Reason: While taking the address creates an undefined reference in the object file, those references get resolved at link time to internal symbols. The final binary still doesn't export the symbols, so:
1. The symbols exist in the binary (code is there)
2. But they're not visible to dlsym
3. And the JIT still creates its own definitions

### The Core Problem

The fundamental issue is deeper than just symbol visibility:

1. When CppInterOp parses `imgui.h`, it creates its OWN `GImGui` variable (the global ImGui context pointer)
2. When the JIT compiles code that calls `ImGui::NewFrame()`, it uses CppInterOp's `GImGui`
3. But the AOT code uses the ORIGINAL `GImGui` from the compiled imgui.cpp
4. So `ImGui::NewFrame()` sets `g.WithinFrameScope = true` on the WRONG context
5. When `ImGui::Begin()` is called, it checks the AOT's `GImGui` which is still false

Even if we successfully register the function symbols, CppInterOp's parsed code would still reference the wrong context variable.

## Current Implementation (Partial)

Modified `codegen/processor.cpp` to collect C++ symbols during wasm_aot compilation with filters:
- Skip `jank::` namespace (already in libjank.a)
- Skip `clojure::` namespace (already in libjank.a)
- Skip `sdfx::` namespace (has overloaded functions)
- Skip `sdf_` prefix (macros for SDL)
- Skip `*_jank` suffix (helper functions)
- Skip `_G__` pattern (generated internal functions)
- Skip `operator` functions (overloaded)

Registration code uses address-of:
```cpp
jank_jit_register_symbol("__ZN5ImGui8NewFrameEv", (void*)&ImGui::NewFrame, 1);
```

## Potential Real Solutions

### Option 1: Export ImGui Symbols

Force ImGui symbols to be exported with `__attribute__((visibility("default")))`:
- Modify ImGui compilation to export all symbols
- Would require changes to the Xcode project's OTHER_CFLAGS
- Would increase binary size

### Option 2: Prevent CppInterOp from Parsing Headers

Tell CppInterOp that certain headers are "already compiled" and shouldn't create new definitions:
- Would require CppInterOp modifications
- Complex to implement correctly

### Option 3: Share ImGui Context

Register not just function symbols but also the GImGui variable:
- After CppInterOp parses imgui.h, replace its GImGui with the AOT GImGui
- Complex because GImGui might be used during parsing

### Option 4: Pre-compile Headers for JIT

Use the same PCH for JIT as was used for AOT:
- Would ensure identical symbol definitions
- Requires careful PCH management

## Files Modified

- `src/cpp/jank/codegen/processor.cpp` - Symbol collection in gen() functions
- `src/cpp/jank/runtime/context.cpp` - Registration code generation
- `bin/ios-bundle` - Integration with AOT init

## Technical Notes

### Symbol Filtering

The filters skip namespaces/patterns that cause compilation issues:
1. **jank::/clojure::** - Functions in libjank.a, dlsym can find them
2. **sdfx::** - Has overloaded functions, can't take address without cast
3. **operator** - Overloaded by definition
4. **_G__** - Generated anonymous functions
5. **sdf_/\_jank** - Project-specific helper functions

### C++ Name Mangling

On Apple platforms, C++ mangled names use double underscore: `__ZN5ImGui8NewFrameEv`

### JIT Symbol Registration

Uses LLVM ORC JIT's `absoluteSymbols` to pre-define symbols:
```cpp
llvm::orc::SymbolMap symbols;
symbols[es.intern(name)] = { llvm::orc::ExecutorAddr::fromPtr(ptr), flags };
ee->getMainJITDylib().define(llvm::orc::absoluteSymbols(symbols));
```

## Status

**NOT WORKING** - Symbol registration is implemented but doesn't solve the core problem of CppInterOp creating its own definitions and context variables.
