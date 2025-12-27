# iOS JIT: Shared PCH Solution Analysis

Date: 2025-12-25

## Executive Summary

**Question**: Would using shared PCH (Pre-compiled Headers) that include native headers like imgui.h solve the iOS JIT crash problem entirely?

**Answer**: **NO - PCH alone is NOT sufficient.** A combined approach is required:
1. Shared PCH with native headers (prevents re-parsing)
2. Symbol registration for critical global variables (ensures JIT uses AOT symbols)

---

## Problem Statement

When redefining functions via nREPL on iOS that call C++ inline functions (like ImGui), the app crashes with:
```
Assertion failed: (g.WithinFrameScope), function Begin, file imgui.cpp, line 7303
```

### Root Cause Chain

1. **jank code calls ImGui::Begin()** which is an inline function in imgui.h
2. **ImGui::Begin() references `GImGui`** - a global pointer to the ImGui context
3. **GImGui is declared extern** in imgui_internal.h:
   ```cpp
   extern IMGUI_API ImGuiContext* GImGui;  // Current implicit context pointer
   ```
4. **GImGui is defined** in imgui.cpp:
   ```cpp
   ImGuiContext* GImGui = NULL;
   ```
5. **When JIT compiles code that uses GImGui:**
   - CppInterOp parses imgui_internal.h
   - Sees the extern declaration
   - JIT needs to resolve the symbol
   - Symbol is NOT exported (iOS strips internal symbols)
   - **JIT creates a NEW definition with its own NULL pointer!**
6. **Result**: JIT's GImGui != AOT's GImGui = crash

---

## Why PCH Alone Does NOT Solve the Problem

### What PCH Contains

A Pre-compiled Header (PCH) is a binary snapshot of the compiler's state after parsing headers. It contains:
- Parsed AST (Abstract Syntax Tree)
- Type definitions
- Function declarations
- Variable declarations (including `extern`)
- Macro definitions
- Template instantiations

### What PCH Does NOT Contain

- **Variable definitions** (the actual memory allocation)
- **Symbol addresses** (resolved at link time, not compile time)
- **Runtime state** (values of variables)

### The Symbol Resolution Problem

1. **With PCH loaded:**
   ```
   Clang's view: "extern ImGuiContext* GImGui" - declared somewhere
   ORC JIT's view: Symbol "GImGui" - needs address to link
   ```

2. **When JIT links code that uses GImGui:**
   - ORC JIT looks up "GImGui" in its symbol table → NOT FOUND
   - ORC JIT falls back to dlsym(RTLD_DEFAULT, "GImGui") → NOT FOUND (stripped)
   - **Clang generates a tentative/weak definition or inline duplication**

3. **Even with native headers in PCH:**
   - PCH prevents RE-PARSING of imgui.h
   - But doesn't solve the SYMBOL RESOLUTION problem
   - JIT still can't find the AOT's GImGui address

### The Inline Function Problem

The inline functions in imgui_internal.h directly reference GImGui:
```cpp
inline ImGuiWindow* GetCurrentWindowRead() {
    ImGuiContext& g = *GImGui;  // Uses GImGui directly!
    return g.CurrentWindow;
}
```

When these inline functions are used:
1. Clang inlines them into the calling code
2. Each inline expansion needs the GImGui symbol
3. If GImGui isn't registered, each expansion uses a different (JIT-created) GImGui

---

## The Combined Solution

### Part 1: Shared PCH with Native Headers

**Purpose**: Prevent CppInterOp from re-parsing headers at runtime

**Benefits**:
- Faster JIT startup (no header parsing)
- Consistent type definitions between AOT and JIT
- Required foundation for symbol registration

**Implementation**:
```bash
# Build PCH that includes BOTH jank headers AND native headers
$LLVM_CLANG \
  -target arm64-apple-ios17.0 \
  -isysroot "$IOS_SDK" \
  -std=gnu++20 \
  -Xclang -fincremental-extensions \
  -Xclang -emit-pch \
  -I/path/to/imgui \
  -I/path/to/other/native/headers \
  -o combined.pch \
  -c combined_prelude.hpp
```

