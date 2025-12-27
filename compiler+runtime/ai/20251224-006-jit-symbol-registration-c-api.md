# JIT Symbol Registration C API

## Summary

Added C API function `jank_jit_register_symbol` to allow iOS apps to register external symbols with the JIT, preventing duplicate definitions when inline functions are parsed.

## Problem

When JIT compiles code that calls inline functions (like ImGui's functions), CppInterOp creates NEW definitions instead of linking to existing symbols. This causes issues like:
- Duplicate `GImGui` global variable (JIT's version is NULL)
- `ImGui::NewFrame()` operating on wrong context
- Crashes with "Assertion failed: (g.WithinFrameScope)"

## Solution

Pre-register symbols with LLVM ORC JIT using `absoluteSymbols` before parsing headers. This tells the JIT "these symbols already exist, use them" instead of creating new definitions.

## API

### C API (c_api.h)
```c
/* Register an external symbol with the JIT.
 * name: mangled symbol name (e.g., "_ZN5ImGui8NewFrameEv")
 * ptr: pointer to the existing function/variable
 * callable: 1 for functions, 0 for data symbols
 * Returns: 0 on success, non-zero on failure */
int jank_jit_register_symbol(char const *name, void *ptr, jank_bool callable);
```

### C++ API (processor.hpp)
```cpp
jtl::string_result<void>
register_symbol(jtl::immutable_string const &name, void *ptr, bool callable = true) const;
```

## Usage in iOS App

Call after runtime context creation but before module loading:

```cpp
#include <jank/c_api.h>
#include "imgui.h"

// IMPORTANT: On macOS/iOS, C++ mangled names have DOUBLE underscore prefix!
// The mangled name _ZN... gets prefixed with _ to become __ZN...
// Use: nm -g <binary> | grep <symbol> to find exact names

extern ImGuiContext* GImGui;

void register_imgui_symbols_for_jit() {
    // Register globals (C-linkage has single underscore)
    jank_jit_register_symbol("_GImGui", (void*)&GImGui, 0);

    // Register C++ functions (mangled names have double underscore on macOS/iOS)
    jank_jit_register_symbol("__ZN5ImGui8NewFrameEv", (void*)&ImGui::NewFrame, 1);
    jank_jit_register_symbol("__ZN5ImGui6RenderEv", (void*)&ImGui::Render, 1);

    // ImGui::Begin - needs explicit cast for overloaded function
    using BeginFn = bool(*)(const char*, bool*, ImGuiWindowFlags);
    jank_jit_register_symbol("__ZN5ImGui5BeginEPKcPbi", (void*)static_cast<BeginFn>(&ImGui::Begin), 1);

    // Backend NewFrame functions
    jank_jit_register_symbol("__Z23ImGui_ImplSDL3_NewFramev", (void*)&ImGui_ImplSDL3_NewFrame, 1);
    jank_jit_register_symbol("__Z25ImGui_ImplVulkan_NewFramev", (void*)&ImGui_ImplVulkan_NewFrame, 1);
}
```

**CRITICAL**: The symbol names must match EXACTLY what `nm -g` shows.
- C-linkage globals: single underscore (`_GImGui`)
- C++ mangled names: double underscore (`__ZN5ImGui8NewFrameEv`)

## Implementation Details

- Uses LLVM ORC's `absoluteSymbols` to register symbols with the main JITDylib
- Symbols are registered with `Exported` flag and optionally `Callable` flag
- Error handling via `jtl::string_result` on C++ side, integer return on C side

## Files Changed

- `include/cpp/jank/jit/processor.hpp` - Added `register_symbol` method declaration
- `src/cpp/jank/jit/processor.cpp` - Added `register_symbol` implementation
- `include/cpp/jank/c_api.h` - Added `jank_jit_register_symbol` declaration
- `src/cpp/jank/c_api.cpp` - Added `jank_jit_register_symbol` implementation

## Root cause

The symbols have the same names. The issue isn't naming - it's duplicate definitions at different memory addresses.

  Here's what happens:

  AOT compilation (build time):
  ─────────────────────────────
  imgui.cpp compiles → creates GImGui at address 0x12345678
                     → creates ImGui::NewFrame() that uses 0x12345678

  JIT compilation (runtime):
  ──────────────────────────
  CppInterOp parses imgui.h
    → sees "extern ImGuiContext* GImGui;"
    → creates GImGui in JIT module at address 0x87654321 (NEW, initialized to NULL!)
    → sees inline functions that reference GImGui
    → generates IR that uses 0x87654321

  Both symbols are named "_GImGui", but they're different variables!

  When your redefined new-frame! calls ImGui::NewFrame():
  - JIT's NewFrame sets GImGui->WithinFrameScope = true at 0x87654321
  - AOT's ImGui::Begin() checks GImGui->WithinFrameScope at 0x12345678
  - The AOT GImGui was never touched → assertion fails

  The register_symbol("_GImGui", &GImGui) fix works because it tells the JIT linker: "When you see a reference to _GImGui, use address 0x12345678 (the AOT one), don't create a new one."

  The real question is: why does CppInterOp create a new definition instead of just declaring an external reference? That's the root cause we'd need to fix for a truly automatic solution.

## Testing

1. Build jank with the changes
2. Update iOS app to call `jank_jit_register_symbol` for ImGui symbols
3. Run app and redefine `new-frame!` via nREPL
4. App should not crash

### Verified Working (2025-12-24)

Test sequence:
```bash
# Start app
make ios-jit-sim-run

# After app starts, connect to nREPL
clj-nrepl-eval -p 5558 '(in-ns (quote vybe.sdf.ui))'

# Redefine new-frame!
cat /tmp/new-frame.jank | clj-nrepl-eval -p 5558
# => #'vybe.sdf.ui/new-frame!

# Verify app still running after several seconds
clj-nrepl-eval -p 5558 '(+ 100 200)'
# => 300  (app is alive, no crash!)
```

The fix was using **double underscores** for C++ mangled symbol names on macOS/iOS.

## Related Files

- `ai/20251224-005-ios-jit-inline-function-issue.md` - Root cause analysis
- `ai/20251224-003-jit-symbol-registration-fix.md` - Earlier investigation
