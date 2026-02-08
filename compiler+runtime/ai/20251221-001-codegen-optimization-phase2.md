# Code Generation Optimization Phase 2

**Date**: 2025-12-21
**Goal**: Further reduce generated C++ code size and improve runtime efficiency - building on Phase 1 optimizations

## Executive Summary

Building on the Phase 1 optimizations (CSE for cpp/unbox, inline #cpp strings), this plan identifies **additional optimization opportunities** from analyzing the form-jank.txt/form-cpp.txt example.

### New Optimizations Identified

| Optimization | Code Reduction | Runtime Impact | Complexity |
|--------------|---------------|----------------|------------|
| Shared static var refs | ~40 lines | Faster startup | Low |
| Small integer cache | ~20 boxings | Fewer allocations | Medium |
| Keyword direct invocation | ~15 lines | Faster map lookups | Medium |
| Cond native int branches | ~30 lines | Eliminate boxing | Medium |
| Static nil ref | ~20 refs | Marginal | Low |
| Empty meta optimization | ~1 line per struct | Faster construct | Low |
| C++ operator shortcuts | ~40 lines | Fewer conversions | Medium |

---

## Detailed Analysis

### 1. Per-Instance Var Refs Should Be Static Shared

**Current**: Each JIT function struct stores its own copies of var_refs for core functions:

```cpp
struct vybe_sdf_ui_native_source_1499 : jank::runtime::obj::jit_function
{
    jank::runtime::var_ref const vybe_sdf_ui_println_1481;      // per-instance
    jank::runtime::var_ref const vybe_sdf_ui__PLUS__1451;       // clojure.core/+
    jank::runtime::var_ref const vybe_sdf_ui___1449;            // clojure.core/-
    jank::runtime::var_ref const vybe_sdf_ui_count_1422;        // clojure.core/count
    jank::runtime::var_ref const vybe_sdf_ui__LT__1437;         // clojure.core/<
    jank::runtime::var_ref const vybe_sdf_ui__GT__1380;         // clojure.core/>
    jank::runtime::var_ref const vybe_sdf_ui_not___1395;        // clojure.core/not
    // ... 14+ var refs in this example
```

**Problem**: Every instance of this struct re-interns these vars. For frequently-called functions or multiple instances, this wastes memory and initialization time.

**Proposed Solution**:

```cpp
// Module-level static initialization (once per module load)
namespace vybe::sdf::ui::__vars {
    inline jank::runtime::var_ref const& println() {
        static auto v = jank::runtime::__rt_ctx->intern_var("clojure.core", "println").expect_ok();
        return v;
    }
    inline jank::runtime::var_ref const& plus() {
        static auto v = jank::runtime::__rt_ctx->intern_var("clojure.core", "+").expect_ok();
        return v;
    }
    // ...
}

struct vybe_sdf_ui_native_source_1499 : jank::runtime::obj::jit_function {
    // No var_ref members needed!
    // Usage: __vars::println()->deref()
```

**Implementation Location**: `codegen/processor.cpp` - constant lifting and struct generation

**Estimated Impact**:
- ~40 lines removed from struct declaration
- ~40 lines removed from constructor initializer
- Faster module load (vars interned once, not per-instance)
- Smaller struct size

---

### 2. Small Integer Cache

**Current**: Common small integers are boxed individually:

```cpp
jank::runtime::obj::integer_ref const vybe_sdf_ui_const_1468{
    jank::runtime::make_box<jank::runtime::obj::integer>(static_cast<jank::i64>(-1))
};
jank::runtime::obj::integer_ref const vybe_sdf_ui_const_1381{
    jank::runtime::make_box<jank::runtime::obj::integer>(static_cast<jank::i64>(0))
};
jank::runtime::obj::integer_ref const vybe_sdf_ui_const_1439{
    jank::runtime::make_box<jank::runtime::obj::integer>(static_cast<jank::i64>(8))
};
// Also: 16, 32, 64, 128, 256
```

**Insight**: These specific integers (0, 8, 16, 32, 64, 128, 256, -1) are common in code. Many Lisps cache small integers (-128 to 127 or similar).

**Proposed Solution**:

```cpp
// In runtime, add cached integers:
namespace jank::runtime {
    inline object_ref cached_int(i64 n) {
        static obj::integer_ref cache[256]; // -128 to 127
        if(n >= -128 && n <= 127) {
            auto idx = n + 128;
            if(!cache[idx]) cache[idx] = make_box<obj::integer>(n);
            return cache[idx];
        }
        return make_box<obj::integer>(n);
    }
}

// Codegen emits:
jank::runtime::cached_int(8)  // instead of make_box<integer>(8)
```

**Or simpler**: Pre-create common values as globals:

```cpp
// In jank/runtime/obj/integer.hpp or similar:
inline obj::integer_ref const int_0 = make_box<obj::integer>(0);
inline obj::integer_ref const int_neg1 = make_box<obj::integer>(-1);
// For powers of 2 used in UI/graphics: 8, 16, 32, 64, 128, 256, 512, 1024
```

**Implementation Location**:
- `runtime/obj/integer.hpp` - add cache
- `codegen/processor.cpp` - detect small integers and use cache

**Estimated Impact**:
- Fewer heap allocations at module load
- Potential sharing across modules

---

### 3. Keyword Direct Invocation

**Current**: Keywords like `:distance`, `:angle-x` used for map access go through `dynamic_call`:

```cpp
// (:distance cam) becomes:
auto const vybe_sdf_ui_call_1570(
    jank::runtime::dynamic_call(vybe_sdf_ui_const_1405,  // :distance keyword
                                vybe_sdf_ui_cam_1366));  // cam map
```

**Problem**: `dynamic_call` does runtime dispatch. Keywords are callable - they implement `call(object_ref)` directly.

**Proposed Solution**:

```cpp
// Direct invocation (keyword has call method):
auto const result = vybe_sdf_ui_const_1405->call(vybe_sdf_ui_cam_1366);
```

Or even better, when we know it's a map:

```cpp
// Direct map get (fastest):
auto const result = jank::runtime::get(vybe_sdf_ui_cam_1366, vybe_sdf_ui_const_1405);
```

**Implementation Location**: `codegen/processor.cpp` - in call expression handling, detect keyword in function position

**Detection**: Check if the callable expression is a lifted keyword constant.

**Estimated Impact**:
- Fewer dynamic dispatch calls
- 10-20% faster map accesses in hot paths

---

### 4. Cond with Native Integer Branches

**Current**: The `cond` expression for calculating `step` returns boxed integers:

```cpp
// From: (cond (< res 64) 8 (< res 128) 16 (< res 256) 32 :else 64)
jank::runtime::object_ref vybe_sdf_ui_if_1621{};
auto &&vybe_sdf_ui_cpp_operator_1622(vybe_sdf_ui_res_1436 < 64LL);  // Native comparison!
if(vybe_sdf_ui_cpp_operator_1622)
{
    vybe_sdf_ui_if_1621 = vybe_sdf_ui_const_1439;  // Boxed 8
}
else
{
    // ... more nested ifs returning boxed integers
}
{
    auto &&vybe_sdf_ui_step_1445(vybe_sdf_ui_if_1621);  // step is object_ref
    // Later, step is used in arithmetic which requires boxing/unboxing
}
```

**Observation**: The comparisons are already native (`res < 64LL`), but the return values are boxed.

**Problem**: `step` is then used in `(- res step)` which boxes res, calls `dynamic_call`, and unboxes result.

**Proposed Solution**: When all cond branches return integer literals AND the value is used in native context:

```cpp
// Generate native int cond:
int step;
if(res < 64LL) { step = 8; }
else if(res < 128LL) { step = 16; }
else if(res < 256LL) { step = 32; }
else { step = 64; }

// Then arithmetic is native:
int result = res - step;
```

**Implementation**: Requires type inference to determine:
1. All cond branches return same primitive type (int literals)
2. The value is used in a context expecting that type

**Complexity**: Medium-High (requires flow analysis)

---

### 5. Static Nil Reference

**Current**: Nil is stored as a lifted constant and referenced many times:

```cpp
jank::runtime::obj::nil_ref const vybe_sdf_ui_const_1397;
// constructor:
, vybe_sdf_ui_const_1397{ jank::runtime::jank_nil }

// Used ~20+ times:
vybe_sdf_ui_if_1544 = vybe_sdf_ui_const_1397;
vybe_sdf_ui_if_1549 = vybe_sdf_ui_const_1397;
```

**Proposed Solution**: Don't lift nil - use `jank::runtime::jank_nil` directly:

```cpp
// Instead of:
vybe_sdf_ui_if_1544 = vybe_sdf_ui_const_1397;

// Just:
vybe_sdf_ui_if_1544 = jank::runtime::jank_nil;
```

**Implementation Location**: `codegen/processor.cpp` - in constant generation, skip nil lifting

**Estimated Impact**:
- 2 lines removed (member + init)
- Tiny binary size reduction
- No runtime difference (jank_nil is already a global)

---

### 6. Empty Metadata Optimization

**Current**: Every jit_function reads empty map `"{}"`:

```cpp
vybe_sdf_ui_native_source_1499()
    : jank::runtime::obj::jit_function{ jank::runtime::__rt_ctx->read_string("{}") }
```

**Problem**: `read_string("{}")` parses a string at runtime to create an empty persistent_hash_map.

**Proposed Solution**: Use pre-created empty map:

```cpp
// In runtime (likely already exists):
inline object_ref const empty_map = make_box<obj::persistent_hash_map>();

// Codegen:
: jank::runtime::obj::jit_function{ jank::runtime::empty_map }
```

**Or**: If jit_function always has empty metadata, make it the default:

```cpp
// jit_function constructor:
jit_function() : meta{ jank::runtime::empty_map } {}
```

**Implementation Location**:
- Check if `jank::runtime::obj::persistent_hash_map::empty()` exists
- `codegen/processor.cpp` - struct initialization

---

### 7. C++ Operator Result Direct Use

**Current**: C++ operator results are often captured then immediately used:

```cpp
auto &&vybe_sdf_ui_cpp_operator_1520(vybe_sdf_ui_cpp_call_1521 > 0LL);
{
    auto &&vybe_sdf_ui_mesh_has_indices_1382(vybe_sdf_ui_cpp_operator_1520);
    // ... mesh_has_indices is used
}
```

**Issue**: Extra variable binding when the value is used once.

**Proposed Solution**: When the result of an operator is immediately bound and used only once:

```cpp
// Skip intermediate:
auto &&mesh_has_indices = (get_mesh_preview_triangle_count() > 0LL);
```

**Or inline directly when used in boolean context**:

```cpp
if(get_mesh_preview_triangle_count() > 0LL) { ... }
```

**Complexity**: Low-Medium (liveness analysis to determine single use)

---

### 8. Float Literal Suffix

**Current**: Float literals use verbose casting:

```cpp
float vybe_sdf_ui_cpp_ctor_1509{ static_cast<float>(10.000000) };
```

**Proposed Solution**: Use `f` suffix for known float literals:

```cpp
float vybe_sdf_ui_cpp_ctor_1509{ 10.0f };
```

**Implementation**: In codegen for `cpp/float.` with literal argument, emit `Nf` instead of `static_cast<float>(N)`.

---

### 9. Boolean Cast Elimination

**Current**: Bool values are cast unnecessarily:

```cpp
auto &&vybe_sdf_ui_cpp_operator_1664(*vybe_sdf_ui_cpp_unbox_1658);  // Already bool
bool vybe_sdf_ui_cpp_ctor_1663{ static_cast<bool>(vybe_sdf_ui_cpp_operator_1664) };  // Redundant
sdfx::set_mesh_use_dual_contouring(vybe_sdf_ui_cpp_ctor_1663);
```

**Proposed Solution**: Skip cast when source is already bool:

```cpp
sdfx::set_mesh_use_dual_contouring(*use_dc_ptr);  // Direct
```

**Detection**: Track that `*unbox_ptr` returns the target type.

---

## Implementation Plan

### Phase 2A: Quick Wins (1-2 days)

1. **Static nil reference** (30 min)
   - Skip lifting nil constants
   - Use `jank::runtime::jank_nil` directly

2. **Float literal suffix** (30 min)
   - In `cpp/float.` codegen, emit `Nf` for literals

3. **Empty metadata optimization** (1 hour)
   - Use pre-created empty map instead of `read_string("{}")`

4. **Boolean cast elimination** (1 hour)
   - Detect when static_cast<bool> is redundant

### Phase 2B: Medium Effort (2-3 days)

5. **Static shared var refs** (4 hours)
   - Create module-level static var accessors
   - Remove per-instance var_ref members
   - Update usage sites to use static accessors

6. **Keyword direct invocation** (3 hours)
   - Detect keyword in function position
   - Emit direct `->call()` instead of `dynamic_call`

7. **Small integer cache** (3 hours)
   - Add runtime integer cache
   - Modify codegen to use cache for common values

### Phase 2C: Significant Refactoring (1 week)

8. **Native cond optimization** (1-2 days)
   - Detect all-integer-literal cond branches
   - Infer when result is used in native context
   - Generate native if-else chain

9. **C++ operator inlining** (2 days)
   - Track single-use bindings
   - Inline operator results directly

---

## Relationship to Phase 1

This plan complements the Phase 1 optimizations:

| Phase 1 (Done) | Phase 2 (This Plan) |
|----------------|---------------------|
| CSE for cpp/unbox | Small integer cache |
| Inline #cpp strings | Static var refs |
| (Planned) Source location table | Keyword direct call |
| (Planned) Native arithmetic | Native cond branches |
| (Planned) Native and/or | C++ operator shortcuts |

---

## Metrics to Track

Before/after measurements for the form-jank.txt example:

1. **Generated C++ lines**: Currently ~1290, target ~500
2. **Struct member count**: Currently ~80, target ~25
3. **Constructor initializer lines**: Currently ~100, target ~20
4. **Boxing operations**: Currently ~40, target ~10
5. **Dynamic calls**: Currently ~20, target ~5

---

## Key Implementation Files

| File | Purpose | Changes |
|------|---------|---------|
| `codegen/processor.cpp` | Main codegen | Most changes |
| `codegen/processor.hpp` | Codegen state | Add caches |
| `runtime/obj/integer.hpp` | Integer type | Add cache |
| `runtime/core.hpp` | Core runtime | Empty map, nil |
| `analyze/processor.cpp` | Analysis | Type tracking |

---

## Testing Strategy

1. **Existing tests**: Run `./bin/test` before and after
2. **Generated code diff**: Compare output for form-jank.txt
3. **Compilation test**: Ensure generated code compiles
4. **Runtime test**: Verify semantics preserved
5. **Performance test**: Measure UI function call time

