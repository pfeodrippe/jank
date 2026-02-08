# JIT Expression Compilation Performance Optimization Plan

**Date:** 2025-12-11
**Goal:** Improve JIT expression compilation performance (currently 95-98% of eval time)

---

## Table of Contents

1. [Executive Summary](#executive-summary)
2. [Current Architecture Analysis](#current-architecture-analysis)
3. [Bottleneck Analysis](#bottleneck-analysis)
4. [Strategy 0: Incremental JIT - Only Modified Forms](#strategy-0-incremental-jit---only-modified-forms)
5. [Optimization Strategies](#optimization-strategies)
6. [Implementation Priorities](#implementation-priorities)
7. [Detailed Implementation Plans](#detailed-implementation-plans)
8. [Appendix: Code References](#appendix-code-references)

---

## Executive Summary

### The Problem

Based on benchmarks (see `ai/20251211-001-forms-evaluation-performance-plan.md`):

| Phase | Time % |
|-------|--------|
| LEX | ~0% |
| PARSE | ~0.3% |
| ANALYZE | ~2-3% |
| **EVAL** | **~97-98%** |

Within EVAL for JIT-triggering expressions (`let`, `fn`, `loop`, `defn`):

| Sub-phase | Time % | Time |
|-----------|--------|------|
| CODEGEN (C++ generation) | ~0.001% | ~500ns |
| JIT_DECL (clang declaration) | ~2-5% | ~1-3ms |
| **JIT_EXPR (clang execution)** | **~95-98%** | **~40-70ms** |

**Root Cause:** `clang::Interpreter::ParseAndExecute` is slow because it:
1. Parses C++ source code (lexer + parser + semantic analysis)
2. Generates LLVM IR
3. Optimizes the IR
4. JIT compiles via LLVM ORC
5. Executes the code

### Constraint: Must Use C++ Codegen

jank uses **C++ codegen** (`codegen::processor`) for JIT compilation because:
- C++ interop features require full C++ parsing
- Template instantiation for jank's type system
- Access to C++ headers and libraries
- Features that cannot be expressed in LLVM IR directly

**Therefore, all optimizations must work within the C++ codegen path.**

---

## Current Architecture Analysis

### JIT Processor (`src/cpp/jank/jit/processor.cpp`)

```cpp
processor::processor()
{
    // Creates clang::Interpreter with these flags:
    // JANK_JIT_FLAGS = jank_common_compiler_flags + jank_jit_compiler_flags + includes

    // jank_jit_compiler_flags:
    // -w -fwrapv -fno-stack-protector -fPIC

    // Debug: -O0 -g
    // Release: -O2 -DNDEBUG
}

void processor::eval_string(jtl::immutable_string_view const &code)
{
    interpreter->ParseAndExecute({ code.data(), code.size() });
}
```

### Evaluation Flow (`src/cpp/jank/evaluate.cpp:616-691`)

For function expressions (which trigger JIT):

```cpp
object_ref eval(expr::function_ref const expr)
{
    // Path 1: LLVM IR (if --codegen llvm-ir)
    if(util::cli::opts.codegen == util::cli::codegen_type::llvm_ir)
    {
        codegen::llvm_processor cg_prc{ wrapped_expr, module, target };
        cg_prc.gen();           // Generate LLVM IR directly
        cg_prc.optimize();      // Run LLVM passes
        jit_prc.load_ir_module(cg_prc.get_module());  // Load into ORC JIT
        // Execute
    }

    // Path 2: C++ (default)
    codegen::processor cg_prc{ expr, module, target };
    jit_prc.eval_string(cg_prc.declaration_str());  // JIT_DECL (~2-5%)
    jit_prc.interpreter->ParseAndExecute(expr_str); // JIT_EXPR (~95-98%)
}
```

### Generated C++ Code Structure

The C++ codegen generates:
1. **Declaration** - Struct definition with captured variables and call operator
2. **Expression** - Instantiation and call of the struct

Example for `(let [x 1] x)`:
```cpp
// Declaration
namespace user {
    struct __let_123 {
        jank::runtime::object_ref operator()() {
            auto x = jank::runtime::make_box(1);
            return x;
        }
    };
}

// Expression
user::__let_123{}().erase()
```

---

## Bottleneck Analysis

### Why C++ JIT is Slow

1. **C++ Parsing** - Full C++ parser (one of the most complex parsers)
2. **Template Instantiation** - jank uses many templates (`make_box`, `oref`, etc.)
3. **Header Parsing** - Even with PCH, there's overhead
4. **Name Lookup** - Qualified name resolution in large namespaces
5. **LLVM IR Generation** - Clang frontend + IR emission
6. **Optimization** - LLVM passes (even -O0 has some passes)
7. **Machine Code Gen** - LLVM backend

### Where Time Goes in C++ JIT

Based on profiling, the breakdown within `ParseAndExecute`:
1. **Lexing/Parsing** (~10-15%) - Tokenizing and parsing C++
2. **Semantic Analysis** (~20-30%) - Type checking, template instantiation
3. **LLVM IR Generation** (~15-20%) - Clang CodeGen
4. **LLVM Optimization** (~10-15%) - Even -O0 has passes
5. **Machine Code Generation** (~20-30%) - LLVM backend, linking

### Optimization Opportunities Within C++ Codegen

1. **Reduce redundant compilation** - Cache and reuse compiled forms
2. **Batch compilations** - Multiple forms in single `ParseAndExecute`
3. **Simpler generated code** - Less template instantiation
4. **Better PCH utilization** - Pre-instantiate common templates
5. **Clang flags** - Trade runtime performance for faster compilation

---

## Strategy 0: Incremental JIT - Only Modified Forms

**This is the recommended FIRST APPROACH for improving performance.**

### The Problem: Redundant Recompilation

When reloading a file or namespace, jank currently:
1. Re-reads and re-parses ALL forms in the file
2. Re-analyzes ALL forms
3. **Re-JIT compiles ALL forms** (even unchanged ones!)

Example scenario:
```clojure
;; user.jank - 100 top-level forms
(def a 1)
(def b 2)
(defn foo [x] ...)      ; <-- Only this changed!
(defn bar [x] ...)
;; ... 96 more unchanged forms
```

Currently: 100 JIT compilations × ~50ms = **~5 seconds**
With incremental: 1 JIT compilation × ~50ms = **~50ms** (100x faster!)

### How It Works

1. **Track form identity** - Hash each top-level form by:
   - Form type (`def`, `defn`, `defmacro`, etc.)
   - Form name (the symbol being defined)
   - Form body hash (structure + literals)

2. **Maintain a compilation cache** - Map of:
   ```
   {namespace/name → {hash, compiled_fn_ptr, var_ref}}
   ```

3. **On file reload**:
   - Parse all forms (fast: ~0.3% of time)
   - For each top-level `def`/`defn`:
     - Compute form hash
     - Check cache: same hash? → Skip JIT, reuse existing code
     - Different hash? → JIT compile, update cache, update var root

4. **On var redefinition** (REPL):
   - Always JIT compile (user explicitly wants new version)
   - Update cache with new hash

### Implementation Details

#### Data Structures

```cpp
// In runtime/context.hpp or new file jit/incremental_cache.hpp

struct form_identity
{
    obj::symbol_ref name;           // e.g., user/foo
    u64 body_hash;                  // Hash of analyzed expression
    jtl::immutable_string source_file;
    size_t source_line;
};

struct compiled_form
{
    form_identity identity;
    llvm::orc::ExecutorAddr fn_addr;      // JIT'd function pointer
    var_ref var;                          // The var this defines
    std::chrono::steady_clock::time_point compiled_at;
};

struct incremental_jit_cache
{
    // namespace/name -> compiled form
    native_unordered_map<obj::symbol_ref, compiled_form> cache;

    // Check if form needs recompilation
    bool needs_recompile(obj::symbol_ref name, u64 body_hash) const;

    // Store compiled form
    void store(form_identity const& id, llvm::orc::ExecutorAddr addr, var_ref var);

    // Get cached function (returns none if not cached or hash mismatch)
    option<compiled_form const&> get(obj::symbol_ref name, u64 body_hash) const;

    // Invalidate all forms from a namespace
    void invalidate_namespace(obj::symbol_ref ns);

    // Invalidate single form
    void invalidate(obj::symbol_ref name);
};
```

#### Form Hashing

```cpp
// Hash an analyzed expression (structure + literals, not source positions)
u64 hash_expression(analyze::expression_ref expr)
{
    return visit_expression(
        [](auto const& e) -> u64 {
            using T = std::decay_t<decltype(e)>;

            u64 h = std::hash<std::type_index>{}(typeid(T));

            if constexpr(std::is_same_v<T, expr::def_ref>)
            {
                h = hash_combine(h, e->name->hash());
                h = hash_combine(h, hash_expression(e->value));
            }
            else if constexpr(std::is_same_v<T, expr::function_ref>)
            {
                for(auto const& arity : e->arities)
                {
                    h = hash_combine(h, arity.params.size());
                    h = hash_combine(h, hash_expression(arity.body));
                }
            }
            else if constexpr(std::is_same_v<T, expr::primitive_literal_ref>)
            {
                h = hash_combine(h, e->data->hash());
            }
            // ... handle all expression types

            return h;
        },
        expr
    );
}
```

#### Modified Evaluation Flow

```cpp
// In evaluate.cpp - for def expressions

object_ref eval(expr::def_ref const expr)
{
    auto const qualified_name = /* ns/name symbol */;
    auto const body_hash = hash_expression(expr->value);

    // Check incremental cache
    if(auto cached = __rt_ctx->jit_cache.get(qualified_name, body_hash))
    {
        // Cache hit! Just update the var's root if needed
        if(cached->var->deref() != /* current root */)
        {
            cached->var->bind_root(/* execute cached fn */);
        }
        return cached->var;
    }

    // Cache miss - need to JIT compile
    auto const result = jit_compile_and_eval(expr->value);

    // Store in cache for next time
    __rt_ctx->jit_cache.store(
        form_identity{qualified_name, body_hash, expr->source_file, expr->line},
        /* fn_addr */,
        result_var
    );

    return result;
}
```

### Integration Points

#### 1. File Loading (`runtime/module/loader.cpp`)

```cpp
void load_file(path const& file)
{
    auto const source = read_file(file);
    auto const forms = parse_all(source);

    for(auto const& form : forms)
    {
        auto const analyzed = analyze(form);

        // Skip JIT for cached forms
        if(is_def_form(analyzed))
        {
            auto const hash = hash_expression(analyzed);
            if(__rt_ctx->jit_cache.get(def_name(analyzed), hash))
            {
                continue; // Already compiled, skip
            }
        }

        eval(analyzed);
    }
}
```

#### 2. REPL Evaluation

```cpp
// In REPL, always recompile (user wants fresh version)
object_ref repl_eval(jtl::immutable_string_view code)
{
    auto const analyzed = analyze(parse(code));

    // Force recompilation in REPL
    __rt_ctx->jit_cache.invalidate(def_name(analyzed));

    return eval(analyzed);
}
```

#### 3. Namespace Reload

```cpp
void reload_namespace(obj::symbol_ref ns)
{
    // Option 1: Invalidate all (conservative)
    __rt_ctx->jit_cache.invalidate_namespace(ns);
    load_namespace(ns);

    // Option 2: Smart reload (only changed forms)
    // - implemented via the normal load path with cache checks
}
```

### Benefits

| Scenario | Before | After | Speedup |
|----------|--------|-------|---------|
| Reload 100-form file (1 changed) | 5000ms | 50ms | **100x** |
| Reload 100-form file (10 changed) | 5000ms | 500ms | **10x** |
| Reload 100-form file (all changed) | 5000ms | 5000ms | 1x |
| Initial load | 5000ms | 5000ms | 1x |
| REPL single form | 50ms | 50ms | 1x |

### Edge Cases to Handle

1. **Macro changes** - If a macro changes, all forms that use it must be recompiled
   - Solution: Track macro dependencies, invalidate dependents

2. **Inline function changes** - Similar to macros
   - Solution: Track inline dependencies

3. **Protocol/multimethod changes** - May affect dispatch
   - Solution: Invalidate all implementations when protocol changes

4. **Var metadata changes** - `^:private`, `^:dynamic`, etc.
   - Solution: Include metadata in hash

5. **Source position changes** - Line numbers change but code doesn't
   - Solution: Exclude source positions from hash (already proposed)

### Implementation Priority

**Phase 1: Basic Incremental JIT** (2-3 days)
- Form hashing (structure only)
- Simple cache (namespace/name → compiled_form)
- Skip JIT for unchanged `def`/`defn`

**Phase 2: Dependency Tracking** (3-5 days)
- Track macro usage
- Invalidate dependents on macro change
- Track inline function usage

**Phase 3: Smart Namespace Reload** (2-3 days)
- `(require ... :reload)` only recompiles changed forms
- CIDER/nREPL integration

---

## Additional Optimization Strategies

> **Note:** Strategy 0 (Incremental JIT) above is the recommended **first approach**.
> The strategies below provide additional optimizations.

### Strategy 1: Expression Batching (HIGH PRIORITY)

**Impact:** 2-5x faster for multiple expressions
**Effort:** Medium

Batch multiple small expressions into a single JIT compilation.

**Example:**
```clojure
;; Instead of 3 separate JIT compilations:
(def a 1)
(def b 2)
(def c (+ a b))

;; Batch into single compilation:
;; namespace user { struct __batch_1 { ... } }
```

**Implementation:**
1. Add expression queue in eval context
2. Flush on explicit request or when dependencies require
3. Single `ParseAndExecute` call for batch

### Strategy 2: Expression Caching (MEDIUM PRIORITY)

**Impact:** Instant for repeated expressions
**Effort:** Medium-High

Cache compiled functions by expression structure hash.

**Cache Key:** Hash of normalized expression AST
**Cache Value:** Function pointer or JIT symbol

**Example:**
```clojure
(let [x 1] x)     ;; First: compile (~50ms)
(let [y 2] y)     ;; Hit: structurally equivalent, just different binding name
```

**Implementation:**
1. Compute hash of expression structure (not literals)
2. Check cache before JIT
3. If hit, instantiate with different captures
4. Eviction policy based on LRU or memory pressure

### Strategy 3: Lazy JIT / Simple Interpreter (LOW-MEDIUM PRIORITY)

**Impact:** Very fast for simple expressions
**Effort:** High

For simple expressions, use direct interpretation instead of JIT.

**Fast Path Candidates:**
- Primitive literals: `1`, `"hello"`, `:keyword`
- Simple var derefs: `foo` (already fast)
- Simple calls with constant args: `(+ 1 2)`
- Vector/map literals: `[1 2 3]`, `{:a 1}`

**Implementation:**
1. Pattern-match on expression type
2. If "simple", interpret directly
3. Otherwise, fall back to JIT

### Strategy 4: PCH Optimization (MEDIUM PRIORITY)

**Impact:** 10-30% faster JIT
**Effort:** Low

Optimize precompiled header usage:

1. **Minimal PCH for eval** - Only include necessary headers
2. **Better PCH invalidation** - Don't regenerate unnecessarily
3. **PCH modules** - Use Clang's module system

**Current PCH:** `build/incremental.pch`

### Strategy 5: Clang Optimization Flags (LOW PRIORITY)

**Impact:** 5-20% faster compilation
**Effort:** Low

Tune Clang flags for faster compilation over faster code:

```cmake
# Current JIT flags
set(jank_jit_compiler_flags -w -fwrapv -fno-stack-protector -fPIC)

# Proposed additions for faster compilation:
-fno-exceptions              # If exceptions not needed in generated code
-fno-rtti                    # If RTTI not needed
-fno-unwind-tables           # Skip unwind info
-fno-asynchronous-unwind-tables
-Oz                          # Optimize for size (faster than -O2 to compile)
```

### Strategy 6: JIT Warm-up / Background Compilation (LOW PRIORITY)

**Impact:** Better perceived performance
**Effort:** Medium

Compile common patterns in background during startup.

**Pre-compile:**
- Common clojure.core functions
- Standard patterns (`let`, `if`, `fn`)
- User's frequently used expressions

---

## Implementation Priorities

### Phase 1: Incremental JIT (FIRST PRIORITY - 2-3 days)

**Goal:** Only recompile forms that actually changed

1. **Implement form hashing**
   - Hash analyzed expressions by structure + literals
   - Exclude source positions from hash
   - File: `src/cpp/jank/analyze/expression_hash.cpp` (new)

2. **Create incremental JIT cache**
   - Data structure: `namespace/name → {hash, fn_ptr, var}`
   - Store compiled forms on first JIT
   - File: `src/cpp/jank/jit/incremental_cache.cpp` (new)

3. **Modify def/defn evaluation**
   - Check cache before JIT compilation
   - Skip JIT if hash matches
   - File: `src/cpp/jank/evaluate.cpp`

4. **Add cache invalidation**
   - Invalidate on REPL redefinition
   - Invalidate namespace on reload
   - File: `src/cpp/jank/runtime/context.cpp`

**Expected Impact:** 10-100x faster namespace reloads when few forms change

### Phase 2: Quick Wins (1-2 days)

1. **Add compilation flags for faster JIT**
   - `-fno-unwind-tables -fno-asynchronous-unwind-tables`
   - Test impact on compilation speed
   - File: `CMakeLists.txt:309-314`

2. **Profile ParseAndExecute breakdown**
   - Identify which Clang phase is slowest
   - File: `test/cpp/jank/perf/eval_benchmark.cpp`

### Phase 3: Expression Batching (3-5 days)

1. **Expression queue implementation**
   - Queue multiple def/defn forms
   - Flush when dependencies require or explicitly
   - File: `src/cpp/jank/evaluate.cpp`

2. **Batched code generation**
   - Generate single C++ block for multiple forms
   - Single `ParseAndExecute` call
   - File: `src/cpp/jank/codegen/processor.cpp`

**Expected Impact:** 2-5x faster for loading files with many small forms

### Phase 4: PCH Optimization (2-3 days)

1. **Pre-instantiate common templates in PCH**
   - `make_box<integer>`, `make_box<persistent_string>`, etc.
   - Explicit instantiation declarations
   - File: `include/cpp/jank/precompiled.hpp`

2. **Minimize generated code template usage**
   - Use pre-instantiated versions where possible
   - File: `src/cpp/jank/codegen/processor.cpp`

**Expected Impact:** 10-30% faster JIT compilation

### Phase 5: Interpreter Fast Path (2-3 weeks)

1. **Pattern matching for simple expressions**
2. **Direct interpretation logic**
3. **Fallback to JIT for complex cases**

**Expected Impact:** Near-instant for simple expressions (literals, var derefs)

---

## Detailed Implementation Plans

### Plan A: Incremental JIT Implementation (FIRST PRIORITY)

**Step 1: Create expression hashing utility**

File: `src/cpp/jank/analyze/expression_hash.cpp`
```cpp
#include <jank/analyze/expression_hash.hpp>
#include <jank/analyze/visit.hpp>

namespace jank::analyze
{
    namespace
    {
        u64 hash_combine(u64 seed, u64 value)
        {
            // boost::hash_combine algorithm
            return seed ^ (value + 0x9e3779b9 + (seed << 6) + (seed >> 2));
        }
    }

    u64 hash_expression(expression_ref const expr)
    {
        return visit_expression(
            [](auto const& e) -> u64 {
                using T = std::decay_t<decltype(e)>;
                u64 h = typeid(T).hash_code();

                if constexpr(std::is_same_v<T, expr::def_ref>)
                {
                    h = hash_combine(h, e->name->hash());
                    h = hash_combine(h, hash_expression(e->value));
                }
                else if constexpr(std::is_same_v<T, expr::function_ref>)
                {
                    h = hash_combine(h, e->unique_name.hash());
                    for(auto const& arity : e->arities)
                    {
                        h = hash_combine(h, arity.params.size());
                        for(auto const& param : arity.params)
                        {
                            h = hash_combine(h, param.name->hash());
                        }
                        h = hash_combine(h, hash_expression(arity.body));
                    }
                }
                else if constexpr(std::is_same_v<T, expr::primitive_literal_ref>)
                {
                    h = hash_combine(h, runtime::hash(e->data));
                }
                else if constexpr(std::is_same_v<T, expr::var_deref_ref>)
                {
                    h = hash_combine(h, e->var->n->to_hash());
                }
                else if constexpr(std::is_same_v<T, expr::call_ref>)
                {
                    h = hash_combine(h, hash_expression(e->source));
                    for(auto const& arg : e->args)
                    {
                        h = hash_combine(h, hash_expression(arg));
                    }
                }
                else if constexpr(std::is_same_v<T, expr::let_ref>)
                {
                    for(auto const& binding : e->bindings)
                    {
                        h = hash_combine(h, binding.first.name->hash());
                        h = hash_combine(h, hash_expression(binding.second));
                    }
                    h = hash_combine(h, hash_expression(e->body));
                }
                // ... handle all expression types similarly

                return h;
            },
            expr
        );
    }
}
```

**Step 2: Create incremental JIT cache**

File: `include/cpp/jank/jit/incremental_cache.hpp`
```cpp
#pragma once

#include <jank/runtime/object.hpp>
#include <jank/runtime/var.hpp>

namespace jank::jit
{
    struct compiled_def
    {
        u64 body_hash;
        runtime::var_ref var;
        // The compiled function is already stored in the var's root
    };

    struct incremental_cache
    {
        // qualified name (ns/sym) -> compiled def info
        native_unordered_map<runtime::obj::symbol_ref, compiled_def> defs;

        // Returns true if the def needs recompilation
        bool needs_recompile(runtime::obj::symbol_ref name, u64 body_hash) const
        {
            auto const it = defs.find(name);
            if(it == defs.end())
                return true; // Not cached
            return it->second.body_hash != body_hash; // Hash changed
        }

        // Store compiled def
        void store(runtime::obj::symbol_ref name, u64 body_hash, runtime::var_ref var)
        {
            defs[name] = compiled_def{body_hash, var};
        }

        // Get cached var (returns none if not cached or hash doesn't match)
        jtl::option<runtime::var_ref> get(runtime::obj::symbol_ref name, u64 body_hash) const
        {
            auto const it = defs.find(name);
            if(it == defs.end() || it->second.body_hash != body_hash)
                return jtl::none;
            return it->second.var;
        }

        // Invalidate a specific def
        void invalidate(runtime::obj::symbol_ref name)
        {
            defs.erase(name);
        }

        // Invalidate all defs in a namespace
        void invalidate_namespace(runtime::obj::symbol_ref ns)
        {
            for(auto it = defs.begin(); it != defs.end(); )
            {
                if(it->first->ns == ns->name)
                    it = defs.erase(it);
                else
                    ++it;
            }
        }

        // Clear entire cache
        void clear()
        {
            defs.clear();
        }
    };
}
```

**Step 3: Add cache to runtime context**

File: `include/cpp/jank/runtime/context.hpp`
```cpp
// Add member:
jit::incremental_cache jit_cache;
```

**Step 4: Modify def evaluation to use cache**

File: `src/cpp/jank/evaluate.cpp`
```cpp
object_ref eval(expr::def_ref const expr)
{
    auto const ns = expect_object<ns>(__rt_ctx->current_ns_var->deref());
    auto const qualified_name = make_box<obj::symbol>(ns->to_string(), expr->name->name);
    auto const body_hash = analyze::hash_expression(expr->value);

    // Check if we can reuse the cached version
    if(auto cached_var = __rt_ctx->jit_cache.get(qualified_name, body_hash))
    {
        // Cache hit! The var already has the correct root value
        // Just return the var
        return cached_var.unwrap();
    }

    // Cache miss - need to JIT compile
    // ... existing JIT compilation code ...

    // After successful compilation, store in cache
    __rt_ctx->jit_cache.store(qualified_name, body_hash, result_var);

    return result_var;
}
```

**Step 5: Add benchmark for incremental JIT**

File: `test/cpp/jank/perf/eval_benchmark.cpp`
```cpp
TEST_CASE("Incremental JIT: Reload Performance")
{
    std::cout << "\n=== Incremental JIT Benchmark ===\n";

    // Define 10 functions
    for(int i = 0; i < 10; i++)
    {
        auto code = util::format("(defn test-fn-{} [x] (+ x {}))", i, i);
        __rt_ctx->eval_string(code);
    }

    // Reload all (should be cached)
    auto start = high_resolution_clock::now();
    for(int i = 0; i < 10; i++)
    {
        auto code = util::format("(defn test-fn-{} [x] (+ x {}))", i, i);
        __rt_ctx->eval_string(code);
    }
    auto cached_time = duration_cast<milliseconds>(high_resolution_clock::now() - start).count();

    // Modify one function (force recompile)
    __rt_ctx->jit_cache.invalidate(/* test-fn-5 symbol */);
    start = high_resolution_clock::now();
    __rt_ctx->eval_string("(defn test-fn-5 [x] (+ x 100))");
    auto modified_time = duration_cast<milliseconds>(high_resolution_clock::now() - start).count();

    std::cout << "Reload 10 cached forms: " << cached_time << "ms (should be ~0)\n";
    std::cout << "Compile 1 modified form: " << modified_time << "ms\n";
}
```

### Plan B: Expression Caching

**Data Structures:**
```cpp
// In jit/cache.hpp
struct expression_cache
{
    struct entry
    {
        llvm::orc::ExecutorAddr fn_addr;
        std::chrono::steady_clock::time_point last_used;
    };

    // Hash of expression structure -> compiled function
    native_unordered_map<u64, entry> cache;

    option<llvm::orc::ExecutorAddr> lookup(u64 hash);
    void store(u64 hash, llvm::orc::ExecutorAddr addr);
    void evict_lru(size_t count);
};

// Expression hashing (structure only, not literals)
u64 hash_expression_structure(analyze::expression_ref expr);
```

### Plan C: Interpreter Fast Path

**Pattern matching:**
```cpp
// In evaluate.cpp
bool is_simple_expression(analyze::expression_ref expr)
{
    return visit_expression(
        [](auto const &e) -> bool {
            using T = std::decay_t<decltype(e)>;
            if constexpr(std::is_same_v<T, expr::primitive_literal_ref>)
                return true;
            if constexpr(std::is_same_v<T, expr::var_deref_ref>)
                return true;
            if constexpr(std::is_same_v<T, expr::vector_ref>)
                return e->values.size() < 10 && all_literals(e->values);
            // ... more patterns
            return false;
        },
        expr
    );
}

object_ref eval(analyze::expression_ref expr)
{
    if(is_simple_expression(expr))
        return interpret(expr);  // Fast path
    return jit_compile_and_eval(expr);  // Slow path
}
```

---

## Appendix: Code References

### Key Files

| File | Description |
|------|-------------|
| `src/cpp/jank/jit/processor.cpp` | JIT processor, creates Clang interpreter |
| `src/cpp/jank/evaluate.cpp` | Main eval dispatch, lines 600-800 |
| `src/cpp/jank/codegen/processor.cpp` | C++ code generation |
| `src/cpp/jank/codegen/llvm_processor.cpp` | LLVM IR code generation |
| `include/cpp/jank/util/cli.hpp` | CLI options including `codegen_type` |
| `CMakeLists.txt:308-320` | JIT compiler flags |
| `test/cpp/jank/perf/eval_benchmark.cpp` | Performance benchmarks |

### Key Functions

| Function | Location | Description |
|----------|----------|-------------|
| `processor::eval_string` | `jit/processor.cpp` | Calls `ParseAndExecute` |
| `eval(expr::function_ref)` | `evaluate.cpp:616` | Main JIT compilation entry |
| `codegen::processor::declaration_str` | `codegen/processor.cpp:2124` | C++ code generation |
| `llvm_processor::gen` | `codegen/llvm_processor.cpp` | LLVM IR generation |

### Related Documentation

- [Clang-Repl Documentation](https://clang.llvm.org/docs/ClangRepl.html)
- [LLVM ORC JIT](https://llvm.org/docs/ORCv2.html)
- jank profiling plan: `ai/20251208-001-profiling-plan.md`
- Performance baseline: `ai/20251211-001-forms-evaluation-performance-plan.md`

---

## Summary

**Most Promising Optimization:** Incremental JIT - Only recompile modified top-level forms.

This is the **first approach** because:
1. Provides massive speedup for the common case (namespace reload with few changes)
2. Works entirely within the C++ codegen path (required for full feature support)
3. No changes to compilation speed - just avoids unnecessary compilations
4. Relatively straightforward implementation (2-3 days)

**Expected Impact:**
- **10-100x faster** namespace reloads when few forms change
- Reload 100-form file with 1 change: ~50ms instead of ~5000ms

**Key Implementation Points:**
1. Hash analyzed expressions by structure (not source positions)
2. Cache: `namespace/name → {hash, var_ref}`
3. On `def`/`defn`: check hash, skip JIT if unchanged
4. Invalidate cache on REPL redefinition

**Next Steps:**
1. Implement expression hashing (`src/cpp/jank/analyze/expression_hash.cpp`)
2. Create incremental JIT cache (`include/cpp/jank/jit/incremental_cache.hpp`)
3. Modify `eval(expr::def_ref)` to use cache
4. Add benchmark to verify speedup
5. Consider additional optimizations (batching, PCH, flags) as Phase 2+

**Verifying Improvements:**

Run the eval benchmarks to check if performance improvements are occurring:

```bash
./build/jank-test --test-case="*Eval*"
```

This runs all eval-related benchmarks including:
- Phase Breakdown (lex → parse → analyze → eval)
- Eval Breakdown (codegen vs JIT_DECL vs JIT_EXPR)
- Warm vs Cold comparisons
- Var resolution overhead
- Object creation overhead

Compare the output before and after changes to verify improvements.
