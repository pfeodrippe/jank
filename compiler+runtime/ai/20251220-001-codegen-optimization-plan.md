# Code Generation Optimization Plan

**Date**: 2025-12-20
**Goal**: Reduce generated C++ code size and improve runtime efficiency for jank-compiled code

## Executive Summary

Comparing a 134-line jank UI function to its 1339-line generated C++ reveals significant optimization opportunities:

| Metric | Current | Target | Improvement |
|--------|---------|--------|-------------|
| Generated lines | 1339 | ~500 | 60% reduction |
| Struct members | ~80 | ~25 | 70% reduction |
| Runtime boxing ops | ~40 | ~10 | 75% reduction |
| Dynamic calls for primitives | ~15 | 0 | 100% elimination |

**Quick wins (implement first)**:
1. **Inline `#cpp` string literals** - Skip wrapper functions for simple strings
2. **Cache var deref results** - Eliminate repeated `->deref()` calls
3. **Cache unbox results** - Eliminate repeated `jank_unbox_lazy_source` calls
4. **Source location table** - Replace inline strings with indexed lookup

**Medium-term wins**:
1. **Native arithmetic** - `(+ a b)` → `a + b` when types are known
2. **Flatten `and`/`or`** - Nested ifs → `&&`/`||` chains for C++ bools
3. **Static string constants** - `constexpr char*` instead of boxed strings

## What Optimized Output Should Look Like

### Current Generated Code (excerpt):
```cpp
// 80+ member fields for constants
jank::runtime::obj::persistent_string_ref const vybe_sdf_ui_const_769;
jank::runtime::obj::persistent_string_ref const vybe_sdf_ui_const_770;
// ... 78 more

// Constructor initializes each
, vybe_sdf_ui_const_769{ jank::runtime::make_box<jank::runtime::obj::persistent_string>("Hide Mesh") }
, vybe_sdf_ui_const_770{ jank::runtime::make_box<jank::runtime::obj::persistent_string>("Show Mesh") }

// Inline function for each #cpp string
[[gnu::always_inline]]
inline decltype(auto) vybe_sdf_ui_G__719() { return ("FPS: %.1f"); }

// In body: repeated unbox calls
auto vybe_sdf_ui_cpp_unbox_1003{ static_cast<bool *>(jank_unbox_lazy_source(
    "bool *", vybe_sdf_ui__STAR_use_dual_contouring_800->deref().data,
    "{:jank/source {:module \"vybe.sdf.ui\", :file ...}")) };
// Same var unboxed again 30 lines later...
auto vybe_sdf_ui_cpp_unbox_1010{ static_cast<bool *>(jank_unbox_lazy_source(
    "bool *", vybe_sdf_ui__STAR_use_dual_contouring_800->deref().data,
    "{:jank/source {:module \"vybe.sdf.ui\", :file ...}")) };

// Arithmetic via dynamic call
auto const vybe_sdf_ui_call_983(
  jank::runtime::dynamic_call(vybe_sdf_ui___792->deref(),  // clojure.core/-
                              vybe_sdf_ui_cpp_cast_984,
                              vybe_sdf_ui_step_788));
```

### Target Optimized Output:
```cpp
// Static constants (no heap allocation)
static constexpr char* str_hide_mesh = "Hide Mesh";
static constexpr char* str_show_mesh = "Show Mesh";

// Source locations in table (not inline)
static constexpr source_loc __locs[] = {{90,37,90,46}, {93,38,93,47}, ...};

// No wrapper functions - string literals used directly
ImGui::Text("FPS: %.1f", fps);

// Cached var deref (once per scope)
auto& use_dc_var = vybe_sdf_ui__STAR_use_dual_contouring_800->deref();
auto* use_dc_ptr = static_cast<bool*>(jank_unbox<0>("bool*", use_dc_var.data));
// Reuse use_dc_ptr everywhere

// Native arithmetic when types known
int result = res - step;  // instead of dynamic_call
```

## Analysis Summary

Comparing a 134-line jank UI function to its 1339-line generated C++ reveals significant optimization opportunities. The generated code is approximately **10x larger** than necessary and contains multiple runtime inefficiencies.

## Identified Issues

### 1. Excessive Constant Boxing (~80 member fields)

**Current**: Every literal value (string, int, float, keyword, nil) becomes a heap-allocated boxed object stored as a struct member.

