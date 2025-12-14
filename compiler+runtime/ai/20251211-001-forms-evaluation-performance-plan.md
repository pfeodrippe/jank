# Forms Evaluation Performance Testing & Improvement Plan

**Date:** 2025-12-11
**Goal:** Create performance tests for form evaluation and systematically improve evaluation performance

---

## Table of Contents

1. [Executive Summary](#executive-summary)
2. [Benchmark Results (2025-12-11)](#benchmark-results-2025-12-11)
3. [Current Architecture Overview](#current-architecture-overview)
4. [Phase 1: Performance Test Suite Design](#phase-1-performance-test-suite-design)
5. [Phase 2: Baseline Measurements](#phase-2-baseline-measurements)
6. [Phase 3: Performance Investigation Areas](#phase-3-performance-investigation-areas)
7. [Phase 4: Optimization Strategies](#phase-4-optimization-strategies)
8. [Implementation Plan](#implementation-plan)
9. [Appendix: Code References](#appendix-code-references)

---

## Executive Summary

This document outlines a comprehensive plan to:
1. **Create a performance test suite** for measuring form evaluation speed
2. **Establish baseline metrics** across different form types
3. **Identify bottlenecks** in the evaluation pipeline
4. **Propose optimizations** based on profiling data

---

## Benchmark Results (2025-12-11)

### Key Findings

**The benchmark suite has been implemented in `test/cpp/jank/perf/eval_benchmark.cpp`**

#### Phase Breakdown Summary

For expressions that **DON'T trigger JIT** (primitives, vectors, arithmetic without `let`):
- **LEX**: ~0% (negligible)
- **PARSE**: ~35-53%
- **ANALYZE**: ~47-65%
- **EVAL**: ~0.1-3%

For expressions that **TRIGGER JIT** (`let`, `fn`, `loop`, `defn`):
- **LEX**: ~0% (negligible)
- **PARSE**: ~0.3%
- **ANALYZE**: ~2-3%
- **EVAL**: **~97-98%** ← This is the bottleneck!

#### EVAL Phase Detailed Breakdown

Within the EVAL phase for JIT-triggering expressions:
- **CODEGEN (C++ generation)**: ~0.001% (~375-600 ns) ← Essentially free!
- **JIT_DECL (clang declaration)**: ~2-5% (~1-3 ms)
- **JIT_EXPR (clang execution)**: **~95-98%** (~40-70 ms) ← Main bottleneck!

#### Sample Results

| Expression | Total Time | JIT_EXPR % |
|-----------|-----------|------------|
| `(let [x 1] x)` | ~50ms | 97.5% |
| `((fn [x] x) 5)` | ~45ms | 96.5% |
| `(defn add1 [x] (+ x 1))` | ~70ms | 97.3% |
| Large defn with nested let/if | ~75ms | 96.4% |

#### Warm vs Cold Comparison

| Operation | Cold (first call) | Warm (pre-compiled) |
|-----------|-------------------|---------------------|
| `(+ 1 2)` | ~130-230μs | ~90ns |
| `(let [x 1] x)` | ~45ms | N/A (always JIT) |
| `(identity 5)` | ~20ns | ~20ns |

#### Core Function Call Overhead (Warm)

| Function | Time per call |
|----------|---------------|
| `(identity 5)` | ~20ns |
| `(inc 5)` | ~65ns |
| `(+ 1 2)` | ~90ns |
| `(first [1 2 3])` | ~90ns |
| `(get {:a 1} :a)` | ~80ns |
| `(conj [1 2 3] 4)` | ~400ns |
| `(assoc {:a 1} :c 3)` | ~170ns |

### Key Conclusions

1. **JIT Expression Compilation is the bottleneck** - 95-98% of EVAL time
2. **Codegen is essentially free** - <1μs for any expression
3. **Parsing and Analysis are fast** - only 2-3% for JIT expressions
4. **Once compiled, function calls are very fast** - 20-400ns range
5. **Optimization focus should be on JIT compilation strategy**

### Potential Optimization Strategies

1. **Lazy JIT**: Don't JIT immediately, interpret first N calls
2. **Tiered compilation**: Fast interpreter → JIT on hot paths
3. **Expression caching**: Cache JIT'd expressions by structure hash
4. **PCH improvements**: Better precompiled header usage
5. **Clang optimization flags**: Tune for faster compilation over faster code

### Key Insight from Codebase Analysis

The jank evaluation pipeline has **5 distinct phases** that can be measured and optimized independently:

```
[Source Code]
     |
     v
[1. LEX] --> lex::processor (read/lex.cpp)
     |
     v
[2. PARSE] --> parse::processor (read/parse.cpp)
     |
     v
[3. ANALYZE] --> analyze::processor (analyze/processor.cpp)
     |
     v
[4. OPTIMIZE] --> analyze::pass::optimize (analyze/pass/*.cpp)
     |
     v
[5. EVAL/CODEGEN+JIT] --> evaluate::eval OR codegen + JIT (evaluate.cpp, codegen/*.cpp, jit/*.cpp)
```

---

## Current Architecture Overview

### Evaluation Entry Points

**File:** `compiler+runtime/src/cpp/jank/runtime/context.cpp`

```cpp
// Main entry: context::eval_string (lines 171-395)
object_ref context::eval_string(jtl::immutable_string_view const &code,
                                 usize const start_line,
                                 usize const start_col)
{
    profile::timer const timer{ "rt eval_string" };
    read::lex::processor l_prc{ code, start_line, start_col };
    read::parse::processor p_prc{ l_prc.begin(), l_prc.end() };

    for(auto const &form : p_prc)
    {
        auto const parsed_form = form.expect_ok().unwrap().ptr;

        profile::enter("phase:analyze");
        analyze::processor an_prc;
        auto expr = an_prc.analyze(parsed_form, ...).expect_ok();
        profile::exit("phase:analyze");

        profile::enter("phase:optimize");
        expr = analyze::pass::optimize(expr);
        profile::exit("phase:optimize");

        profile::enter("phase:eval");
        ret = evaluate::eval(expr);
        profile::exit("phase:eval");
    }
    return ret;
}
```

### Evaluation Dispatch

**File:** `compiler+runtime/src/cpp/jank/evaluate.cpp`

The evaluator has **34 overloaded `eval` functions** for different expression types:

| Expression Type | Evaluation Strategy | Performance Impact |
|-----------------|--------------------|--------------------|
| `expr::primitive_literal` | Direct return | Very fast |
| `expr::var_deref` | Var lookup + deref | Medium (involves map lookup) |
| `expr::call` | Dynamic dispatch | Slow (virtual calls + args eval) |
| `expr::function` | JIT compilation | Very slow (C++ codegen + clang) |
| `expr::let`, `expr::loop` | Wrapped + JIT | Very slow |
| `expr::if_` | Condition eval + branch | Medium |
| `expr::do_` | Sequential eval | Medium |
| `expr::cpp_*` | JIT required | Slow |

### Existing Performance Infrastructure

| Component | Location | Description |
|-----------|----------|-------------|
| `profile::timer` | `src/cpp/jank/profile/time.cpp` | RAII timer with ns precision |
| `profile::enter/exit` | `include/cpp/jank/profile/time.hpp` | Manual region profiling |
| `nanobench` | `third-party/nanobench/` | Microbenchmarking library |
| `perf::benchmark` | `src/cpp/jank/runtime/perf.cpp` | Exposed as `jank.perf-native/benchmark` |

---

## Phase 1: Performance Test Suite Design

### 1.1 Test File Structure

Create new test file: `compiler+runtime/test/cpp/jank/perf/eval_benchmark.cpp`

```cpp
#include <chrono>
#include <doctest/doctest.h>
#include <nanobench.h>
#include <jank/runtime/context.hpp>
#include <jank/profile/time.hpp>

namespace jank::perf
{
  using namespace jank::runtime;
  using namespace std::chrono;

  // Helper to measure eval time
  struct eval_timer
  {
    jtl::immutable_string label;
    high_resolution_clock::time_point start;

    eval_timer(jtl::immutable_string_view l)
      : label{l}, start{high_resolution_clock::now()} {}

    ~eval_timer() {
      auto end = high_resolution_clock::now();
      auto ns = duration_cast<nanoseconds>(end - start).count();
      std::cout << "[PERF] " << label << ": " << ns << " ns\n";
    }
  };

  // Phase timing helper
  struct phase_breakdown
  {
    i64 lex_ns{0};
    i64 parse_ns{0};
    i64 analyze_ns{0};
    i64 optimize_ns{0};
    i64 eval_ns{0};
    i64 total_ns{0};
  };

  phase_breakdown measure_phases(jtl::immutable_string_view code);
}
```

### 1.2 Benchmark Categories

#### Category A: Primitive Literals
```clojure
;; A1: Numbers
42
3.14
1000000000000N

;; A2: Strings
"hello world"

;; A3: Keywords
:keyword
:namespaced/keyword

;; A4: Symbols
'symbol
'namespace/symbol

;; A5: Collections (empty)
[]
{}
#{}
()
```

#### Category B: Simple Expressions
```clojure
;; B1: Arithmetic
(+ 1 2)
(+ 1 2 3 4 5)
(* (+ 1 2) (- 3 4))

;; B2: Comparisons
(= 1 1)
(< 1 2 3)

;; B3: Logic
(and true false)
(or nil 42)
(not false)
```

#### Category C: Collection Operations
```clojure
;; C1: Vector operations
(conj [1 2 3] 4)
(nth [1 2 3 4 5] 2)
(count [1 2 3 4 5])

;; C2: Map operations
(get {:a 1 :b 2} :a)
(assoc {:a 1} :b 2)

;; C3: Sequence operations
(first [1 2 3])
(rest [1 2 3])
(map inc [1 2 3])
```

#### Category D: Control Flow
```clojure
;; D1: If expression
(if true 1 2)
(if (> 5 3) "yes" "no")

;; D2: Cond
(cond
  (< x 0) "negative"
  (> x 0) "positive"
  :else "zero")

;; D3: Case
(case x
  1 "one"
  2 "two"
  "other")
```

#### Category E: Bindings
```clojure
;; E1: Let
(let [x 1] x)
(let [x 1 y 2] (+ x y))
(let [a 1 b 2 c 3 d 4 e 5] (+ a b c d e))

;; E2: Nested let
(let [x 1]
  (let [y 2]
    (+ x y)))
```

#### Category F: Functions
```clojure
;; F1: Anonymous function call
((fn [x] (+ x 1)) 5)

;; F2: Def + call
(do (def add1 (fn [x] (+ x 1)))
    (add1 5))

;; F3: Multi-arity
((fn ([x] x) ([x y] (+ x y))) 1 2)

;; F4: Variadic
((fn [& args] (apply + args)) 1 2 3 4 5)
```

#### Category G: Recursion
```clojure
;; G1: Simple recursion (fib)
(do
  (def fib (fn [n]
    (if (<= n 1)
      n
      (+ (fib (- n 1)) (fib (- n 2))))))
  (fib 10))

;; G2: Loop/recur
(loop [i 0 sum 0]
  (if (< i 100)
    (recur (inc i) (+ sum i))
    sum))
```

#### Category H: Macro Expansion
```clojure
;; H1: When
(when true 42)

;; H2: Threading
(-> 1 inc inc inc)
(->> [1 2 3] (map inc) (filter even?))

;; H3: Let destructuring
(let [{:keys [a b]} {:a 1 :b 2}] (+ a b))
```

### 1.3 Test Implementation Pattern

```cpp
TEST_SUITE("Eval Performance")
{
  TEST_CASE("primitive literals")
  {
    ankerl::nanobench::Bench bench;
    bench.minEpochIterations(1000);

    bench.run("integer", [&] {
      auto result = __rt_ctx->eval_string("42");
      ankerl::nanobench::doNotOptimizeAway(result);
    });

    bench.run("keyword", [&] {
      auto result = __rt_ctx->eval_string(":test");
      ankerl::nanobench::doNotOptimizeAway(result);
    });

    bench.run("vector", [&] {
      auto result = __rt_ctx->eval_string("[1 2 3 4 5]");
      ankerl::nanobench::doNotOptimizeAway(result);
    });
  }

  TEST_CASE("function calls")
  {
    ankerl::nanobench::Bench bench;
    bench.minEpochIterations(100);

    bench.run("(+ 1 2)", [&] {
      auto result = __rt_ctx->eval_string("(+ 1 2)");
      ankerl::nanobench::doNotOptimizeAway(result);
    });

    bench.run("(+ 1 2 3 4 5)", [&] {
      auto result = __rt_ctx->eval_string("(+ 1 2 3 4 5)");
      ankerl::nanobench::doNotOptimizeAway(result);
    });
  }

  TEST_CASE("phase breakdown")
  {
    // Measure time spent in each compilation phase
    auto breakdown = measure_phases("(let [x 1 y 2] (+ x y))");

    std::cout << "LEX:      " << breakdown.lex_ns << " ns\n";
    std::cout << "PARSE:    " << breakdown.parse_ns << " ns\n";
    std::cout << "ANALYZE:  " << breakdown.analyze_ns << " ns\n";
    std::cout << "OPTIMIZE: " << breakdown.optimize_ns << " ns\n";
    std::cout << "EVAL:     " << breakdown.eval_ns << " ns\n";
    std::cout << "TOTAL:    " << breakdown.total_ns << " ns\n";
  }
}
```

---

## Phase 2: Baseline Measurements

### 2.1 What to Measure

| Metric | Description | Tool |
|--------|-------------|------|
| **Total eval time** | End-to-end time for `eval_string` | nanobench |
| **Phase times** | Time per phase (lex, parse, analyze, optimize, eval) | profile::timer |
| **Memory allocations** | Number of heap allocations | heaptrack / Instruments |
| **JIT compilation time** | Time spent in CppInterOp | profile::timer |
| **Var lookup time** | Time to resolve vars | profile::timer |

### 2.2 Expected Bottlenecks

Based on codebase analysis, likely bottlenecks are:

1. **JIT Compilation** (for let, loop, fn)
   - Wrapping expressions in functions
   - C++ codegen string building
   - CppInterOp parsing and compilation

2. **Var Resolution**
   - Synchronized map access
   - Multiple lookups for qualified symbols

3. **Boxing/Unboxing**
   - Primitive operations returning boxed values
   - Collection operations

4. **Dynamic Dispatch**
   - `dynamic_call` with 11+ argument overloads
   - `visit_object` pattern matching

### 2.3 Baseline Test Script

```bash
#!/bin/bash
# Run from compiler+runtime directory

echo "=== jank Performance Baseline ==="
echo "Date: $(date)"
echo "Commit: $(git rev-parse --short HEAD)"
echo ""

# Run with profiling enabled
./build/jank run --profile --profile-output baseline.profile -e '
(println "=== Primitive Literals ===")
(time (dotimes [_ 10000] 42))
(time (dotimes [_ 10000] :keyword))
(time (dotimes [_ 10000] [1 2 3]))

(println "=== Arithmetic ===")
(time (dotimes [_ 10000] (+ 1 2)))
(time (dotimes [_ 10000] (+ 1 2 3 4 5)))

(println "=== Let Bindings ===")
(time (dotimes [_ 1000] (let [x 1] x)))
(time (dotimes [_ 1000] (let [x 1 y 2 z 3] (+ x y z))))

(println "=== Functions ===")
(def add1 (fn [x] (+ x 1)))
(time (dotimes [_ 10000] (add1 5)))

(println "=== Collections ===")
(time (dotimes [_ 1000] (conj [1 2 3] 4)))
(time (dotimes [_ 1000] (get {:a 1 :b 2 :c 3} :b)))
'

# Analyze profile output
echo ""
echo "=== Profile Analysis ==="
cat baseline.profile | grep -E "enter|exit" | \
  awk '/enter/ {start[$3] = $2}
       /exit/  {if(start[$3]) print $3, $2 - start[$3], "ns"; delete start[$3]}' | \
  sort -k2 -rn | head -20
```

---

## Phase 3: Performance Investigation Areas

### 3.1 JIT Compilation Overhead

**Current Problem:** Every `let`, `loop`, and complex expression gets wrapped in a function and JIT compiled.

**File:** `compiler+runtime/src/cpp/jank/evaluate.cpp:172-250`

```cpp
// wrap_expression creates a new function around any expression
// This triggers full C++ codegen + JIT compilation
expr::function_ref wrap_expression(jtl::ref<E> const orig_expr,
                                   jtl::immutable_string const &name,
                                   native_vector<obj::symbol_ref> params)
{
    // Creates anonymous function
    // Sets up frame, captures, etc.
    // Result must be JIT compiled
}
```

**Investigation Questions:**
1. Can we cache compiled functions for repeated evals?
2. Can we interpret simple lets directly without JIT?
3. Can we batch multiple expressions into one JIT compilation?

### 3.2 Var Resolution

**Current Problem:** Var lookups go through synchronized maps.

**File:** `compiler+runtime/src/cpp/jank/runtime/context.cpp`

```cpp
var_ref context::find_var(obj::symbol_ref const sym)
{
    // Multiple map lookups:
    // 1. Find namespace
    // 2. Find var in namespace
    // Uses jtl::synchronized (mutex-based)
}
```

**Investigation Questions:**
1. Can we cache hot vars?
2. Can we use lock-free data structures?
3. Can we inline known core function calls?

### 3.3 Dynamic Call Dispatch

**File:** `compiler+runtime/src/cpp/jank/evaluate.cpp:342-498`

```cpp
object_ref eval(expr::call_ref const expr)
{
    auto source(eval(expr->source_expr));
    // Dereference vars
    while(source->type == object_type::var) { source = deref(source); }

    // Dynamic dispatch via visit_object
    return visit_object([&](auto const typed_source) -> object_ref {
        // Type checking at runtime
        // Argument count checking
        // Multiple switch cases for 0-10+ args
    }, source);
}
```

**Investigation Questions:**
1. Can we specialize common call patterns?
2. Can we inline core functions at analyze time?
3. Can we avoid the visit_object pattern for known types?

### 3.4 Boxing Overhead

**Current Problem:** All values are boxed (`object_ref`), even primitives in tight loops.

**Existing Work:** The `needs_box` flag and type tracking in `local_binding` suggests unboxing support is partially implemented.

**Investigation Questions:**
1. How complete is the unboxing infrastructure?
2. Can we avoid boxing for loop counters?
3. Can we specialize arithmetic for primitive types?

### 3.5 Collection Operations

**File:** `compiler+runtime/src/cpp/jank/evaluate.cpp:510-608`

**Investigation Questions:**
1. Are persistent data structures causing overhead?
2. Can we use transients more aggressively?
3. Are there unnecessary copies?

---

## Phase 4: Optimization Strategies

### 4.1 Quick Wins (Low Effort, Measurable Impact)

#### Q1: Interpreter for Simple Expressions
Instead of JIT compiling every let, implement direct interpretation:

```cpp
// Add to evaluate.cpp
object_ref eval_let_interpreted(expr::let_ref const expr)
{
    // Simple stack-based evaluation without JIT
    native_hash_map<obj::symbol_ref, object_ref> bindings;
    for(auto const &binding : expr->bindings) {
        bindings[binding.sym] = eval(binding.value);
    }
    // Evaluate body with bindings available
    // ...
}
```

**Expected Impact:** 10-100x faster for simple lets

#### Q2: Var Cache
Cache frequently accessed vars:

```cpp
// Hot var cache (lockless)
inline object_ref get_cached_var(obj::symbol_ref sym) {
    static thread_local native_hash_map<obj::symbol_ref, var_ref> cache;
    auto it = cache.find(sym);
    if(it != cache.end()) return it->second->deref();
    // Fall back to full lookup, populate cache
}
```

**Expected Impact:** 2-5x faster var resolution

#### Q3: Inline Core Functions
Recognize and inline calls to core functions:

```cpp
// In analyze::processor::analyze_call
if(is_core_function(callee, "+")) {
    // Instead of (+ a b), generate direct addition
    return make_builtin_add_expr(args);
}
```

**Expected Impact:** 5-20x faster for arithmetic

### 4.2 Medium Effort Optimizations

#### M1: Expression Caching
Cache compiled expressions by hash:

```cpp
struct eval_cache {
    native_hash_map<size_t, object_ref(*)(context*)> compiled;

    object_ref eval_cached(jtl::immutable_string_view code) {
        auto hash = std::hash<jtl::immutable_string_view>{}(code);
        if(auto it = compiled.find(hash); it != compiled.end()) {
            return it->second(__rt_ctx);
        }
        // Compile and cache
    }
};
```

#### M2: Batch JIT Compilation
Compile multiple expressions together:

```cpp
// Instead of compiling each form separately,
// batch them into one C++ compilation unit
void batch_compile(native_vector<expression_ref> exprs) {
    std::string combined_cpp;
    for(auto& expr : exprs) {
        combined_cpp += codegen(expr);
    }
    jit_compile(combined_cpp);
}
```

#### M3: Specialized Call Sites
Track and specialize hot call sites:

```cpp
struct call_site {
    object_ref target;
    size_t call_count{0};
    object_ref (*specialized_fn)(object_ref*);

    object_ref invoke(object_ref* args, size_t n) {
        if(++call_count > 1000 && !specialized_fn) {
            specialize();
        }
        return specialized_fn ? specialized_fn(args) : dynamic_call(target, args, n);
    }
};
```

### 4.3 High Effort Optimizations

#### H1: Bytecode Interpreter
Add an optional bytecode interpreter for non-JIT scenarios:

```cpp
enum class opcode : uint8_t {
    LOAD_CONST,
    LOAD_VAR,
    STORE_LOCAL,
    LOAD_LOCAL,
    CALL,
    RETURN,
    // ...
};

object_ref interpret(bytecode const& bc) {
    object_ref stack[256];
    size_t sp = 0;
    for(auto op : bc.ops) {
        switch(op) {
            case opcode::LOAD_CONST: stack[sp++] = bc.consts[bc.read_u16()]; break;
            // ...
        }
    }
    return stack[0];
}
```

#### H2: LLVM IR Direct Generation
Currently jank generates C++ which is then compiled by Clang. Direct LLVM IR generation could be faster:

**Note:** This is partially implemented in `codegen/llvm_processor.cpp`

#### H3: Profile-Guided Optimization
Use profiling data to optimize hot paths:

```cpp
// During profiled runs, collect:
// - Hot functions
// - Likely branch directions
// - Type distributions at call sites

// Use this data to:
// - Inline hot functions
// - Reorder branches
// - Specialize call sites
```

---

## Implementation Plan

### Week 1: Test Infrastructure

1. **Create benchmark test file**
   - File: `test/cpp/jank/perf/eval_benchmark.cpp`
   - Categories A-H from above
   - Integration with nanobench

2. **Create phase timing utilities**
   - Extend `profile::timer` for phase breakdown
   - Add reporting utilities

3. **Create baseline script**
   - Shell script for reproducible measurements
   - Output format for tracking over time

### Week 2: Baseline & Profiling

1. **Run baseline measurements**
   - All benchmark categories
   - Multiple runs for statistical significance

2. **Instruments profiling**
   - CPU profiling with xctrace
   - Identify hot functions

3. **Profile analysis**
   - Parse `jank.profile` output
   - Identify phase bottlenecks

### Week 3-4: Quick Wins

1. **Implement interpreter for simple lets** (Q1)
   - Prototype in evaluate.cpp
   - Benchmark comparison

2. **Implement var cache** (Q2)
   - Thread-local cache
   - Cache invalidation strategy

3. **Inline core arithmetic** (Q3)
   - Recognize +, -, *, / in analyze
   - Generate optimized code

### Week 5+: Medium Optimizations

Based on profiling data, prioritize:
- Expression caching (M1)
- Batch compilation (M2)
- Call site specialization (M3)

---

## Appendix: Code References

### Key Files

| File | Purpose |
|------|---------|
| `src/cpp/jank/runtime/context.cpp:171-395` | Main eval_string implementation |
| `src/cpp/jank/evaluate.cpp` | Expression evaluation (34 overloads) |
| `src/cpp/jank/analyze/processor.cpp` | Semantic analysis |
| `src/cpp/jank/codegen/processor.cpp` | C++ code generation |
| `src/cpp/jank/jit/processor.cpp` | JIT compilation via CppInterOp |
| `src/cpp/jank/profile/time.cpp` | Profiling infrastructure |
| `src/cpp/jank/runtime/perf.cpp` | Benchmark function |

### Key Functions

| Function | Location | Purpose |
|----------|----------|---------|
| `context::eval_string` | context.cpp:171 | Main entry point |
| `evaluate::eval` | evaluate.cpp:301 | Expression dispatch |
| `evaluate::wrap_expression` | evaluate.cpp:172 | JIT wrapper |
| `analyze::processor::analyze` | processor.cpp | Semantic analysis |
| `codegen::processor::declaration_str` | processor.cpp | C++ generation |
| `jit::processor::eval_string` | processor.cpp:280 | JIT compilation |

### Profile Regions

Current profile regions in the codebase:
- `"rt eval_string"` - Total eval time
- `"phase:analyze"` - Analysis phase
- `"phase:optimize"` - Optimization phase
- `"phase:eval"` - Evaluation phase
- `"eval ast node"` - Single AST node eval
- `"eval:def"` - Def evaluation
- `"eval:function"` - Function JIT
- `"eval:fn:llvm_gen"` - LLVM IR generation
- `"eval:fn:llvm_load"` - LLVM module loading
- `"eval:fn:cpp_jit_decl"` - C++ decl JIT
- `"eval:fn:cpp_jit_expr"` - C++ expr JIT
- `"jit eval_string"` - JIT string compilation

---

## Next Steps

1. **Review this plan** - Discuss priorities with team
2. **Create benchmark file** - Start with Categories A-C
3. **Run baseline** - Get initial numbers
4. **Profile with Instruments** - Identify actual bottlenecks
5. **Iterate** - Implement optimizations based on data

---

*This document should be updated as we progress through the phases and learn more about actual performance characteristics.*
