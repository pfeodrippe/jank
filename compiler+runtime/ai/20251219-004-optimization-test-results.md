# C++ Optimization Test Results

## Summary Table

| Test | Description | Status | Generated Code |
|------|-------------|--------|----------------|
| 1 | `>` with cpp/int + literal | **OPTIMIZED** | `x > 0LL` |
| 2 | `<` with cpp/int + literal | **OPTIMIZED** | `x < 10LL` |
| 3 | `+` with two cpp/int | **OPTIMIZED** | `a + b` |
| 4 | `-` with cpp/int + literal | **OPTIMIZED** | `x - 5LL` |
| 5 | `*` with cpp/int + literal | **OPTIMIZED** | `x * 7LL` |
| 6 | `/` with cpp/int + literal | **OPTIMIZED** | `x / 6LL` |
| 7 | `if` with C++ bool condition | **PARTIAL** | Boxes bool, uses `truthy()` |
| 8 | `cpp/float.` from literal | **OPTIMIZED** | `static_cast<float>(3.14)` |
| 9 | `cpp/double.` from int literal | **NOT OPT** | `convert<double>::from_object()` |
| 10 | Chained arithmetic | **OPTIMIZED** | Stays in C++ land |
| 11 | Mixed jank/cpp | **NOT OPT** | Falls back to `dynamic_call` |
| 12 | `and` with comparisons | **PARTIAL** | Ops optimized, `and` boxes |
| 13 | `not` with C++ bool | **NOT OPT** | `dynamic_call(not, ...)` |
| 14 | Pure jank `(+ 1 2)` | **CORRECT** | `dynamic_call` (expected) |
| 15 | `inc` with cpp/int | **NOT OPT** | `dynamic_call(inc, ...)` |
| 16 | `dec` with cpp/int | **NOT OPT** | `dynamic_call(dec, ...)` |
| 17 | `>=` with cpp/int | **OPTIMIZED** | `x >= 0LL` |
| 18 | `=` with cpp/int | **NOT OPT** | `dynamic_call(=, ...)` |
| 19 | `zero?` with cpp/int | **NOT OPT** | `dynamic_call(zero?, ...)` |
| 20 | `pos?` with cpp/int | **NOT OPT** | `dynamic_call(pos?, ...)` |

## What's Working

1. **Binary arithmetic operators** (`+`, `-`, `*`, `/`) with C++ numeric types
2. **Comparison operators** (`>`, `<`, `>=`, `<=`, `=`) with C++ numeric types
3. **Chained arithmetic** stays in C++ land (e.g., `(+ (+ x 1) 2)`)
4. **Constant folding** for same-type conversions (`cpp/float.` from float literal)

## Optimization Opportunities

### High Priority

1. **`if` with C++ bool condition**
   - Current: `if(jank::runtime::truthy(convert<bool>::into_object(cpp_bool)))`
   - Should be: `if(cpp_bool)`
   - Impact: Every comparison-based conditional

2. **`not` with C++ bool**
   - Current: `dynamic_call(not, boxed_bool)`
   - Should be: `!cpp_bool`
   - Impact: Boolean logic

3. **`inc`/`dec` with C++ numeric**
   - Current: `dynamic_call(inc, boxed_x)`
   - Should be: `x + 1` / `x - 1`
   - Impact: Loop counters, iteration

4. **`and`/`or` with C++ bools**
   - Current: Boxes intermediate values, uses `truthy()`
   - Should be: Direct `&&` / `||` on C++ bools

### Medium Priority

5. **Cross-type constant folding**
   - Current: `cpp/double.` from int uses `convert<double>::from_object()`
   - Should be: `static_cast<double>(42)`

6. **Mixed jank/cpp arithmetic**
   - Current: Falls back to `dynamic_call` if any arg is jank
   - Could: Convert jank literals to C++ at compile time

### Lower Priority

7. **`zero?`** - could be `x == 0`
8. **`pos?`** - could be `x > 0`
9. **`neg?`** - could be `x < 0`
10. **Bitwise operators** (`bit-and`, `bit-or`, `bit-xor`, `bit-shift-left`, etc.)
11. **`min`/`max`** with C++ types
12. **`abs`** with C++ types

## Additional Notes

- **`>=` and `<=`** ARE optimized (tested and confirmed)
- **`=` (equality)** is NOT optimized - but note that Clojure `=` is structural equality, not just numeric `==`. Would need `==` for numeric-only comparison optimization.

## Implemented Optimizations (Phase 6-7+)

### Phase 6: `if` with C++ bool (analyzer fix)
- **File**: `analyze/processor.cpp` in `analyze_if()`
- **Change**: Skip `apply_implicit_conversion` when condition is already a C++ bool
- **Result**: `if(cpp_bool)` instead of `if(truthy(boxed_bool))`

### Phase 7: `not` with C++ bool (analyzer fix)
- **File**: `analyze/processor.cpp` in `analyze_call()`
- **Change**: Look through `cpp_cast` with `into_object` policy to find underlying bool
- **Result**: `!cpp_bool` instead of `dynamic_call(not, boxed_bool)`

### Phase 8: Statement position for non-void C++ calls
- **File**: `codegen/processor.cpp` in `gen(cpp_call_ref)`
- **Change**: Skip capturing return value for non-void calls in statement position
- **Result**: `ImGui::Begin(...)` instead of `auto &&unused{ ImGui::Begin(...) }`

### DEFERRED: `inc`/`dec`/`zero?`/`pos?`/`neg?` optimizations
These were initially implemented but removed to avoid hardcoding function names in the analyzer.
Should be implemented in a later pass (optimization pass) instead.

## Test Command

```clojure
(require 'jank.compiler)
(println (jank.compiler/native-source '(let [x (cpp/int. 5)] (> x 0))))
```
