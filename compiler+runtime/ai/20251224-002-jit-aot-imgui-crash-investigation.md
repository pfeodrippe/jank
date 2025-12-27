# JIT vs AOT ImGui Crash Investigation

## Crash Summary

When evaluating `new-frame!` via nREPL (JIT), the app crashes with:
```
ImGui::Begin(char const*, bool*, int) + 252 (imgui.cpp:7303)
```

The call from JIT code (`0x1689e5eec` - outside main binary range `0x10d020000-0x11355ffff`) into ImGui fails an assertion.

## Stack Trace Analysis

```
0  __pthread_kill + 8
1  pthread_kill + 256
2  abort + 104
3  __assert_rtn + 268                          ← Assertion failed!
4  ImGui::Begin() + 252 (imgui.cpp:7303)       ← ImGui assertion
5  ??? 0x1689e5eec                              ← JIT-compiled code (new-frame!)
6  jit_function::call()                         ← jank calling JIT function
7-9 dynamic_call chain
10 vybe::sdf::ios::vybe_sdf_ios_draw_517::call() ← AOT draw function
...
```

## Key Observations

### 1. The Mystery Address `0x1689e5eec`
- Main dylib: `0x10d020000 - 0x11355ffff` (~100MB)
- JIT code at: `0x1689e5eec` (~1.5GB higher!)
- This is JIT-compiled code living in dynamically allocated memory
- The JIT code successfully CALLS into ImGui, but ImGui's internal state is wrong

### 2. ImGui Assertion at Line 7303
Looking at imgui.cpp, line ~7303 is in `ImGui::Begin()`:
```cpp
IM_ASSERT(g.WithinFrameScope && "Forgot to call ImGui::NewFrame()!");
```

This means when JIT's `new-frame!` calls `ImGui::NewFrame()`, either:
- The ImGui context `GImGui` (aka `g`) is NULL or different
- Or `WithinFrameScope` is false because `NewFrame()` wasn't called yet

### 3. The Second Error: `cpp/unbox` Type Inference
```
Unable to infer type for 'cpp/unbox'. The value must be a var with a known pointer type,
or you must specify the type explicitly: (cpp/unbox type value).
```

This error in `draw-debug-ui!` suggests the JIT compiler cannot determine C++ pointer types that were known at AOT compile time.

---

## Deep Investigation Findings

### Finding 1: JIT Symbol Resolution Mechanism

Looking at `jit/processor.cpp:479-497`, the JIT uses:
1. **Primary**: `interpreter->getSymbolAddress(name)` - searches JIT symbol table
2. **iOS Fallback**: `dlsym(RTLD_DEFAULT, name)` - searches main executable

```cpp
jtl::string_result<void *> processor::find_symbol(jtl::immutable_string const &name) const
{
  if(auto symbol{ interpreter->getSymbolAddress(name.c_str()) })
  {
    return symbol.get().toPtr<void *>();
  }

#ifdef JANK_IOS_JIT
  // Fallback: search main executable and loaded libraries
  if(void *sym = dlsym(RTLD_DEFAULT, name.c_str()))
  {
    return sym;
  }
#endif

  return err(util::format("Failed to find symbol: '{}'", name));
}
```

**Problem**: `dlsym` finds symbols only if they are **exported**. ImGui is compiled as source files directly into the app - its symbols may not be in the dynamic symbol table!

### Finding 2: How C++ Calls are Generated

In `codegen/processor.cpp:1668-1756`, when generating code for `(imgui/NewFrame)`:

1. The analyzer resolves `imgui/NewFrame` to a `cpp_value` with a Clang scope
2. Codegen emits: `ImGui::NewFrame();` as C++ text
3. The CppInterOp interpreter parses and compiles this C++ text
4. At link time, the JIT needs to find `ImGui::NewFrame` symbol

**Problem**: If the symbol is not exported, the JIT will fail to link or might create a NEW definition!

### Finding 3: `GImGui` Global Context Issue

ImGui defines in `imgui.cpp:1383-1385`:
```cpp
#ifndef GImGui
ImGuiContext*   GImGui = NULL;
#endif
```

When the JIT parses `imgui.h` and sees `extern ImGuiContext* GImGui;`:
- CppInterOp/Clang creates an IR declaration for `GImGui`
- At link time, this must resolve to the EXISTING `GImGui` in the main binary
- If `GImGui` is not in the dynamic symbol table, the JIT might:
  1. Create a NEW `GImGui` with value NULL
  2. Or fail to link entirely

**Result**: JIT code calls `ImGui::NewFrame()` but it operates on NULL or a different `GImGui`, so `WithinFrameScope` is never set. Then `ImGui::Begin()` crashes with the assertion.

### Finding 4: `cpp/unbox` Type Inference Failure

In `analyze/processor.cpp:5068-5088`:

```cpp
/* Get the unbox target type from var_deref's tag_type or call's return_tag_type. */
jtl::ptr<void> inferred_type{ nullptr };
if(value_expr->kind == expression_kind::var_deref)
{
  auto const var_deref_expr{ llvm::cast<expr::var_deref>(value_expr.data) };
  inferred_type = var_deref_expr->tag_type;  // ← FROM var_deref
}
```

