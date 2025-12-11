# Further JIT Performance Optimizations Plan

**Date:** 2025-12-11
**Updated:** 2025-12-11 (after Clojure study and jank capability analysis)

**Context:** After implementing incremental JIT cache (Strategy 0), `vybe.type` namespace load time improved from ~7s to ~4s. This document outlines further optimizations based on studying Clojure's approach and jank's existing capabilities.

---

## Key Insight from Clojure Study

### Why Clojure is Fast

Clojure compiles to **JVM bytecode**, not C++:
- Bytecode generation is **trivially fast** (~0.1ms per form)
- No C++ parsing, no template instantiation
- JVM's JIT handles optimization at runtime
- AOT option pre-compiles to `.class` files for instant loading

### Why jank's C++ JIT is Slow

The bottleneck is **Clang parsing C++**, not code generation:
```
CODEGEN (generate C++ string):  ~0.0004ms  ← Fast!
JIT_DECL (Clang parses decls):  ~2-10ms    ← Slow (C++ parsing)
JIT_EXPR (Clang parses+exec):   ~20-45ms   ← Very slow (C++ parsing)
```

**Root cause:** Clang must parse C++ source text, which is one of the most complex parsing problems in computing.

---

## jank's Existing Capabilities (Discovered!)

jank already has infrastructure that could help:

| Capability | Status | Location |
|------------|--------|----------|
| Object file loading | ✅ Works | `module/loader.cpp` (checks for `.o` files) |
| Binary cache dir | ✅ Exists | `~/.cache/jank/<version>/` |
| JIT cache (memory) | ✅ Works | `jit/incremental_cache.hpp` |
| Save C++ | ✅ Works | `--save-cpp` flag |

### Missing Pieces (Opportunities)
| Gap | Impact |
|-----|--------|
| JIT cache not persisted to disk | Lose cache across sessions |
| No auto `.o` generation | Must use external toolchain |
| Binary cache not auto-checked | Not used for module loading |

### Constraint: C++ Codegen Required
We must use C++ codegen (not LLVM IR) because:
- C++ interop features require full C++ parsing
- Template instantiation for jank's type system
- Access to C++ headers and libraries

---

## Current State

### What's Already Done
- **Incremental JIT Cache (Strategy 0)**: Caches compiled defs by expression hash
  - Skip JIT for unchanged forms on reload
  - ~27-120x speedup for namespace reloads with unchanged code

### Current Bottleneck Analysis

From benchmark results, each form that triggers JIT takes:
| Phase | Time | % of Total |
|-------|------|------------|
| CODEGEN (C++ gen) | ~500ns | ~0.001% |
| JIT_DECL (clang decl) | ~2-10ms | ~4-20% |
| JIT_EXPR (clang exec) | ~40-55ms | ~80-95% |
| **Total per form** | **~45-65ms** | 100% |

**The ~45ms is almost entirely C++ parsing overhead, NOT optimization or codegen.**

### vybe.type Specific Analysis

**File:** `/Users/pfeodrippe/dev/something/target/classes/vybe/type.jank`
**Size:** 1381 lines

**Form Breakdown:**
| Form Type | Count | Est. Time Each | Total |
|-----------|-------|----------------|-------|
| `defn` / `defn-` | 36 | ~50ms | ~1.8s |
| `defmacro` | 2 | ~50ms | ~0.1s |
| `defonce` | 4 | ~50ms | ~0.2s |
| `cpp/raw` (large C++ blocks) | 3 | ~100-300ms | ~0.5-1s |
| **Total** | **~45 forms** | - | **~2.5-3s base** |

**Key Observations:**
1. **3 large `cpp/raw` blocks** with ~324 lines of C++ code
   - Line 6-144: VybeStructBuilder helper functions
   - Line 145-457: Field access helpers using meta cursor API
   - Line 539+: Additional C++ helpers
2. Many small `defn` forms (type manipulation, field access)
3. Uses Flecs ECS library with heavy C++ interop

**Why ~4s instead of ~3s?**
- Header parsing overhead for `flecs.h`
- Macro expansion for `cpp/raw`, `defmacro`
- Global state initialization (`defonce`, `atom`)

**Key Insight**: Each individual `ParseAndExecute` call has ~40ms fixed overhead regardless of code complexity.

**NOTE: Parallel JIT NOT POSSIBLE** - Functions depend on each other (forward references, sequential definitions).

---

## Optimization Strategies (Ranked by Impact)

### What Won't Work (and Why)

| Strategy | Why It Won't Work |
|----------|-------------------|
| **Expression Batching** | Each top-level form must be fully evaluated before the next can be analyzed (macros, vars, etc.) |
| **Parallel JIT** | Forms depend on each other - forward references, sequential definitions |
| **LLVM IR codegen** | Need C++ codegen for full C++ interop, templates, headers |
| **AST-level Caching** | See detailed analysis below |

