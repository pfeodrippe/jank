# Rust JIT Compilation Research for jank-rs

## The Goal
We want: `Clojure code → Rust code → JIT compile → native performance`

Similar to C++ jank which does: `Clojure code → C++ code → clang-repl → native code`

## The Problem
**Rust has no equivalent to clang-repl.** There's no fast JIT compiler that takes Rust source code directly.

## Available Options

### Option 1: evcxr (Rust REPL)
- **How it works:** Invokes full rustc, compiles to dylib, loads dynamically
- **Latency:** 5-10 seconds per evaluation (too slow for interactive REPL)
- **Quality:** Full Rust, all features
- **Recent improvements:**
  - Now supports Cranelift backend: `:toolchain nightly` then `:codegen_backend cranelift`
  - Built-in caching: `:cache 500` (500MB cache)
  - Compiles dependencies as dylibs for reuse
- **Source:** https://github.com/evcxr/evcxr

### Option 2: rustc + dlopen
- **How it works:** Generate Rust source → call rustc → load .so/.dylib
- **Latency:** ~1 second minimum (rustc overhead)
- **Quality:** Full Rust
- **Source:** https://charlycst.github.io/posts/jit-ing-rust/

### Option 3: Cranelift (current jank-rs approach)
- **How it works:** Manually construct Cranelift IR → native code
- **Latency:** Milliseconds
- **Quality:** ~14% slower than LLVM, but very fast compilation
- **Limitation:** Must manually build IR - doesn't understand Rust source code
- **Source:** https://cranelift.dev/

### Option 4: inkwell/LLVM
- **How it works:** Manually construct LLVM IR from Rust
- **Latency:** Milliseconds (faster than rustc, slower than Cranelift)
- **Quality:** Full LLVM optimization
- **Limitation:** Same as Cranelift - manual IR construction
- **Source:** https://github.com/TheDan64/inkwell

### Option 5: libgccjit
- **How it works:** GCC backend, primarily for AOT
- **Latency:** Slow (designed for AOT, not JIT)
- **License:** GPL (viral)
- **Source:** https://github.com/rust-lang/rustc_codegen_gcc

## rustc Incremental Compilation

### Current State (2025)
- Incremental rebuild after small change: **1.1-1.5 seconds** (best case)
- With mold/lld linker: **~1.5 seconds**
- Default (GNU ld): **7-20 seconds**

### Bottlenecks
1. **Linking** - the only non-incremental part
2. **Codegen** - ~400ms even for small changes
3. **Startup overhead** - rustc itself takes time to start

### Improvements
- LLD linker default on Linux starting Rust 1.90.0 (Sept 2025)
- Cranelift backend: ~20-30% faster codegen
- Incremental linker "wild" in development

### Sources
- https://davidlattimore.github.io/posts/2024/02/04/speeding-up-the-rust-edit-build-run-cycle.html
- https://nnethercote.github.io/2025/03/19/how-to-speed-up-the-rust-compiler-in-march-2025.html

## Cranelift as rustc Backend

Can use Cranelift instead of LLVM for rustc codegen:

```bash
rustup component add rustc-codegen-cranelift-preview --toolchain nightly
RUSTFLAGS="-Zcodegen-backend=cranelift" cargo build
```

### Performance
- **Compilation:** ~20-30% faster than LLVM
- **Runtime:** ~14% slower than LLVM
- **Status:** Production-ready goal for 2025

### In evcxr
```
:toolchain nightly
:codegen_backend cranelift
```

## Comparison Table

| Approach | Compile Time | Runtime Perf | Effort | Full Rust? |
|----------|-------------|--------------|--------|------------|
| evcxr (LLVM) | 5-10s | 100% | Low | Yes |
| evcxr (Cranelift) | 3-7s | ~86% | Low | Yes |
| rustc incremental | 1-2s | 100% | Medium | Yes |
| Cranelift IR | <100ms | ~86% | High | No |
| Generate C + clang | <100ms | ~95% | High | No |

## Conclusions

