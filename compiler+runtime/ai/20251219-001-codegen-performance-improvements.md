# Codegen Performance Improvements Analysis

**Date**: 2025-12-19
**Context**: Analysis of jank-to-C++ codegen for ImGui UI code
**Files Analyzed**: form-jank.txt (jank source), form-cpp.txt (generated C++)

## Executive Summary

After analyzing a ~130-line jank ImGui UI form and its ~1000+ line C++ output, I've identified several significant opportunities for codegen optimization. The current codegen is correct but generates substantial overhead that could be eliminated with targeted improvements.

---

## Key Observations

### 1. Excessive Boxing/Unboxing for Primitive Operations

**Problem**: Simple arithmetic and comparisons go through dynamic dispatch even when types are known.

**Example - The `>` Comparison**:
```jank
(> (sdfx/get_mesh_preview_triangle_count) 0)
```

**Generated C++**:
```cpp
auto &&vybe_sdf_ui_cpp_call_4902{ sdfx::get_mesh_preview_triangle_count() };
auto const vybe_sdf_ui_cpp_cast_4901{
  jank::runtime::convert<unsigned int>::into_object(vybe_sdf_ui_cpp_call_4902)
};
auto const vybe_sdf_ui_call_4900(
  jank::runtime::dynamic_call(vybe_sdf_ui__GT__4760->deref(),
                              vybe_sdf_ui_cpp_cast_4901,
                              vybe_sdf_ui_const_4761));
```

**Optimal C++**:
```cpp
auto result = sdfx::get_mesh_preview_triangle_count() > 0;
```

**Impact**:
- `into_object()` allocates a boxed integer
- `dynamic_call()` does virtual dispatch + argument boxing
- Var deref adds indirection
- This pattern repeats for `<`, `-`, `+`, `count`, etc.

### 2. Constant Float/Int Conversions at Runtime

**Problem**: Compile-time constants are stored as boxed objects and converted at runtime.

**Example**:
```jank
(imgui-h/ImVec2. (cpp/float. 10.0) (cpp/float. 10.0))
```

**Generated C++**:
```cpp
float vybe_sdf_ui_cpp_ctor_4889{ jank::runtime::convert<float>::from_object(
  vybe_sdf_ui_const_4753.get()) };
float vybe_sdf_ui_cpp_ctor_4890{ jank::runtime::convert<float>::from_object(
  vybe_sdf_ui_const_4753.get()) };
ImVec2 vybe_sdf_ui_cpp_ctor_4888{ vybe_sdf_ui_cpp_ctor_4889,
                                  vybe_sdf_ui_cpp_ctor_4890 };
```

**Optimal C++**:
```cpp
ImVec2 vybe_sdf_ui_cpp_ctor_4888{ 10.0f, 10.0f };
```

**Impact**: Every `(cpp/float. X)` with a literal adds:
- A heap-allocated `obj::real` at struct construction
- Runtime conversion via `from_object()`

### 3. Redundant Nil References

**Problem**: Unused return values are explicitly assigned to nil objects.

**Pattern everywhere**:
```cpp
ImGui::Separator();
jank::runtime::object_ref const vybe_sdf_ui_cpp_call_4954{
  jank::runtime::jank_nil
};
```

**Impact**: Creates unused variables that the C++ optimizer must eliminate.

### 4. `and` Macro Expansion Creates Deep Nesting

**Problem**: The `and` form expands to deeply nested if-else chains.

**Source**:
```jank
(and mesh-visible mesh-solid mesh-initialized mesh-has-indices)
```

**Generated**: ~50 lines of nested if/else with redundant bool->object conversions!

**Each condition does**:
```cpp
auto const vybe_sdf_ui_cpp_cast_4905{
  jank::runtime::convert<bool>::into_object(vybe_sdf_ui_vybe_sdf_ui_G___4763_4764)
};
if(jank::runtime::truthy(vybe_sdf_ui_cpp_cast_4905))
```

**Optimal**:
```cpp
bool compute_skipped = mesh_visible && mesh_solid && mesh_initialized && mesh_has_indices;
```

### 5. Dynamic `not` Calls for Boolean Negation

**Problem**: `(when-not x ...)` generates dynamic var lookup + call.

**Source**:
```jank
(when-not mesh-visible (imgui/Text "  - mesh not visible"))
```

