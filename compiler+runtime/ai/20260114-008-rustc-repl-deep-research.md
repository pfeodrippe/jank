# Building a Fast Rust JIT (rustc-repl) - Deep Research

## Goal
Build something like clang-repl but for Rust: fast, incremental JIT compilation of Rust source code with millisecond latency.

## Why clang-repl is Fast

### Architecture
```
C++ source → Clang AST → LLVM IR → LLVM ORC JIT → Machine code → Execute
                ↓
        (All in-memory, incremental)
```

### Key Innovations
1. **Incremental Clang facilities** - Clang can parse and compile code incrementally
2. **LLVM ORC JIT** - Just-in-time compilation infrastructure with:
   - Lazy compilation (CompileOnDemandLayer)
   - Symbol resolution like dynamic linker
   - In-memory execution (no disk I/O)
   - dlupdate() for running only new initializers
3. **No link step** - JIT handles symbol resolution directly
4. **Compiler as a Service** - Clang exposes incremental API via clangInterpreter library

### Sources
- https://clang.llvm.org/docs/ClangRepl.html
- https://llvm.org/docs/ORCv2.html

## Why Rust Doesn't Have This

### The Problem
rustc is not designed for incremental JIT:

1. **No incremental parsing** - rustc parses entire files
2. **Complex type system** - Borrow checking, lifetimes require full analysis
3. **No stable compiler API** - rustc internals change constantly
4. **LLVM codegen is slow** - Even with incremental, ~1s minimum
5. **No JIT integration** - rustc outputs object files, not JIT-ready IR

### Current State
- **evcxr**: Spawns full rustc process each time (~5-10s)
- **miri**: Interprets MIR (slow, not JIT)
- **Cranelift**: Fast JIT but doesn't understand Rust source

## Existing Approaches Analysis

### 1. evcxr (Current Best)
**How it works:**
```
Rust code → Generate full crate → rustc → dylib → dlopen → execute
```

**Why it's slow:**
- Spawns new rustc process each eval
- Full type checking every time
- Writes to disk (object files, dylib)
- Full linking even for small changes

**Recent improvements:**
- Cranelift backend support (`:codegen_backend cranelift`)
- Dependencies compiled as dylibs (cached)
- Built-in caching (`:cache 500`)

**Latency:** 1-10 seconds

### 2. rusti (Historical, Dead)
**How it worked:**
- Used rustc internals to get LLVM IR
- Used LLVM JIT to execute

**Why it died:**
- rustc removed the internal APIs it depended on
- Unstable compiler internals
- Maintenance nightmare with rapidly evolving language

### 3. miri (MIR Interpreter)
**How it works:**
- Interprets MIR directly without codegen
- Used for CTFE (const eval) and UB detection

**Why not suitable for REPL:**
- Very slow (interpretation, not JIT)
- Not designed for interactive use
- Limited to safe analysis, not general execution

## Architecture Options for rustc-repl

### Option A: Improve evcxr (Evolutionary)

**Strategy:** Make evcxr faster by:
1. Keep rustc running as a long-lived process (daemon)
2. Use incremental compilation more aggressively
3. Minimize disk I/O
4. Faster linker (mold)

**Estimated improvement:** 5-10s → 0.5-2s

**Pros:** Build on existing work, easier
**Cons:** Still slow, fundamental limits of rustc process model

### Option B: Use rustc_interface Directly (Library Approach)

**Strategy:** Embed rustc as a library:
```rust
#![feature(rustc_private)]
extern crate rustc_interface;
extern crate rustc_driver;

// Use rustc_interface::run_compiler with custom Config
// Keep compiler state across evals
// Reuse type checking results
```

**Key components:**
- `rustc_interface::Config` - Compiler configuration
- `rustc_interface::Compiler` - Compiler session
- `rustc_interface::Queries` - Access to compilation artifacts

**Challenge:**
- API is unstable (requires nightly)
- Need to figure out how to do incremental compilation
- Still need codegen step

**Estimated improvement:** Unknown, potentially 0.3-1s

### Option C: MIR → JIT (Skip LLVM Codegen)