```cpp
// 80+ fields like this:
jank::runtime::obj::persistent_string_ref const vybe_sdf_ui_const_837;
jank::runtime::obj::integer_ref const vybe_sdf_ui_const_811;
jank::runtime::obj::real_ref const vybe_sdf_ui_const_716;

// Constructor initializes each:
, vybe_sdf_ui_const_837{ jank::runtime::make_box<jank::runtime::obj::persistent_string>(
    "Loaded exported_scene.glb into viewer") }
, vybe_sdf_ui_const_811{ jank::runtime::make_box<jank::runtime::obj::integer>(
    static_cast<jank::i64>(-1)) }
```

**Impact**: ~300 lines of member declarations + initialization, heap allocations at load time.

---

### 2. Dynamic Dispatch for Primitive Arithmetic

**Current**: `+`, `-`, `<`, `>` are called via var lookup + dynamic_call even for primitive types.

```cpp
// (- res step) becomes:
auto const vybe_sdf_ui_call_983(
  jank::runtime::dynamic_call(vybe_sdf_ui___792->deref(),  // lookup clojure.core/-
                              vybe_sdf_ui_cpp_cast_984,     // box res as object
                              vybe_sdf_ui_step_788));       // step is already boxed
int vybe_sdf_ui_cpp_ctor_982{ jank::runtime::convert<int>::from_object(
  vybe_sdf_ui_call_983.get()) };  // unbox result
```

**Impact**: 3 function calls + 2 boxing operations for simple subtraction.

**Optimal**: `res - step` (single instruction)

---

### 3. Repeated Var Deref + Unbox Operations

**Current**: Same var is deref'd and unboxed multiple times within the same scope.

```cpp
// First use of *use-dual-contouring
auto vybe_sdf_ui_cpp_unbox_1003{ static_cast<bool *>(jank_unbox_lazy_source(
    "bool *",
    vybe_sdf_ui__STAR_use_dual_contouring_800->deref().data,
    "{:jank/source ...}")) };

// Same var again, 30 lines later:
auto vybe_sdf_ui_cpp_unbox_1010{ static_cast<bool *>(jank_unbox_lazy_source(
    "bool *",
    vybe_sdf_ui__STAR_use_dual_contouring_800->deref().data,
    "{:jank/source ...}")) };

// And again:
auto vybe_sdf_ui_cpp_unbox_1013{ static_cast<bool *>(jank_unbox_lazy_source(
    "bool *",
    vybe_sdf_ui__STAR_use_dual_contouring_800->deref().data,
    "{:jank/source ...}")) };
```

**Impact**: Each `jank_unbox_lazy_source` includes a long source location string + runtime type check.

---

### 4. String Constant Conversion Overhead

**Current**: C string literals like `"Hide Mesh"` are first boxed as `persistent_string`, then converted back to `char const*` for ImGui calls.

```cpp
// String stored as boxed object:
, vybe_sdf_ui_const_769{ jank::runtime::make_box<jank::runtime::obj::persistent_string>("Hide Mesh") }

// Then converted at use site:
auto const vybe_sdf_ui_cpp_cast_943{
  jank::runtime::convert<char const *>::from_object(vybe_sdf_ui_if_944)
};
```

**Impact**: Heap allocation + conversion overhead for static strings.

---

### 5. Verbose `and` Macro Expansion

**Current**: `(and a b c d)` compiles to deeply nested if-else with `jtl::option<bool>` wrappers.

```cpp
jtl::option<bool> vybe_sdf_ui_let_865{};
{
  auto &&vybe_sdf_ui_vybe_sdf_ui_G___726_727(vybe_sdf_ui_mesh_visible_720);
  bool vybe_sdf_ui_if_866{};
  if(vybe_sdf_ui_vybe_sdf_ui_G___726_727)
  {
    jtl::option<bool> vybe_sdf_ui_let_867{};
    {
      auto &&vybe_sdf_ui_vybe_sdf_ui_G___728_729(vybe_sdf_ui_mesh_solid_721);
      bool vybe_sdf_ui_if_868{};
      if(vybe_sdf_ui_vybe_sdf_ui_G___728_729)
      {
        // ... continues nesting for each term
      }
    }
  }
}
auto &&vybe_sdf_ui_compute_skipped_732(vybe_sdf_ui_let_865.unwrap());
```

**Optimal for bools**:
```cpp
bool compute_skipped = mesh_visible && mesh_solid && mesh_initialized && mesh_has_indices;
```

---

### 6. Inline Helper Functions for `#cpp` Strings

**Current**: Each `#cpp "..."` format string generates a separate inline function.

