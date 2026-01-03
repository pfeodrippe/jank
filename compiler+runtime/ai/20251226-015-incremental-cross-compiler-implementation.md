# Incremental Cross-Compiler Implementation

## Date: 2025-12-26

## Summary

Implemented a new `incremental_compiler` class using `clang::IncrementalCompilerBuilder` and `clang::Interpreter` for TRUE incremental iOS cross-compilation. Headers are parsed ONCE at startup, and subsequent compilations only parse the new code.

## Key Insight

The existing `persistent_compiler` was still slow (~1.5s for `(+ 1 2)`) because it was spawning a new Clang compilation for each request, which re-parsed all headers each time.

The new `incremental_compiler` uses:
1. `clang::IncrementalCompilerBuilder` to create a cross-compilation-ready `CompilerInstance`
2. `clang::Interpreter` for incremental parsing (headers stay in AST)
3. LLVM's `TargetMachine::addPassesToEmitFile()` to emit ARM64 object code

## Implementation Details

### New Class: `incremental_compiler`

Location: `include/cpp/jank/compile_server/persistent_compiler.hpp`

```cpp
class incremental_compiler {
public:
  bool init(clang_path, target_triple, sysroot, pch_path, include_paths, extra_flags);
  bool parse_runtime_headers(prelude_code);  // Called once at startup
  compile_result compile(cpp_code, module_name);  // Fast for subsequent calls

private:
  std::unique_ptr<clang::Interpreter> interpreter_;
  std::unique_ptr<llvm::TargetMachine> target_machine_;
};
```

### Key Flow

1. **Initialization**:
   ```cpp
   clang::IncrementalCompilerBuilder builder;
   builder.SetTargetTriple("arm64-apple-ios17.0-simulator");
   builder.SetCompilerArgs(args);  // -isysroot, -I paths, etc.
   auto ci = builder.CreateCpp();
   interpreter_ = clang::Interpreter::create(std::move(ci));
   target_machine_ = target->createTargetMachine(...);
   ```

2. **Header Parsing (once at startup)**:
   ```cpp
   interpreter_->Parse("#include <jank/prelude.hpp>");
   // Headers now in AST, subsequent parses skip them
   ```

3. **Fast Compilation**:
   ```cpp
   auto ptu = interpreter_->Parse(cpp_code);
   // ptu.TheModule contains LLVM IR

   ptu.TheModule->setTargetTriple(target_triple_);
   ptu.TheModule->setDataLayout(target_machine_->createDataLayout());

   llvm::legacy::PassManager pass;
   target_machine_->addPassesToEmitFile(pass, output_stream, nullptr,
                                        llvm::CodeGenFileType::ObjectFile);
   pass.run(*ptu.TheModule);
   ```

### Server Integration

In `server.hpp`:
1. Try `incremental_compiler` first (fastest)
2. Fall back to `persistent_compiler` if incremental fails
3. Fall back to `popen` clang CLI if both fail

## Expected Performance

| Metric | Before (persistent) | After (incremental) |
|--------|---------------------|---------------------|
| Header parsing | ~1.5s per request | ~1.5s ONCE at startup |
| Code parsing | ~10ms | ~10ms |
| Object emission | ~100ms | ~100ms |
| **Total per request** | **~1.6s** | **~100-200ms** |

## Files Modified

1. `include/cpp/jank/compile_server/persistent_compiler.hpp`
   - Added `incremental_compiler` class
   - Added includes for `clang::Interpreter`, LLVM target machine, etc.

2. `include/cpp/jank/compile_server/server.hpp`
   - Added `incremental_compiler` member
   - Updated initialization to try incremental compiler first
   - Updated `cross_compile()` to use incremental compiler if available

## Build Blocked

**Issue**: The jank PCH (precompiled header) build is crashing with clang 22 on boost headers.

**Error**: Segmentation fault in `clang::Sema::ActOnStartNamespaceDef` while parsing `boost/lexical_cast/detail/converter_lexical.hpp`

**This is a pre-existing issue** - not related to these code changes. The incremental_compiler implementation is complete but cannot be tested until the PCH build issue is resolved.

## Next Steps

1. Fix the PCH build crash (clang 22 + boost headers issue)
2. Build compile-server
3. Test with: `time clj-nrepl-eval -p 5558 "(+ 1 2)"`
4. Expected: ~100-200ms instead of ~1600ms

## Research Notes

Key discoveries from Clang internals:

1. `IncrementalCompilerBuilder` is designed for exactly this use case - pre-configured for incremental compilation
2. `Interpreter::Parse()` returns `PartialTranslationUnit` which contains `std::unique_ptr<llvm::Module> TheModule`
3. The JIT executor is only created on `Execute()`, not during `Parse()` - so cross-compilation works
4. CppInterOp uses the same pattern for AOT compilation (see `lib/Interpreter/CppInterOp.cpp:2284`)