**Strategy:**
```
Rust source → rustc (parse, type check) → MIR → Cranelift/LLVM JIT → Execute
```

**Key insight:** rustc already produces MIR. We could:
1. Use rustc for parsing and type checking (keeps safety)
2. Skip LLVM codegen entirely
3. JIT compile MIR directly with Cranelift

**Implementation:**
1. Hook into rustc after MIR generation
2. Translate MIR to Cranelift IR
3. JIT compile and execute

**Existing work:**
- `rustc_codegen_cranelift` already does MIR → Cranelift
- Could potentially be modified for JIT use

**Estimated improvement:** 0.1-0.5s (type checking still slow)

### Option D: Salsa-based Incremental (rust-analyzer approach)

**Strategy:** Use Salsa framework for fine-grained incrementality:
```
Source change → Salsa query graph → Only recompute affected → JIT
```

**Key insight:** rust-analyzer already does fast incremental analysis. Could we:
1. Use rust-analyzer's incremental infrastructure
2. Only re-typecheck changed functions
3. Only re-codegen changed code
4. JIT with function-level granularity

**Challenge:**
- rust-analyzer doesn't do codegen
- Would need to add codegen layer
- Complex integration

**Estimated improvement:** Potentially <100ms for small changes

### Option E: Hybrid Interpretation + JIT

**Strategy:**
```
First run: Interpret MIR (fast startup)
Hot code: JIT compile to native (fast execution)
```

**Like:**
- JavaScript V8 engine (interpreter + TurboFan JIT)
- Java HotSpot (interpreter + C1 + C2 JIT)

**Implementation:**
1. Start with miri-like MIR interpretation
2. Profile to find hot functions
3. JIT compile hot functions with Cranelift
4. Replace interpreted with native

**Pros:** Fast first execution, optimizes over time
**Cons:** Complex, interpretation is slow for compute

## Recommended Approach: Option C + D Hybrid

### Architecture
```
                    ┌─────────────────────────────────────┐
                    │         rustc-repl daemon          │
                    │                                     │
┌─────────┐         │  ┌─────────┐    ┌─────────────┐   │
│  Clojure │ ─────► │  │ Parser  │───►│ Type Check  │   │
│  source  │        │  │ (incr)  │    │   (incr)    │   │
└─────────┘         │  └─────────┘    └──────┬──────┘   │
                    │                         │         │
                    │                         ▼         │
                    │                   ┌─────────┐     │
                    │                   │   MIR   │     │
                    │                   └────┬────┘     │
                    │                        │          │
                    │            ┌───────────┴──────────┤
                    │            │                      │
                    │            ▼                      │
                    │     ┌────────────┐                │
                    │     │ Cranelift  │                │
                    │     │  JIT (ORC) │                │
                    │     └─────┬──────┘                │
                    │           │                       │
                    │           ▼                       │
                    │     ┌────────────┐                │
                    │     │  Execute   │                │
                    │     └────────────┘                │
                    └─────────────────────────────────────┘
```

### Key Components

1. **Long-lived rustc daemon**
   - Keep rustc loaded in memory
   - Maintain compilation state
   - No process startup overhead

2. **Incremental parsing with Salsa**
   - Only re-parse changed code
   - Cache AST nodes
   - Early cutoff when possible

3. **Incremental type checking**
   - Function-level granularity
   - Reuse type information
   - Only re-check dependencies

4. **MIR → Cranelift JIT**
   - Translate MIR directly to Cranelift IR
   - Use Cranelift's fast compilation
   - No LLVM, no disk I/O

5. **ORC-style symbol resolution**
   - Lazy compilation
   - Symbol lookup like dynamic linker
   - Support for redefinition

## Implementation Steps

### Phase 1: rustc Daemon (2-4 weeks)
1. Create long-lived rustc process using `rustc_interface`
2. Accept code snippets over IPC
3. Compile to dylib and execute
4. **Goal:** Eliminate process startup overhead
5. **Expected latency:** 0.5-2s

### Phase 2: Incremental Compilation (4-8 weeks)
1. Integrate Salsa for query caching
2. Implement function-level incremental type checking
3. Cache MIR between evaluations
4. **Goal:** Only re-typecheck changed code
5. **Expected latency:** 0.2-0.5s