```cpp
[[gnu::always_inline]]
inline decltype(auto) vybe_sdf_ui_G__719()
{
  return ("FPS: %.1f");
}

// Called as:
auto &&vybe_sdf_ui_cpp_call_858{ vybe_sdf_ui_G__719() };
ImGui::Text(vybe_sdf_ui_cpp_call_858, vybe_sdf_ui_fps_715);
```

**Optimal**:
```cpp
ImGui::Text("FPS: %.1f", fps);
```

---

### 7. Unnecessary Boxing/Unboxing Round-trips

**Current**: Values from C++ calls get boxed only to be immediately unboxed.

```cpp
// C++ call returns int
auto &&vybe_sdf_ui_cpp_call_965{ sdfx::get_mesh_preview_resolution() };

// Later, compared with < (which needs boxing because clojure.core/< is called)
auto &&vybe_sdf_ui_cpp_operator_967(vybe_sdf_ui_res_779 < 64LL);  // Wait, this one is native!
```

Actually, this example shows the comparison IS native. But for `+`/`-`:

```cpp
auto const vybe_sdf_ui_cpp_cast_984{
  jank::runtime::convert<int>::into_object(vybe_sdf_ui_res_779)  // box int
};
auto const vybe_sdf_ui_call_983(
  jank::runtime::dynamic_call(vybe_sdf_ui___792->deref(),  // call -
                              vybe_sdf_ui_cpp_cast_984,
                              vybe_sdf_ui_step_788));
int vybe_sdf_ui_cpp_ctor_982{ jank::runtime::convert<int>::from_object(
  vybe_sdf_ui_call_983.get()) };  // unbox result
```

---

### 8. Source Location Strings in Unbox Calls

**Current**: Each `jank_unbox_lazy_source` embeds a full source location string (~150 bytes each).

```cpp
"{:jank/source {:module \"vybe.sdf.ui\", :file "
"\"/Users/pfeodrippe/dev/something/src/vybe/sdf/ui.jank\", :start {:offset "
"2388, :line 90, :col 37}, :end {:offset 2397, :line 90, :col 46}}}"
```

**Impact**: ~20 such strings in this file = ~3KB of string data.

---

## Optimization Implementation Plan

### Phase 1: Quick Wins (Low Risk, High Impact)

#### 1.1 Static String Literals for `#cpp` Strings
Instead of boxing string constants, use `static constexpr char*`:

```cpp
// Before
jank::runtime::obj::persistent_string_ref const const_717;
// constructor: const_717{ make_box<persistent_string>("SDF Debug") }
// usage: convert<char const*>::from_object(const_717)

// After (for strings only used with C++ interop)
static constexpr char* const_717 = "SDF Debug";
// usage: const_717
```

**Files to modify**: `compiler+runtime/src/cpp/jank/codegen/llvm_processor.cpp` (or wherever codegen happens)

#### 1.2 Inline Format String Functions
Remove the wrapper functions for `#cpp "..."`:

```cpp
// Before
inline decltype(auto) G__719() { return ("FPS: %.1f"); }
auto &&cpp_call_858{ G__719() };
ImGui::Text(cpp_call_858, fps);

// After
ImGui::Text("FPS: %.1f", fps);
```

#### 1.3 Deduplicate Source Location Strings
Use a static table of source locations instead of embedding inline:

```cpp
// Before: inline string per call
jank_unbox_lazy_source("bool *", data, "{:jank/source {:module \"vybe.sdf.ui\"...}}")

// After: index into table
static constexpr source_loc locs[] = { {90, 37, 90, 46}, ... };
jank_unbox_lazy_source<0>("bool *", data)  // template index
```

---

### Phase 2: Common Subexpression Elimination (Medium Risk)

#### 2.1 Cache Var Deref Results
When the same var is deref'd multiple times in a scope:

```cpp
// Before: 3 separate deref() calls
vybe_sdf_ui__STAR_use_dual_contouring_800->deref().data
vybe_sdf_ui__STAR_use_dual_contouring_800->deref().data
vybe_sdf_ui__STAR_use_dual_contouring_800->deref().data

// After: single deref, reuse
auto& cached_use_dc = vybe_sdf_ui__STAR_use_dual_contouring_800->deref();
// then use cached_use_dc.data throughout
```

#### 2.2 Cache Unbox Results
When the same pointer is unboxed multiple times:

```cpp
// Before: 3 separate unbox calls
auto* ptr1 = static_cast<bool*>(jank_unbox_lazy_source(..., var->deref().data, ...));
auto* ptr2 = static_cast<bool*>(jank_unbox_lazy_source(..., var->deref().data, ...));
auto* ptr3 = static_cast<bool*>(jank_unbox_lazy_source(..., var->deref().data, ...));

// After: single unbox
auto* use_dc_ptr = static_cast<bool*>(jank_unbox_lazy_source(..., cached_use_dc.data, ...));
// reuse use_dc_ptr
```

---

### Phase 3: Type-Specialized Primitive Operations (Medium-High Risk)

#### 3.1 Native Arithmetic for Typed Values
When operands have known primitive types, generate direct operations:

```cpp
// jank source: (- res step)
// where res is from (sdfx/get_mesh_preview_resolution) which returns int
// and step is from cond which returns an integer

// Before:
auto const boxed_res = convert<int>::into_object(res);
auto const result = dynamic_call(minus_var->deref(), boxed_res, step);
int unboxed = convert<int>::from_object(result.get());

// After (when types are known):
int result = res - unbox_int(step);  // or just: res - step_value
```

**Requirements**:
- Track types through codegen (from cpp/ type hints)
- Emit specialized code paths for known-primitive arithmetic

#### 3.2 Native Boolean `and`/`or`
For `(and bool1 bool2 ...)` where all are C++ bools:

```cpp
// Before: nested jtl::option<bool> with if-else chains

// After:
bool result = bool1 && bool2 && bool3 && bool4;
```

---

### Phase 4: Lazy Initialization (High Impact, Moderate Risk)

#### 4.1 Move Constants to Static Storage
Instead of per-instance member initialization:

```cpp
// Before: struct members initialized in constructor
struct jit_function {
  persistent_string_ref const_1;  // member
  persistent_string_ref const_2;  // member
  // ... 80 more

  jit_function() : const_1{make_box("...")}, const_2{make_box("...")} {}
};

// After: static initialization on first use
struct jit_function {
  static persistent_string_ref& get_const_1() {
    static auto val = make_box<persistent_string>("...");
    return val;
  }
};
// Or better, a single static constants table
```

#### 4.2 Keyword/Symbol Interning at Load Time
Pre-intern common keywords in a shared table:

```cpp
// Before: each jit_function interns its own keywords
, const_748{ __rt_ctx->intern_keyword("", "distance", true).expect_ok() }

// After: module-level shared intern table
static auto kw_distance = lazy_intern_keyword("distance");
```

---

### Phase 5: Structural Improvements (Requires Careful Design)

#### 5.1 Flattening Nested Scopes
The generated code has excessive nesting from let bindings:

```cpp
// Before: deeply nested scopes
{
  auto&& a = ...;
  {
    auto&& b = ...;
    {
      auto&& c = ...;
      // actual work
    }
  }
}

// After: flat scope when safe
auto&& a = ...;
auto&& b = ...;
auto&& c = ...;
// actual work
```

#### 5.2 Return Value Optimization
Many places return `jank_nil` unnecessarily:

```cpp
jank::runtime::object_ref const vybe_sdf_ui_cpp_call_873{ jank::runtime::jank_nil };
vybe_sdf_ui_if_871 = jank::runtime::jank_nil;
```

When the result is unused, skip the assignment entirely.

---

## Expected Impact

| Optimization | Code Size Reduction | Runtime Improvement |
|--------------|--------------------|--------------------|
| Static string literals | ~100 lines | Minor (fewer boxes) |
| Inline format strings | ~30 lines | Minor |
| Dedup source locations | ~60 lines (~3KB strings) | None |
| Cache var deref | ~50 lines | Moderate (fewer indirections) |
| Cache unbox results | ~40 lines | Moderate |
| Native arithmetic | ~50 lines | **Significant** (10-100x faster) |
| Native `and`/`or` | ~80 lines | Moderate |
| Static constants | ~200 lines | Faster startup |
| Flat scopes | ~100 lines | Minor |

**Total estimated reduction**: ~60-70% code size reduction, 2-5x runtime improvement for hot paths.

---

## Implementation Status

### Completed (2025-12-20)

1. **CSE for cpp/unbox** - DONE (scope bug fixed)
   - Implementation in `codegen/processor.cpp` with `unbox_cache` in `processor.hpp`
   - **Bug found and fixed**: Cache entries were crossing C++ scope boundaries in branching constructs
   - **Root cause**: Variables declared inside `if(...) {}` block are not visible in `else {}` block,
     but the cache was shared across both branches
   - **Fix**: Cache save/restore pattern around branching constructs:
     - `if`/`else`: Save cache before branches, restore before each branch, restore after construct
     - `try`/`catch`: Save cache before try, restore before catch, restore after construct
     - `case`/`switch`: Save cache before cases, restore before each case and default, restore after construct
   - Location of fix: `codegen/processor.cpp` lines 1370-1408 (if), 1446-1487 (try/catch), 1523-1556 (case)
   - Tests pass, user's project compiles and runs correctly

