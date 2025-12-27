# Compile Server Performance Optimization Plan

## Date: 2025-12-26

## Problem

The iOS compile server is slow. Every eval request:
1. Writes C++ to temp file (disk I/O)
2. Spawns a new clang process (process overhead ~50-100ms)
3. Clang loads the 52MB PCH file every time (significant overhead)
4. Compiles the code
5. Writes object to disk
6. Reads object file back

For interactive REPL use, this latency is unacceptable.

## Root Cause Analysis

The current `cross_compile()` function uses `popen()` to spawn clang:
```cpp
FILE *pipe = popen(cmd.c_str(), "r");  // Spawns new process every time!
```

This means EVERY compile request:
- Forks the process (~1ms)
- Loads clang binary (~10-50ms)
- Parses 52MB PCH file (~200-500ms)
- Compiles code (~50-200ms)
- Total: ~300-800ms per eval

## Solution: Persistent Clang CompilerInstance

Instead of spawning clang CLI, use Clang's C++ API directly with a **persistent CompilerInstance**.

### How Clang Works (from [MaskRay's deep dive](https://maskray.me/blog/2023-09-24-a-deep-dive-into-clang-source-file-compilation)):

1. **CompilerInstance** manages all compilation state:
   - CompilerInvocation (command-line options)
   - DiagnosticsEngine
   - TargetInfo (iOS ARM64 in our case)
   - FileManager, SourceManager
   - Preprocessor (with PCH loaded)
   - ASTContext, Sema

2. **EmitObjAction** generates object files:
   - BackendConsumer generates LLVM IR
   - EmitBackendOutput produces the .o file

3. **In-memory compilation** (from [memzero blog](https://blog.memzero.de/libclang-c-to-llvm-ir/)):
   - Use PreprocessorOptions::addRemappedFile() to provide code from memory
   - Use raw_svector_ostream to capture output in memory

### Implementation Plan

```cpp
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/CodeGen/CodeGenAction.h>
#include <llvm/Support/MemoryBuffer.h>

class persistent_ios_compiler {
  std::unique_ptr<clang::CompilerInstance> ci_;
  bool initialized_{false};

public:
  bool init(ios_compile_config const& config) {
    ci_ = std::make_unique<clang::CompilerInstance>();

    // Configure for iOS cross-compilation
    auto& invocation = ci_->getInvocation();

    // Target options
    auto& target_opts = invocation.getTargetOpts();
    target_opts.Triple = config.target_triple;  // "arm64-apple-ios17.0-simulator"

    // Header search (sysroot, includes)
    auto& header_opts = invocation.getHeaderSearchOpts();
    header_opts.Sysroot = config.ios_sdk_path;
    for(auto const& inc : config.include_paths) {
      header_opts.AddPath(inc, clang::frontend::Angled, false, false);
    }

    // Preprocessor options - load PCH
    auto& pp_opts = invocation.getPreprocessorOpts();
    pp_opts.ImplicitPCHInclude = config.pch_path;

    // Language options
    auto& lang_opts = *invocation.getLangOpts();
    lang_opts.CPlusPlus = true;
    lang_opts.CPlusPlus20 = true;

    // Code generation options
    auto& codegen_opts = invocation.getCodeGenOpts();
    codegen_opts.RelocationModel = llvm::Reloc::PIC_;

    // Initialize the compiler instance
    ci_->createDiagnostics();
    ci_->createFileManager();
    ci_->createSourceManager(ci_->getFileManager());

    // Create target - this is where iOS ARM64 is configured
    ci_->setTarget(clang::TargetInfo::CreateTargetInfo(
      ci_->getDiagnostics(),
      std::make_shared<clang::TargetOptions>(target_opts)));

    // Load the PCH - this is the expensive part, done ONCE
    ci_->createPreprocessor(clang::TU_Complete);
    // PCH is loaded automatically via ImplicitPCHInclude

    initialized_ = true;
    return true;
  }

  std::vector<uint8_t> compile(std::string const& cpp_code, std::string const& name) {
    if(!initialized_) return {};

    // Create in-memory buffer for input
    auto buffer = llvm::MemoryBuffer::getMemBufferCopy(cpp_code, name + ".cpp");

    // Remap the file to our in-memory buffer
    ci_->getPreprocessorOpts().addRemappedFile(name + ".cpp", buffer.release());

    // Create output buffer
    llvm::SmallVector<char, 0> output_buffer;
    auto output_stream = std::make_unique<llvm::raw_svector_ostream>(output_buffer);

    // Set up the backend output
    ci_->setOutputStream(std::move(output_stream));

    // Create and execute EmitObjAction
    clang::EmitObjAction action;
    if(!ci_->ExecuteAction(action)) {
      return {};  // Compilation failed
    }

    // Return the object data
    return std::vector<uint8_t>(output_buffer.begin(), output_buffer.end());
  }
};
```

### Key Benefits

1. **PCH loaded once** - The 52MB PCH is parsed at startup, not per-request
2. **No process spawn** - Direct function calls, no fork/exec
3. **All in-memory** - No disk I/O for temp files
4. **Compiler state reused** - FileManager, SourceManager caches preserved

### Expected Performance

| Metric | Before (popen) | After (persistent) |
|--------|----------------|-------------------|
| PCH load | 200-500ms/request | 500ms once |
| Process spawn | 50-100ms/request | 0ms |
| Disk I/O | 10-50ms/request | 0ms |
| **Total per eval** | **300-800ms** | **50-200ms** |

That's a **4-10x speedup**!

### Implementation Steps

1. **Create `persistent_compiler.hpp`**
   - Define the persistent_ios_compiler class
   - Handle initialization with iOS config
   - Implement in-memory compilation

2. **Integrate into server.hpp**
   - Initialize persistent compiler at server startup
   - Replace `cross_compile()` popen with persistent compiler
   - Handle compiler state reset if needed between compiles

3. **Handle edge cases**
   - Compiler error recovery
   - Memory management for buffers
   - Thread safety (if needed)

### Files to Create/Modify

1. **NEW**: `include/cpp/jank/compile_server/persistent_compiler.hpp`
2. **MODIFY**: `include/cpp/jank/compile_server/server.hpp`
   - Add persistent_compiler member
   - Initialize in constructor
   - Replace cross_compile() internals

### References

- [Clang CompilerInstance docs](https://clang.llvm.org/doxygen/classclang_1_1CompilerInstance.html)
- [Cross-compilation using Clang](https://clang.llvm.org/docs/CrossCompilation.html)
- [In-memory compilation example](https://blog.memzero.de/libclang-c-to-llvm-ir/)
- [MaskRay's Clang deep dive](https://maskray.me/blog/2023-09-24-a-deep-dive-into-clang-source-file-compilation)

### Alternative: Use Existing CppInterOp

jank already uses CppInterOp for JIT. However:
- CppInterOp is designed for JIT execution on HOST architecture
- It uses LLVM ORC JIT which executes code, not just emits objects
- Cross-compilation to iOS requires different target configuration

We could potentially:
1. Create a second CppInterOp interpreter with iOS target flags
2. Use Clang's Interpreter API to get compiled modules
3. Extract object code from the ORC JIT layer

But this is more complex than using CompilerInstance directly, which is designed for exactly this use case (compile to object file).

## Next Steps

1. Implement persistent_compiler.hpp
2. Test with simple C++ code
3. Integrate into server
4. Benchmark performance improvement