### Phase 3: MIR JIT (4-8 weeks)
1. Extract MIR from rustc
2. Translate MIR to Cranelift IR
3. JIT compile with Cranelift
4. Execute in-process (no dlopen)
5. **Goal:** Eliminate disk I/O and linking
6. **Expected latency:** 50-200ms

### Phase 4: ORC-style Execution (2-4 weeks)
1. Implement lazy compilation
2. Symbol table management
3. Support for function redefinition
4. **Goal:** Near-instant for small changes
5. **Expected latency:** 10-50ms

### Phase 5: Integration with jank-rs (2-4 weeks)
1. Create Rust code generator from Clojure AST
2. Integrate rustc-repl as JIT backend
3. Remove interpreter fallback
4. **Goal:** Full JIT for all Clojure code

## Technical Challenges

### Challenge 1: rustc API Stability
**Problem:** rustc internals change every nightly
**Solution:**
- Pin to specific nightly version
- Use rustc_plugin framework
- Accept maintenance burden

### Challenge 2: Type System Complexity
**Problem:** Borrow checking requires whole-function analysis
**Solution:**
- Function-level granularity (not expression-level)
- Cache borrow check results
- Invalidate only affected functions

### Challenge 3: MIR to Cranelift Translation
**Problem:** MIR has Rust-specific constructs
**Solution:**
- Use `rustc_codegen_cranelift` as reference
- May need to handle some operations specially
- Could start with subset of Rust

### Challenge 4: Runtime Library
**Problem:** Rust std library needs to be available
**Solution:**
- Precompile std as dylib
- Link against it at runtime
- Handle std changes with version pinning

### Challenge 5: Generics and Monomorphization
**Problem:** Rust monomorphizes generics, expensive
**Solution:**
- Cache monomorphized instances
- Lazy monomorphization
- Share generic instantiations

## Alternative: Fork/Modify rustc

A more aggressive approach would be to fork rustc and modify it for REPL use:

1. Add incremental parsing mode
2. Add JIT codegen backend
3. Add REPL-specific optimizations
4. Maintain as separate project

**Pros:** Full control, can optimize aggressively
**Cons:** Huge maintenance burden, hard to keep up with upstream

## Comparison with clang-repl

| Aspect | clang-repl | rustc-repl (proposed) |
|--------|------------|----------------------|
| Parser | Incremental | Function-level incremental |
| Type checking | Fast (C++ simpler) | Slower (borrow checking) |
| IR generation | LLVM IR | MIR → Cranelift |
| JIT | ORC | Cranelift |
| Expected latency | <10ms | 10-200ms |

## Conclusion

Building a fast Rust JIT is possible but challenging. The recommended approach:

1. Start with Option B (rustc as library) to eliminate process overhead
2. Add Salsa-based incrementality (Option D) for fine-grained caching
3. Add MIR → Cranelift JIT (Option C) to eliminate disk I/O
4. Iterate and optimize

**Realistic target latency:** 50-200ms for small changes

This won't match clang-repl's <10ms but would be a massive improvement over evcxr's 5-10 seconds.

## Resources

### Documentation
- https://rustc-dev-guide.rust-lang.org/ - Rustc internals
- https://salsa-rs.github.io/salsa/ - Salsa framework
- https://rust-analyzer.github.io/book/contributing/architecture.html - rust-analyzer arch
- https://cranelift.dev/ - Cranelift docs

### Code to Study
- https://github.com/evcxr/evcxr - Current best Rust REPL
- https://github.com/rust-lang/rustc_codegen_cranelift - MIR to Cranelift
- https://github.com/rust-lang/miri - MIR interpreter
- https://github.com/rust-lang/rust-analyzer - Incremental analysis
- https://github.com/cognitive-engineering-lab/rustc_plugin - rustc as library

### Papers/Posts
- https://medium.com/@eliah.lakhin/salsa-algorithm-explained - Salsa algorithm
- https://rust-analyzer.github.io/blog/2023/07/24/durable-incrementality.html - Incrementality
- https://llvm.org/docs/ORCv2.html - ORC JIT architecture