2. **Inline #cpp string literals** - DONE
   - Files modified:
     - `include/cpp/jank/analyze/expr/cpp_value.hpp`: Added `string_literal` to `value_kind` enum, added `literal_str` field
     - `src/cpp/jank/analyze/processor.cpp`: Detect simple C string literals (strings starting/ending with `"`) and skip wrapper function
     - `src/cpp/jank/codegen/processor.cpp`: Emit string literal directly instead of function call
     - `src/cpp/jank/codegen/llvm_processor.cpp`: WASM codegen for string literals using `CreateGlobalString`
   - How it works:
     - In analyze phase, detects `#cpp "..."` where the content is a simple string literal
     - Gets `char const*` type via `Cpp::GetType("char const*")`
     - Falls back to normal path if type lookup fails (safety net)
     - In codegen, emits the literal string directly instead of generating a wrapper function
   - Result: `#cpp "FPS: %.1f"` now emits `"FPS: %.1f"` directly instead of `G__719()`
   - Tests pass (same 1 pre-existing nREPL test failure)

### Pending

3. **Source location table**
   - Complexity: Requires two-pass codegen (collect then emit table)
   - Current: Each unbox embeds ~150 byte source string inline
   - Would need: Collection phase + table generation + indexed lookups

## Implementation Priority (Remaining)

1. **Short-term**:
   - Native arithmetic when types known (high impact for numeric code)
   - Native boolean `and`/`or` operations

2. **Medium-term**:
   - Source location table (deduplication of source strings)
   - Static string constants (constexpr for jank string literals)

3. **Long-term**:
   - Scope flattening
   - Full type inference for optimization

---

## Concrete Implementation Details (From Codebase Analysis)

### Key Files and Line Numbers

| Component | File | Key Lines |
|-----------|------|-----------|
| Constant type generation | `codegen/processor.cpp` | 72-148 (`gen_constant_type`) |
| Constant value generation | `codegen/processor.cpp` | 151-409 (`gen_constant`) |
| Var deref codegen | `codegen/processor.cpp` | 610-627 |
| C++ call codegen | `codegen/processor.cpp` | 1624-1782 |
| Builtin operator codegen | `codegen/processor.cpp` | 2005-2064 |
| cpp/unbox generation | `codegen/processor.cpp` | 2108-2147 |
| cpp/value wrapper creation | `analyze/cpp_util.cpp` | 227-272 (`resolve_literal_value`) |
| `and`/`or` macros | `src/jank/clojure/core.jank` | 769-799 |

### Already-Implemented Optimizations