**Generated**:
```cpp
auto const vybe_sdf_ui_cpp_cast_4933{
  jank::runtime::convert<bool>::into_object(vybe_sdf_ui_mesh_visible_4757)
};
auto const vybe_sdf_ui_call_4932(
  jank::runtime::dynamic_call(vybe_sdf_ui_not___4775->deref(),
                              vybe_sdf_ui_cpp_cast_4933));
if(jank::runtime::truthy(vybe_sdf_ui_call_4932))
```

**Optimal**:
```cpp
if(!mesh_visible)
```

### 6. `cond` with `:else` Generates Unnecessary Truthy Check

**Source**:
```jank
(cond
  (< res 64) 8
  (< res 128) 16
  (< res 256) 32
  :else 64)
```

**Generated** (for :else):
```cpp
if(jank::runtime::truthy(vybe_sdf_ui_const_4824))  // keyword :else
{
  vybe_sdf_ui_if_5024 = vybe_sdf_ui_const_4818;  // 64
}
else
{
  vybe_sdf_ui_if_5024 = vybe_sdf_ui_const_4777;  // nil
}
```

**Optimal**: Just `else { return 64; }` - keywords are always truthy.

### 7. Keyword Lookup via Dynamic Call

**Problem**: `(:distance cam)` uses dynamic_call on keywords.

**Generated**:
```cpp
auto const vybe_sdf_ui_call_4960(
  jank::runtime::dynamic_call(vybe_sdf_ui_const_4785, vybe_sdf_ui_cam_4746));
```

**Better**: Direct `get()` on the map if cam type is known.

### 8. Var Deref Chain for Core Functions

**Problem**: Every core function call does `var->deref()`.

**Pattern**:
```cpp
jank::runtime::dynamic_call(vybe_sdf_ui_count_4802->deref(), ...)
```

This happens for: `count`, `+`, `-`, `<`, `>`, `not`, `println`, etc.

---

## Proposed Optimizations

### Priority 1: Primitive Type Inference & Direct Operations

**Goal**: When types are statically known (C++ interop returns, literals), emit direct operations.

| Operation | Current | Proposed |
|-----------|---------|----------|
| `(> cpp-int 0)` | dynamic_call + boxing | `cpp_int > 0` |
| `(- res step)` | dynamic_call + boxing | `res - step` |
| `(count objs)` | dynamic_call | Direct if vector type known |
| `(cpp/float. 1.0)` | box then unbox | `1.0f` |

**Implementation Hints**:
- Track C++ types through the analyzer
- When both operands are C++ primitives, emit direct C++ operators
- `cpp/int.`, `cpp/float.` with literals can be compile-time constants

### Priority 2: Boolean Operations Without Boxing

**Goal**: `and`, `or`, `not`, `when-not` on C++ bools should emit direct C++.

```jank
;; Current: dynamic_call(not_var, into_object(bool))
;; Proposed: !bool_value directly
(when-not mesh-visible ...)
```

**Implementation**:
- Add special handling in `if_expr` codegen for C++ bool conditions
- Recognize `and`/`or` at the semantic level, not just macro expansion
- `(not x)` on C++ bool -> `!x`

### Priority 3: Constant Folding & Hoisting

**Goal**: Literal conversions at compile time, not runtime.

```cpp
// Before
float f{ convert<float>::from_object(const_10_point_0.get()) };

// After
constexpr float f = 10.0f;
```

**Implementation**:
- When `cpp/float.`, `cpp/int.`, `cpp/bool.` take literals, emit C++ literals directly
- Pre-compute `ImVec2(10.0f, 10.0f)` etc.

### Priority 4: Eliminate Unused Result Assignments

**Goal**: Void-returning C++ calls don't need result variables.

```cpp
// Before
ImGui::Separator();
jank::runtime::object_ref const vybe_sdf_ui_cpp_call_4954{ jank::runtime::jank_nil };

// After
ImGui::Separator();
```

**Implementation**:
- Track whether expression result is used
- Don't emit result binding for statement-position expressions

### Priority 5: Core Function Inlining

**Goal**: Inline common core functions when types are known.

| Function | Condition | Inline To |
|----------|-----------|-----------|
| `count` | Known vector | `.size()` |
| `get` | Keyword + map | Direct lookup |
| `+`, `-`, `*`, `/` | Numeric primitives | C++ operators |
| `not` | Boolean | `!` |

### Priority 6: Short-Circuit `and`/`or` for C++ Types

**Goal**: Native short-circuit evaluation without boxing.

```cpp
// Before: nested if/else with into_object/truthy calls
// After:
bool result = a && b && c && d;
```