Where `combined_prelude.hpp` includes:
```cpp
#include <jank/prelude.hpp>
#include "imgui.h"
#include "imgui_internal.h"
// ... other native headers used by jank code
```

### Part 2: Symbol Registration for Critical Variables

**Purpose**: Tell ORC JIT where AOT symbols are located

**Critical Symbols to Register**:
1. **GImGui** - The ImGui global context pointer
2. Any other global state used by inline functions

**Implementation**:
```cpp
// In iOS app initialization, BEFORE any JIT code runs:
extern "C" ImGuiContext* GImGui;  // From imgui.cpp

void register_imgui_symbols_for_jit() {
    // Register the GImGui variable symbol
    // Note: C-linkage symbols have single underscore on Apple platforms
    jank_jit_register_symbol("_GImGui", (void*)&GImGui, 0);  // 0 = data symbol
}
```

**How it works**:
```
                        ORC JIT Symbol Table
                        ┌─────────────────────┐
                        │ _GImGui → 0x12345678│ ← Registered
                        │ ...                 │
                        └─────────────────────┘
                                  ↓
When JIT compiles:              Lookup
  *GImGui ─────────────────────────→ Found! Use 0x12345678
                                        ↓
                           Points to AOT's GImGui ✓
```

---

## Why Both Parts Are Necessary

| Scenario | PCH Only | Symbol Reg Only | PCH + Symbol Reg |
|----------|----------|-----------------|------------------|
| Header parsing | Skipped | Re-parsed | Skipped |
| Type consistency | Good | May differ | Good |
| GImGui resolution | FAILS | Works | Works |
| Performance | Good | Slow | Good |
| Maintenance | Simple | Complex | Moderate |

### PCH without Symbol Registration = FAILURE
- Headers aren't re-parsed (good)
- But GImGui symbol still unresolved (crash)

### Symbol Registration without PCH = WORKS but FRAGILE
- GImGui is registered (good)
- But headers are re-parsed at runtime
- Risk of type mismatches between AOT and JIT
- Slower startup
- Each inline function expansion still uses the registered symbol

### Combined = ROBUST
- No re-parsing
- Consistent types
- Correct symbol resolution
- Fast startup

---

## Implementation Plan

### Phase 1: Extend PCH to Include Native Headers

1. **Create combined prelude header**
   ```
   Location: compiler+runtime/include/cpp/jank/ios_prelude.hpp
   ```

   Contents:
   ```cpp
   #pragma once
   #include <jank/prelude.hpp>

   // Native headers that jank code uses
   #ifdef JANK_IOS_NATIVE_IMGUI
   #include "imgui.h"
   #include "imgui_internal.h"
   #endif
   ```

2. **Update PCH build script**
   ```bash
   # In bin/ios-bundle or build-ios-pch.sh
   $LLVM_CLANG \
     -target arm64-apple-ios17.0 \
     -isysroot "$IOS_SDK" \
     -DJANK_IOS_NATIVE_IMGUI=1 \
     -I${APP_HEADERS}/include \  # imgui.h location
     ... existing flags ...
     -o jank-resources/incremental.pch \
     -c jank/ios_prelude.hpp
   ```

3. **Make PCH include paths configurable**
   - Allow apps to specify additional include paths for PCH
   - Example: `-DJANK_PCH_EXTRA_INCLUDES="/path/to/headers"`

### Phase 2: Automatic Symbol Collection During AOT

1. **During `wasm_aot` compilation**:
   - Collect all C++ extern variables referenced by jank code
   - Focus on global variables (not functions initially)
   - Store in `runtime::context::collected_cpp_jit_symbols`

2. **Filter rules**:
   - Skip jank:: and clojure:: namespaces (in libjank.a)
   - Skip internal/generated symbols
   - Focus on `extern` variables from native headers

3. **Generated code**:
   ```cpp
   // Auto-generated in entrypoint
   extern "C" char _GImGui;  // Use char for generic address

   static void jank_register_cpp_symbols() {
       jank_jit_register_symbol("_GImGui", (void*)&_GImGui, 0);
   }
   ```

### Phase 3: iOS App Integration