These are already done (don't re-implement):

1. **Primitive literal optimization in operators** (processor.cpp:2012-2028)
   - Detects primitive literals and emits raw values (`1LL`, `2.5`)

2. **Constant folding for `cpp/int.`, `cpp/float.`** (processor.cpp:1789-1823)
   - `(cpp/int. 42)` → `int x{ 42 };` directly

3. **Statement position skip** (processor.cpp:1650-1697)
   - Doesn't capture return value for void functions in statement position

4. **Lazy source parsing** (processor.cpp:2108-2147)
   - Source metadata only parsed on error path

5. **C++ bool optimization in conditions** (processor.cpp:1351-1366, WASM only)
   - Skips `truthy()` for C++ bool values

### Specific Implementation Suggestions

#### 1. Eliminate Wrapper Functions for String Literals

**Location**: `analyze/cpp_util.cpp` lines 227-272

**Current behavior**: All `#cpp "..."` values get wrapped in an inline function:
```cpp
[[gnu::always_inline]] inline decltype(auto) G__719(){ return ("FPS: %.1f"\n); }
```

**Optimization**: For simple string literals, skip the function wrapper:

```cpp
// In resolve_literal_value()
// Before wrapping in function, check if literal is a simple string
if(is_simple_string_literal(literal)) {
  // Return type is char const*, no wrapper needed
  return literal_value_result{ nullptr, char_const_ptr_type, literal };
}
```

Then in `codegen/processor.cpp`, when `function_code` is empty but `source_expr` is a string literal, emit directly.

#### 2. CSE for Var Deref

**Location**: `codegen/processor.cpp` around line 610

**Implementation**: Track dereferenced vars in a scope-local map:

```cpp
// Add to processor struct:
native_hash_map<jtl::immutable_string, jtl::immutable_string> deref_cache;

// In gen(var_deref_ref):
auto const cache_key = var.native_name;
if(auto it = deref_cache.find(cache_key); it != deref_cache.end()) {
  return it->second;  // Reuse cached deref
}
auto tmp = format("{}->deref()", runtime::munge(var.native_name));
auto cached_name = unique_tmp();
format_to(body_buffer, "auto const& {}{{ {} }};", cached_name, tmp);
deref_cache[cache_key] = cached_name;
return cached_name;
```

**Scope consideration**: Clear cache at function boundaries (let bindings that rebind).

#### 3. CSE for cpp/unbox

**Location**: `codegen/processor.cpp` around line 2108

**Implementation**: Similar to var deref, cache unbox results:

```cpp
// Key: (type_name, var_name) pair
// Value: cached tmp name

// Before generating unbox:
auto cache_key = std::make_pair(type_name, value_tmp);
if(auto it = unbox_cache.find(cache_key); it != unbox_cache.end()) {
  return it->second;
}
// Generate unbox as normal, cache result
```

#### 4. Native Arithmetic for Known Types

**Location**: `codegen/processor.cpp` around line 2005

**Current**: Even when operands are known-typed (from cpp/ constructors), arithmetic goes through `dynamic_call`.

**Detection**: Check if operands have `cpp_constructor_call` expression type with known numeric types.

```cpp
// In gen for call expressions involving +/-/*//
if(is_core_arithmetic_fn(fn_name) && all_args_cpp_typed(arg_exprs)) {
  auto lhs = gen(arg_exprs[0], arity).unwrap();
  auto rhs = gen(arg_exprs[1], arity).unwrap();
  return format("({} {} {})", lhs.str(false), op_symbol, rhs.str(false));
}
```

#### 5. Flatten `and`/`or` for C++ Bools

**Location**: New optimization pass or modify macro expansion

**Approach A** (Macro change in core.jank):
Add a compiler macro that detects all-bool `and` forms.

**Approach B** (Codegen optimization):
In `codegen/processor.cpp`, detect chains of `if` expressions from `and` expansion:
```cpp
// Pattern match: (let [g (if a (if b (if c c b) a))])
// When all conditions are C++ bools, emit: (a && b && c)
```

#### 6. Static String Constants for Interop

**Location**: `codegen/processor.cpp` constant generation

**For strings only used with C++ interop** (detectable by checking all uses):

```cpp
// Instead of:
, const_717{ make_box<persistent_string>("SDF Debug") }

// Generate:
static constexpr char const* const_717 = "SDF Debug";
```

**Detection**: During lifting, track if a string constant is only used in `cpp_call` contexts.

#### 7. Source Location Table

**Location**: `codegen/processor.cpp` around line 2123

**Current**: Each unbox embeds full source string (~150 bytes).

**Change**: Create static table in header, reference by index:

```cpp
// In header:
static constexpr jank::source_loc __src_locs[] = {
  {90, 37, 90, 46},  // 0
  {93, 38, 93, 47},  // 1
  ...
};

// In unbox call:
jank_unbox_lazy_source<0>("bool *", var.data)
```

### Estimated Code Changes

| Optimization | Files Changed | Lines Added | Lines Removed |
|--------------|---------------|-------------|---------------|
| String literal bypass | cpp_util.cpp, processor.cpp | ~40 | ~10 |
| Var deref CSE | processor.cpp, processor.hpp | ~30 | ~5 |
| Unbox CSE | processor.cpp, processor.hpp | ~30 | ~5 |
| Native arithmetic | processor.cpp | ~60 | ~0 |
| Bool and/or | processor.cpp or core.jank | ~50 | ~0 |
| Static string constants | processor.cpp | ~40 | ~20 |
| Source location table | processor.cpp, c_api.cpp | ~80 | ~40 |

## Files to Investigate

To implement these optimizations, investigate:

1. `compiler+runtime/src/cpp/jank/codegen/` - Core code generation
2. `compiler+runtime/src/cpp/jank/analyze/` - Type analysis passes
3. `compiler+runtime/src/cpp/jank/evaluate/` - Runtime evaluation
4. `compiler+runtime/include/cpp/jank/runtime/` - Runtime types and conversions