---

## Estimated Impact

For this specific form (~130 lines of jank):

| Category | Current Overhead | Reduction Potential |
|----------|-----------------|---------------------|
| Dynamic calls | ~50 | Could eliminate ~40 |
| Boxing operations | ~80 | Could eliminate ~70 |
| Unnecessary variables | ~60 | All eliminable |
| Nested if/else depth | 6+ levels | 1-2 levels |

**Overall**: Could reduce generated C++ from ~1000 lines to ~300-400 lines with equivalent semantics.

---

## Implementation Roadmap

### Phase 1: Type Tracking Infrastructure
- Extend analyzer to track C++ types through expressions
- Add "primitive type" annotations to expressions

### Phase 2: Primitive Operations
- Arithmetic on C++ ints/floats -> direct C++ ops
- Comparisons on C++ primitives -> direct C++ comparisons
- Boolean negation -> `!`

### Phase 3: Constant Folding
- `(cpp/float. 1.0)` -> `1.0f` in codegen
- Constructor with all-literal args -> aggregate init

### Phase 4: Control Flow
- `and`/`or` on bools -> `&&`/`||`
- `cond` with `:else` -> unconditional else
- `when-not` on bool -> negated if

### Phase 5: Dead Code Elimination
- Don't bind unused void results
- Eliminate redundant nil assignments

---

## Related Work

- This builds on the existing type hint system (`^int`, etc.)
- The `cpp/*` forms already have type information - we need to propagate it
- Core function inlining overlaps with the planned native/interop improvements

---

## Conclusion

The jank compiler generates correct C++ but leaves significant performance on the table for C++ interop-heavy code. By tracking C++ types through the compilation pipeline and emitting direct operations instead of dynamic dispatch, we could dramatically reduce both code size and runtime overhead.

The ImGui use case is particularly representative since it involves:
- Many C++ interop calls with known return types
- Frequent primitive arithmetic and comparisons
- Boolean logic for UI state
- Many void-returning imperative calls

These patterns are common in game/graphics code and would benefit greatly from these optimizations.

---

## Implementation Status (2025-12-19)

### Completed Optimizations

#### Phase 1: Type Tracking Infrastructure ✅
- Added type predicates to `cpp_util.hpp/cpp_util.cpp`:
  - `is_boolean_type()`, `is_integer_type()`, `is_floating_type()`, `is_numeric_type()`, `is_void_type()`
  - `expr_is_cpp_bool()`, `expr_is_cpp_numeric()`, `expr_is_cpp_primitive()`

#### Phase 2: Primitive Arithmetic Optimization ✅
- In `analyze/processor.cpp::analyze_call()`:
  - Routes `clojure.core` arithmetic operations (`+`, `-`, `*`, `/`) on C++ numeric types to direct C++ operators
  - Routes comparison operations (`>`, `<`, `>=`, `<=`) on C++ numeric types to direct C++ operators
  - Optimizes `(not bool-expr)` → `!bool-expr` for C++ bool types

#### Phase 3: Boolean Expression Optimization ✅
- In `codegen/processor.cpp`:
  - Skips `truthy()` wrapper for C++ bool conditions in `if` expressions
  - `if(cpp_bool)` instead of `if(jank::runtime::truthy(cpp_bool))`

#### Phase 4: Constant Folding for Literals ✅
- In `codegen/processor.cpp::gen(cpp_constructor_call_ref)`:
  - `(cpp/int. 500)` → `int ret{ static_cast<int>(500) };`
  - `(cpp/float. -7.0)` → `float ret{ static_cast<float>(-7.0) };`
  - Only folds matching types (int→int, float→float)

#### Phase 5: Statement Position Optimization ✅
- In `codegen/processor.cpp::gen(cpp_call_ref)`:
  - Void-returning C++ functions in statement position skip the nil binding
  - Just emits `func(args);` instead of `func(args); object_ref ret{ nil };`

### Deferred to Future Work

#### Phase 6: Core Function Inlining
- Would require new file `analyze/inline.hpp`
- Inline `count` → `.size()`, `first` → `[0]`, etc.
- More invasive architectural change

#### Phase 7: Optimization Pass Infrastructure
- Would require modifying `analyze/pass/optimize.cpp`
- Multi-pass optimization framework
- More invasive architectural change

### Test Results
- Baseline: 228 passed, 1 failed
- After all implementations: 228 passed, 1 failed (no regressions)