The `tag_type` comes from `var->boxed_type` which is set when the var is defined:

```cpp
// In def handling (processor.cpp:1475-1478)
if(original_value_expr->kind == expression_kind::cpp_box)
{
  auto const box_expr{ llvm::cast<expr::cpp_box>(original_value_expr.data) };
  var.expect_ok()->boxed_type = box_expr->boxed_type;  // ← Clang type pointer!
}
```

**Problem**: `boxed_type` is a `jtl::ptr<void>` which is actually a Clang `QualType` pointer from the AOT compilation phase. This type pointer is:
- Created during AOT by the AOT Clang instance
- Stored on the var object
- Not valid in the JIT Clang instance (different AST, different memory)

When JIT code tries to use `(cpp/unbox some-var)`:
1. The JIT analyzer reads `var->boxed_type`
2. This is a STALE pointer to AOT's Clang AST
3. Trying to use it with JIT's Clang results in undefined behavior or NULL

---

## Root Cause Summary

### Issue 1: ImGui Symbols Not Exported for JIT

ImGui is compiled as source files directly into the iOS app:
```yaml
# project-jit-sim.yml
sources:
  - path: ../vendor/imgui/imgui.cpp
  - path: ../vendor/imgui/imgui_draw.cpp
  # ... etc
```

Unlike `libjank.a` which uses `-force_load`, ImGui symbols are NOT guaranteed to be in the dynamic symbol table. When JIT tries to resolve `ImGui::NewFrame` or `GImGui`, it may:
1. Fail to find them entirely
2. Create new definitions instead of linking to existing ones

### Issue 2: Clang Type Pointers Not Portable Between Compiler Instances

The `var->boxed_type` mechanism stores Clang `QualType` pointers:
- These are raw pointers into Clang's AST memory
- Each Clang interpreter instance has its own AST
- AOT Clang types cannot be dereferenced by JIT Clang

---

## Solutions

### Solution A: Export All Symbols (Quick Fix)

Add linker flag to export all symbols:
```yaml
OTHER_LDFLAGS:
  - "-Wl,-export_dynamic"  # Export all dynamic symbols
```

Or specifically export ImGui symbols using an export list.

**Pros**: Quick, works for all symbols
**Cons**: Larger binary, exposes internal symbols

### Solution B: Register Native Function Pointers

Create a registration mechanism at startup:
```cpp
// In app initialization
extern "C" void jank_register_imgui_symbols() {
    jank_register_native_fn("_ZN5ImGui8NewFrameEv", (void*)&ImGui::NewFrame);
    jank_register_native_fn("_ZN5ImGui5BeginEPKcPbiE", (void*)&ImGui::Begin);
    jank_register_native_fn("GImGui", (void*)&GImGui);
    // ... etc
}
```

**Pros**: Precise control, minimal overhead
**Cons**: Need to register every symbol, mangled names are fragile

### Solution C: Force Load ImGui as Static Library

Compile ImGui as a static library and use `-force_load`:
```yaml
OTHER_LDFLAGS:
  - "-Wl,-force_load,$(PROJECT_DIR)/build/libimgui.a"
```

**Pros**: All symbols exported, mirrors libjank.a approach
**Cons**: Build system changes needed

### Solution D: Fix `cpp/unbox` Type Portability

For `cpp/unbox`, don't rely on Clang type pointers. Instead:

1. **Store type as string**: Instead of `boxed_type` being a `QualType*`, store the type name string
2. **Re-resolve at JIT time**: When JIT encounters `cpp/unbox`, look up the type by name in JIT's Clang
3. **Use `:tag` metadata**: Already supported - users can annotate vars with `:tag`

```clojure
;; Current (broken in JIT):
(defonce *mesh-scale (u/v->p 1.0 "float"))
(cpp/unbox *mesh-scale)  ; Fails - can't infer type

;; Workaround:
(cpp/unbox #cpp "float*" *mesh-scale)  ; Explicit type
```

**Implementation**:
```cpp
// In var.hpp, change:
jtl::ptr<void> boxed_type;  // Clang type pointer (not portable!)

// To:
jtl::immutable_string boxed_type_name;  // Type name string (portable!)
```

Then in JIT analysis, re-resolve the type name to a Clang type.

---

## Recommended Fix Order

1. **Immediate (workaround)**: Use explicit types in `cpp/unbox` for JIT-eval'd code
2. **Short-term**: Add `-Wl,-export_dynamic` to export all symbols
3. **Medium-term**: Build ImGui as static lib with `-force_load`
4. **Long-term**: Fix `boxed_type` to use type name strings instead of Clang pointers

---

## Test Commands

```bash
# Check if ImGui symbols are exported
nm -g SdfViewerMobile-JIT.app/SdfViewerMobile-JIT | grep -E "(GImGui|NewFrame)"

# Check if dlsym can find them
# Add debug logging in jit/processor.cpp:find_symbol()
```

## Files to Modify

1. **`project-jit-sim.yml`** - Add symbol export flags
2. **`jit/processor.cpp`** - Add logging for symbol resolution debugging
3. **`runtime/var.hpp`** - Change `boxed_type` to string-based storage
4. **`analyze/processor.cpp`** - Update `cpp/unbox` to re-resolve types in JIT mode
