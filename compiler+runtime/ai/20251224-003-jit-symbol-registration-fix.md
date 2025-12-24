# JIT Symbol Registration Fix for C++ Namespaced Symbols

## Problem Summary

When JIT-compiled code calls C++ functions from AOT code (like ImGui), the JIT creates its own
copies of the symbols instead of linking to the existing AOT definitions. This causes:

1. Global variables like `ImGui::GImGui` to be NULL in JIT code
2. Function calls to call empty stubs instead of the actual functions
3. State not being shared between JIT and AOT code

## Root Cause

When the Clang JIT interpreter parses declarations like:
```cpp
namespace ImGui {
  extern ImGuiContext* GImGui;
  void NewFrame();
  bool Begin(const char* name);
}
```

It creates AST nodes for these declarations. For variables (`extern Context* GContext`), the JIT
correctly resolves the symbol if it's registered. However, for function declarations, Clang may
generate stub functions instead of linking to external definitions.

## Solution: Symbol Registration + Function Pointer Calls

### 1. Register Symbols with the JIT

The `processor::register_symbol()` function allows registering AOT symbols with the JIT:

```cpp
// Register a C++ mangled symbol with its address
jit_prc.register_symbol("_ZN5ImGui8NewFrameEv", (void*)&ImGui::NewFrame);
jit_prc.register_symbol("_ZN5ImGui7GImGuiE", (void*)&ImGui::GImGui);
```

Key implementation details:
- Uses `getGlobalPrefix()` to add the underscore prefix on macOS
- Registers to `ProcessSymbolsJITDylib` on Clang 17+ for correct resolution order
- Symbol names should be the mangled C++ names (get with `nm binary | grep SymbolName`)

### 2. Use Function Pointer Casts for Calls

Direct function calls to extern-declared functions don't work because Clang generates stubs.
Instead, use function pointer casts:

```cpp
// Instead of:
ImGui::NewFrame();  // May call a stub!

// Use:
((void(*)())((void*)&ImGui::NewFrame))();  // Correctly calls AOT function
```

Or with variables for clarity:
```cpp
{
  auto fn = (void(*)())((void*)&ImGui::NewFrame);
  fn();
}
```

## Test Results

The test suite demonstrates both behaviors:

1. **"JIT accesses C++ namespaced symbols - simulates ImGui exactly"** - FAILS
   - Demonstrates the bug: direct function calls don't work

2. **"JIT with symbol registration - FIX for C++ namespaced symbols"** - PASSES
   - Registers symbols before use
   - Uses function pointer casts for calls
   - Successfully calls AOT functions and modifies AOT state

## Getting Mangled Symbol Names

```bash
# Get mangled names from the binary
nm binary | grep ImGui

# Example output:
# 00000001002669bc T __ZN5ImGui8NewFrameEv     # ImGui::NewFrame()
# 00000001015bea90 S __ZN5ImGui7GImGuiE        # ImGui::GImGui
```

Note: `nm` shows names with the leading underscore (Mach-O format). When registering,
use the name without the extra underscore (the code adds it automatically).

## Files Modified

- `include/cpp/jank/jit/processor.hpp` - Added `register_symbol()` declaration
- `src/cpp/jank/jit/processor.cpp` - Added `register_symbol()` implementation
- `test/cpp/jank/jit/processor.cpp` - Added tests demonstrating the bug and fix

## Application to iOS JIT

For iOS JIT (ImGui use case):

1. At app initialization, register all needed ImGui symbols:
```cpp
jit_prc.register_symbol("_ZN5ImGui8NewFrameEv", (void*)&ImGui::NewFrame);
jit_prc.register_symbol("_ZN5ImGui5BeginEPKcPbi", (void*)&ImGui::Begin);
jit_prc.register_symbol("_ZN5ImGui7GImGuiE", (void*)&ImGui::GImGui);
// ... etc
```

2. In jank codegen, generate function pointer calls instead of direct calls when
   calling registered/external C++ functions:
```cpp
// Instead of generating: ImGui::NewFrame();
// Generate: ((void(*)())((void*)&ImGui::NewFrame))();
```

This ensures JIT-compiled jank code correctly calls into AOT-compiled ImGui.
