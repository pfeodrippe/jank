# iOS JIT Inline Function Symbol Duplication Issue

## Problem Summary

When redefining functions via nREPL that call C++ inline functions (like ImGui), the app crashes with:
```
Assertion failed: (g.WithinFrameScope), function Begin, file imgui.cpp, line 7303.
```

## Root Cause

Even with `-Wl,-export_dynamic`, inline functions in headers cause CppInterOp to create **new definitions** instead of linking to existing symbols.

### How It Happens

1. JIT parses `imgui.h` header
2. CppInterOp sees `inline void NewFrame()` and creates NEW IR code
3. This new code references `GImGui` - but creates a NEW `GImGui` variable (NULL)
4. JIT's `ImGui::NewFrame()` sets `WithinFrameScope = true` on NULL/new GImGui
5. AOT code's `ImGui::Begin()` uses the REAL `GImGui` - which never had NewFrame called
6. Assertion fails!

### Symbol Export Verification

All symbols ARE properly exported in the dylib:
```
0000000000018da4 T __ZN5ImGui8NewFrameEv        # ImGui::NewFrame
00000000068e2f40 S _GImGui                       # GImGui context
0000000000088ecc T __Z23ImGui_ImplSDL3_NewFramev
000000000008dbfc T __Z25ImGui_ImplVulkan_NewFramev
```

The `T` (text section) and `S` (data section) markers confirm they're exported. But CppInterOp still creates new definitions for inline functions.

## Affected Functions

- `sdfx::engine_initialized()` - inline function
- `ImGui::NewFrame()` - defined in imgui.cpp but accessed via header
- `ImGui_ImplVulkan_NewFrame()` - may have inline components
- `ImGui_ImplSDL3_NewFrame()` - may have inline components
- Any function that accesses global state like `GImGui`

## Solutions

### Solution 1: Register Function Pointers (Recommended)

Create a registration mechanism in jank that tells CppInterOp to use existing symbols:

```cpp
// In iOS app initialization
void register_imgui_for_jit() {
    jank_jit_register_symbol("_ZN5ImGui8NewFrameEv", (void*)&ImGui::NewFrame);
    jank_jit_register_symbol("GImGui", (void*)&GImGui);
    // ... etc
}
```

This bypasses header parsing for these specific symbols.

### Solution 2: Avoid Redefining Functions That Call ImGui

For now, don't redefine functions that call ImGui via nREPL. Only modify pure jank logic.

### Solution 3: Use Native Wrappers

Create non-inline C wrapper functions:

```cpp
// In sdf_engine_impl.cpp (not header)
extern "C" void sdfx_new_frame() {
    if (sdfx::engine_initialized()) {
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
    }
}
```

Then call this wrapper from jank instead of directly calling inline functions.

### Solution 4: Mark Specific Functions as AOT-Only

In the module system, mark certain functions as "cannot be redefined at runtime":

```clojure
(defn ^:aot-only new-frame! ...)
```

## Test Command

To reproduce:
```bash
# Start app
make ios-jit-sim-run

# After app starts, eval this via nREPL
clj-nrepl-eval -p 5558 "$(cat /tmp/new-frame.jank)"
# App crashes immediately
```

## Related Files

- `/Users/pfeodrippe/dev/something/vulkan/sdf_engine.hpp` - Contains inline functions
- `/Users/pfeodrippe/dev/something/SdfViewerMobile/project-jit-sim.yml` - Has -export_dynamic
- `/Users/pfeodrippe/dev/jank/compiler+runtime/ai/20251224-002-jit-aot-imgui-crash-investigation.md` - Initial investigation