### Why "jank → Rust → fast JIT" is Hard

1. **rustc is slow** - Even incremental builds take 1+ seconds
2. **No Rust JIT exists** - Unlike clang-repl for C++
3. **Cranelift doesn't understand Rust** - Just low-level IR

### Realistic Options for jank-rs

1. **Accept 1-2s latency** - Use rustc with Cranelift backend + incremental
2. **Use Cranelift IR directly** - Fast but manual IR construction (current approach)
3. **Generate C code** - Use clang-repl like C++ jank does
4. **Hybrid** - Cranelift for hot numeric code, rustc for complex code

### Recommendation

The most practical path is probably:

1. **For numeric/hot code:** Use Cranelift IR directly (current approach) - millisecond latency
2. **For complex code:** Accept slower compilation via evcxr/rustc with Cranelift backend

OR

Consider generating C code and using clang-repl (like C++ jank) instead of trying to JIT Rust code.

## evcxr as a Library

evcxr can be embedded as a library, not just used as a REPL:

```rust
use evcxr::EvalContext;

let mut context = EvalContext::new();
context.eval("let s = String::new();")?;
context.eval("s.push_str(\"Hello \");")?;
context.eval("println!(\"{}\", s);")?;
```

### Key Features
- `EvalContext` - main struct for evaluation
- Variables/functions persist across evals
- Can add crate dependencies dynamically
- Supports Cranelift backend for faster compilation

### Source
- https://docs.rs/evcxr
- https://github.com/evcxr/evcxr

## Precompiling Dependencies as dylibs

One approach to speed up incremental builds is to compile dependencies as shared libraries:

### How it works
1. Compile all dependencies once as a single `deps.dylib`
2. Link against this dylib instead of recompiling dependencies
3. Only recompile changed code

### evcxr's Approach
evcxr now compiles dependencies as dylibs. This means:
- Dependencies are compiled once
- Subsequent evals only compile user code
- Mutable static variables in dependencies are preserved

### Limitations
- Rust has no stable ABI - must use same rustc version
- Transitive dependency issues with multiple dylibs
- Platform-specific (.so, .dylib, .dll)

### Sources
- https://robert.kra.hn/posts/2022-09-09-speeding-up-incremental-rust-compilation-with-dylibs/
- https://nicoan.net/posts/accelerating_compile_times/

## Potential Architecture for jank-rs

### Option A: evcxr-based (1-2s latency)
```
Clojure code
    ↓
Generate Rust source
    ↓
evcxr EvalContext (with Cranelift backend)
    ↓
Native code
```

**Pros:** Full Rust, all features, easy to implement
**Cons:** 1-2 second latency per eval

### Option B: Cranelift IR (milliseconds)
```
Clojure code
    ↓
Generate Cranelift IR directly
    ↓
Native code
```

**Pros:** Millisecond compilation
**Cons:** Must manually implement all operations in IR

### Option C: Hybrid
```
Clojure code
    ↓
├─ Simple/hot code → Cranelift IR → Fast native
└─ Complex code → evcxr/rustc → Full Rust
```

**Pros:** Best of both worlds
**Cons:** Complex implementation, two code paths

### Option D: Generate C (like C++ jank)
```
Clojure code
    ↓
Generate C source
    ↓
clang-repl / libclang
    ↓
Native code
```

**Pros:** Proven approach (C++ jank uses this), fast
**Cons:** Not Rust, different ecosystem

## Questions to Resolve

1. Is 1-2 second REPL latency acceptable?
2. Should we generate C instead of Rust for JIT?
3. Can we make Cranelift IR generation cover more cases?
4. Is there value in a hybrid approach?
5. Should we use evcxr as a library for complex code?

## Next Steps

1. **Benchmark evcxr** with Cranelift backend to get actual latency numbers
2. **Prototype evcxr integration** - generate Rust code, eval with EvalContext
3. **Evaluate hybrid approach** - Cranelift for numeric, evcxr for complex
4. **Consider C generation** if Rust JIT is too slow