### Why AST-Level Caching Won't Help jank

**The Idea:** Cache Clang's parsed AST to skip re-parsing the same C++ code.

**Expert Findings:**

1. **zapcc achieves 40x speedup** with AST caching - but required 200k+ lines of modified Clang code
   - Source: [zapcc GitHub](https://github.com/yrnkrn/zapcc)

2. **Clang's AST is mutable and not reusable:**
   > "Reusing the preamble ASTContext mutates it, which we can't undo. Parsing mutates referenced nodes (marking them 'used', adding redecl chains). There is no mechanism to reverse this."
   - Source: [clangd discussions](https://github.com/clangd/clangd/discussions/1240)

3. **Cling (ROOT CERN) uses incremental AST** but still requires 100+ patches to Clang
   - Source: [LLVM Blog on Cling](https://blog.llvm.org/posts/2020-11-30-interactive-cpp-with-cling/)

**Why It Doesn't Apply to jank:**

AST caching helps when parsing the **SAME C++ code repeatedly**. But jank generates **different C++ each time**:

```cpp
// First eval of (defn foo [x] (+ x 1))
struct __fn_abc123 : jfn { ... };  // unique_name = abc123

// Second eval of same code (e.g., reload)
struct __fn_def456 : jfn { ... };  // unique_name = def456 (different!)
```

The AST is **structurally different** each time, so AST caching wouldn't match.

**The Real Solution:**
Skip JIT entirely when the *expression* is unchanged → **Object file caching** (Strategy 1)

---

### Strategy 1: Persistent JIT Cache (HIGH IMPACT)

**Expected Impact:** Instant reloads across sessions (like Clojure's AOT)
**Effort:** Medium (3-5 days)

**The Problem:**
The in-memory JIT cache is lost when jank exits. Every new session must recompile everything.

**The Solution:**
Persist the JIT cache to disk:
```
~/.cache/jank/<version>/jit_cache/
├── vybe.type/
│   ├── -get-world-box.hash     # Expression hash
│   ├── -get-world-box.o        # Compiled object file
│   ├── mangle-char.hash
│   ├── mangle-char.o
│   └── ...
```

**How it works:**
1. On `def`/`defn` evaluation:
   - Compute expression hash (already done)
   - Check disk cache for matching hash + `.o` file
   - If found: load `.o` directly (skip JIT entirely!)
   - If not found: JIT compile, save `.o` to disk

2. Cache invalidation:
   - Hash includes expression structure (not source position)
   - If hash changes, old `.o` is invalid
   - Clean up on jank version change

**Benefits:**
- First load: ~4s (compile + save to cache)
- Subsequent loads: ~0.1-0.5s (just load `.o` files)
- Works across sessions, restarts, reboots

**Implementation:**
```cpp
// In evaluate.cpp for def
object_ref eval(expr::def_ref const expr) {
    auto const hash = hash_expression(expr->value);
    auto const cache_path = get_cache_path(expr->name, hash);

    // Check disk cache
    if(std::filesystem::exists(cache_path)) {
        __rt_ctx->jit_prc.load_object(cache_path);
        return get_cached_var(expr->name);
    }

    // JIT compile
    auto result = jit_compile(expr);

    // Save to disk cache
    save_object_file(cache_path);

    return result;
}
```

---

### Strategy 2: AOT Compilation (HIGH IMPACT)

**Expected Impact:** Near-instant module loading (like Clojure's AOT)
**Effort:** Medium-High (5-7 days)

**The Problem:**
Every time a module is loaded, all forms are JIT compiled even if the source hasn't changed.

**The Solution:**
Pre-compile modules to `.o` files during build:
```bash
# Build step: compile vybe.type to object file
jank compile --aot vybe.type -o target/classes/vybe/type.o
```

**How it works:**
1. Module loader already checks for `.o` files (this exists!)
2. If `.o` is newer than source, load `.o` directly
3. No JIT compilation needed at runtime

**jank already supports this** - see `module/loader.cpp`:
```cpp
// loader.cpp already checks for .o files!
if(module_type::o && is_newer_than_source(o_path)) {
    jit_prc.load_object(o_path);
    return;
}
```

**What's needed:**
1. `jank compile --aot` command to generate `.o` files
2. Integrate into build system (lein/deps.edn plugin)
3. Ship libraries with pre-compiled `.o` files

---

### Strategy 3: PCH Pre-instantiation (MEDIUM IMPACT)

**Expected Impact:** 20-40% faster JIT per form
**Effort:** Low (1-2 days)

**The Problem:**
Templates like `make_box<T>`, `oref<T>`, `expect_object<T>` are instantiated on every JIT call.

**The Solution:**
Pre-instantiate common templates in the PCH:

```cpp
// In include/cpp/jank/precompiled.hpp

// Explicit instantiation declarations
extern template struct jank::runtime::oref<jank::runtime::obj::integer>;
extern template struct jank::runtime::oref<jank::runtime::obj::persistent_string>;
extern template struct jank::runtime::oref<jank::runtime::obj::keyword>;
extern template struct jank::runtime::oref<jank::runtime::obj::symbol>;
extern template struct jank::runtime::oref<jank::runtime::obj::persistent_vector>;
extern template struct jank::runtime::oref<jank::runtime::obj::persistent_hash_map>;
// ... more common types

// Pre-instantiated make_box
extern template jank::runtime::oref<jank::runtime::obj::integer>
    jank::runtime::make_box(i64);
extern template jank::runtime::oref<jank::runtime::obj::real>
    jank::runtime::make_box(f64);
// ... more common boxing operations
```

**Implementation:**
1. Profile which templates are instantiated most often
2. Add explicit instantiation declarations to PCH
3. Add explicit instantiation definitions to a compilation unit
4. Verify template instantiations are reused

---

### Strategy 4: Clang Optimization Flags (LOW-MEDIUM IMPACT)

**Expected Impact:** 10-30% faster JIT compilation
**Effort:** Low (0.5-1 day)

**Current JIT Flags:**
```cmake
-w -fwrapv -fno-stack-protector -fPIC -O2
```

**Proposed Changes:**
```cmake
# Faster compilation at cost of runtime performance
-O0                           # Skip optimization passes (biggest impact)
-fno-exceptions               # If exceptions not needed
-fno-rtti                     # If RTTI not needed
-fno-unwind-tables            # Skip unwind info
-fno-asynchronous-unwind-tables
-fno-stack-protector          # Already present
-fno-ident                    # Skip .ident sections
```

**Trade-offs:**
| Flag | Compile Time Savings | Runtime Cost |
|------|---------------------|--------------|
| `-O0` vs `-O2` | 30-50% | 2-5x slower code |
| `-fno-unwind-tables` | 5-10% | No stack traces |
| `-fno-rtti` | 5-10% | No dynamic_cast |

**Recommended:** Use `-O0` for REPL, `-O2` for AOT compilation.

**Implementation:**
```cpp
// In jit/processor.cpp
if(opts.repl_mode)
{
    flags.push_back("-O0");
}
else
{
    flags.push_back("-O2");
}
```

---

### Strategy 5: Simple Expression Interpreter (MEDIUM IMPACT)

**Expected Impact:** Instant eval for ~30% of expressions
**Effort:** High (1-2 weeks)

**The Problem:**
Simple expressions like literals, var derefs, and simple calls don't need full JIT.

**Fast-Path Candidates:**
```clojure
;; These could be interpreted instantly:
42                    ; primitive literal
:keyword              ; keyword
"string"              ; string
[1 2 3]               ; vector of literals
{:a 1 :b 2}           ; map of literals
foo                   ; var deref
(+ 1 2)               ; simple arithmetic with literal args
(if true 1 2)         ; if with literal condition
```

**Implementation:**
```cpp
object_ref eval(expression_ref expr)
{
    if(can_interpret_fast(expr))
    {
        return interpret(expr);  // Instant!
    }
    return jit_compile_and_eval(expr);  // Full JIT
}

bool can_interpret_fast(expression_ref expr)
{
    return visit_expression(
        [](auto const& e) -> bool {
            using T = std::decay_t<decltype(e)>;
            if constexpr(is_one_of_v<T,
                expr::primitive_literal_ref,
                expr::var_deref_ref,
                expr::vector_ref,  // if all elements are literals
                expr::map_ref>)    // if all k/v are literals
            {
                return true;
            }
            return false;
        },
        expr
    );
}
```

**Benefits:**
- Primitives: 0ms instead of 50ms
- Var derefs: Already fast, but avoids JIT wrapper
- Collection literals: Could save 50ms for common patterns

---

### Strategy 6: JIT Code Structure Optimization (LOW IMPACT)

**Expected Impact:** 5-15% faster JIT
**Effort:** Medium (3-5 days)

**The Problem:**
Generated C++ code is verbose and uses many templates.

**Current Generated Code:**
```cpp
namespace user {
struct __fn_abc123 : jank::runtime::obj::jit_function {
    jank::runtime::object_ref operator()(
        jank::runtime::object_ref const p1) const {
        return jank::runtime::add(p1, jank::runtime::make_box(1LL));
    }
};
}
```

**Optimized Generated Code:**
```cpp
// Use aliases
namespace user {
struct __fn_abc123 : jfn {
    oref operator()(oref const p1) const {
        return add(p1, box(1LL));
    }
};
}
```

**Implementation:**
1. Add short aliases to PCH: `using oref = jank::runtime::object_ref;`
2. Update codegen to use aliases
3. Use fewer templates where possible

---

## Recommended Implementation Order

### Phase 1: Quick Wins (1-2 days)
1. **Clang flags optimization** - Change `-O2` to `-O0` for REPL/interactive mode
2. **PCH pre-instantiation** - Pre-instantiate top 20 templates

### Phase 2: Persistent JIT Cache (3-5 days) - HIGHEST IMPACT
3. **Persist JIT cache to disk** - Load compiled `.o` files across sessions
   - This is the **most impactful** optimization for `vybe.type`
   - First load: ~4s, subsequent loads: ~0.1-0.5s
   - Works like Clojure's AOT but automatic

### Phase 3: Simple Interpreter (1-2 weeks)
4. **Simple expression interpreter** - Fast path for literals/derefs
   - Many small helper functions in `vybe.type` could skip JIT entirely

### Phase 4: AOT Compilation Command (5-7 days)
5. **`jank compile --aot`** - Pre-compile entire modules
   - Ship libraries with `.o` files
   - Instant loading for production deployments

---

## Estimated Impact on vybe.type

Current: ~4s (with in-memory cache, initial load every session)

| Strategy | First Load | Subsequent Loads | Notes |
|----------|------------|------------------|-------|
| Current (in-memory cache) | ~4s | ~4s | Cache lost on exit |
| After Phase 1 (flags + PCH) | ~3s | ~3s | 25% faster per form |
| **After Phase 2 (disk cache)** | **~4s** | **~0.1-0.5s** | **Like Clojure AOT!** |
| After Phase 3 (interpreter) | ~3s | ~0.1s | Skip JIT for simple forms |

**Target:** Sub-1-second namespace loads for development (after first compilation)

### vybe.type Specific Estimates

With persistent disk cache:
```
First session:   45 forms × ~50ms = ~2.25s JIT + save .o files
                 Total: ~4s

Subsequent sessions: 45 forms × ~2ms (load .o) = ~0.09s
                     Total: ~0.1-0.5s (including module init)
```

**This matches Clojure's AOT behavior:**
- First compile: same as now
- After that: instant loads from cached object files

---

## Verification Plan

### Benchmarks to Run
```bash
# Before and after each optimization:
./build/jank-test --test-case="*Eval Breakdown*"
./build/jank-test --test-case="*Incremental JIT*"
./build/jank-test --test-case="*Strategy 0*"
```

### Real-World Test
```clojure
;; Time loading vybe.type
(time (require 'vybe.type :reload))
```

---

## Next Steps

1. **Immediate**: Implement Phase 1 (Clang flags + PCH)
2. **This week**: Design and implement expression batching
3. **Measure**: Run benchmarks after each change
4. **Iterate**: Prioritize based on measured impact

---

## Summary

### Key Insights from Clojure Study

1. **Clojure is fast because bytecode generation is trivial** (~0.1ms per form)
2. **jank's C++ JIT is slow because Clang must parse C++** (~45ms per form)
3. **The solution is the same as Clojure's:** cache compiled code to avoid recompilation

### What Won't Work
- **Batching**: Each top-level form must be evaluated before the next can be analyzed
- **Parallel JIT**: Forms depend on each other
- **LLVM IR codegen**: Need C++ codegen for full C++ interop

### Recommended Approach: Persistent JIT Cache

**This is essentially automatic AOT compilation:**

1. First load: JIT compile as usual, save `.o` files to disk cache
2. Subsequent loads: Load `.o` files directly, skip JIT entirely

**Benefits:**
- First load: ~4s (same as now)
- Subsequent loads: **~0.1-0.5s** (40-80x faster!)
- Works across sessions, restarts, reboots
- No build system integration required (automatic)

### For vybe.type Specifically

The file has:
- 45 top-level forms (36 defn, 2 defmacro, 4 defonce, 3 cpp/raw)
- 324 lines of C++ code in `cpp/raw` blocks

| Scenario | Time |
|----------|------|
| Current (every session) | ~4s |
| With disk cache (first time) | ~4s |
| **With disk cache (after)** | **~0.1-0.5s** |

### Implementation Priority

1. **Phase 1 (1-2 days):** Quick wins (Clang flags, PCH)
2. **Phase 2 (3-5 days):** Persistent JIT cache ← **TOP PRIORITY**
3. **Phase 3 (1-2 weeks):** Simple interpreter for trivial forms
4. **Phase 4 (5-7 days):** `jank compile --aot` command