1. **In jank_aot_init()**:
   - Call the generated registration function
   - This happens BEFORE any JIT code runs

2. **Order of initialization**:
   ```
   1. Load libjank.a (AOT)
   2. Initialize runtime context
   3. Register C++ symbols for JIT  ← NEW
   4. Load PCH (if using JIT)
   5. Run jank code
   ```

---

## Alternative Approaches Considered

### Option A: Export All ImGui Symbols
- **Approach**: Compile ImGui with `-fvisibility=default`
- **Pro**: dlsym would find symbols
- **Con**: Bloats binary, exports internals

### Option B: Modify CppInterOp
- **Approach**: Patch CppInterOp to not create definitions for extern symbols
- **Pro**: Most correct fix
- **Con**: Upstream dependency, complex

### Option C: Use ImGui's GetCurrentContext()
- **Approach**: Never access GImGui directly; use accessor
- **Pro**: Avoids the issue
- **Con**: Inline functions still reference GImGui internally

### Option D: Shared PCH + Symbol Registration (CHOSEN)
- **Approach**: Combined solution as described above
- **Pro**: Works with any library, no upstream changes needed
- **Con**: Requires per-app configuration of which symbols to register

---

## Files to Modify

1. **NEW**: `include/cpp/jank/ios_prelude.hpp` - Combined prelude for PCH
2. **MODIFY**: `bin/ios-bundle` - Update PCH build to include native headers
3. **MODIFY**: `src/cpp/jank/codegen/processor.cpp` - Collect extern variable symbols
4. **MODIFY**: `src/cpp/jank/aot/processor.cpp` - Generate registration code
5. **VERIFY**: `jank_jit_register_symbol` API exists (from previous work)

---

## Verification Test Plan

1. Build iOS app with shared PCH including imgui.h
2. Register GImGui symbol in app initialization
3. Start app, connect nREPL
4. Evaluate:
   ```clojure
   (defn new-frame! []
     (imgui/NewFrame))

   (new-frame!)  ; Should not crash!
   ```
5. Redefine function:
   ```clojure
   (defn new-frame! []
     (println "Before NewFrame")
     (imgui/NewFrame))

   (new-frame!)  ; Should still not crash!
   ```

---

## Technical Deep Dive: Why Inline Functions Break

When Clang compiles:
```cpp
ImGui::Begin("Window", nullptr, 0);
```

The inline function `Begin` uses:
```cpp
inline bool Begin(const char* name, bool* p_open = NULL, ImGuiWindowFlags flags = 0) {
    ImGuiContext& g = *GImGui;  // ← Problem here!
    // ...
}
```

**Without symbol registration:**
1. Clang sees `*GImGui`
2. Looks for GImGui symbol → not found
3. Creates weak/tentative definition: `ImGuiContext* GImGui = NULL`
4. Code uses THIS new NULL pointer
5. `g.WithinFrameScope` is checked on uninitialized memory
6. Crash!

**With symbol registration:**
1. Before any JIT code runs: `jank_jit_register_symbol("_GImGui", &GImGui, 0)`
2. ORC JIT now has: `_GImGui → 0x12345678` (AOT's GImGui)
3. Clang sees `*GImGui`
4. ORC JIT looks up → FOUND
5. Uses AOT's GImGui which points to the real context
6. No crash!

---

## Conclusion

Shared PCH is a **necessary but not sufficient** condition for solving the iOS JIT inline function problem. The complete solution requires:

1. **Shared PCH with native headers** - Ensures type consistency, prevents re-parsing
2. **Symbol registration** - Ensures JIT uses AOT's global variables

This combined approach is robust, maintainable, and doesn't require upstream changes to CppInterOp or ImGui.

---

## References

- `ai/20251222-007-ios-jit-pch-implementation.md` - Original PCH implementation
- `ai/20251224-001-ios-aot-cpp-jit-symbol-registration.md` - Previous symbol registration attempts
- `ai/20251224-006-jit-symbol-registration-c-api.md` - JIT symbol registration API
- `ai/20251224-007-cppinterop-extern-symbol-investigation.md` - CppInterOp behavior analysis
