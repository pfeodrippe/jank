# Dynamic Runtime Library Loading Research (clang-repl / JIT)

**Date**: 2025-12-30
**Status**: Research Complete
**Goal**: Load libraries like Flecs, ImGui, Jolt at runtime WITHOUT precompiled shared objects (.so/.dylib)

## Executive Summary

**YES, this is possible!** jank already has most of the infrastructure needed. The key approaches are:

1. **Source Code Compilation via `eval_string()`** - Include/paste C/C++ source directly
2. **In-Memory Object File Loading** - jank already supports `load_object(data, size, name)`
3. **Source → LLVM IR → Memory** - Compile to IR in memory, load via `load_ir_module()`

The most promising approach is **compiling C/C++ source to object files in memory** and loading via the existing JIT infrastructure.

---

## Current jank JIT Infrastructure

### Existing Loading Mechanisms

jank's `jit::processor` already supports multiple loading methods:

```cpp
// 1. Dynamic libraries (.so/.dylib) - via dlopen/LoadDynamicLibrary
load_dynamic_library(path)  // processor.cpp:600-624

// 2. Object files from disk (.o)
load_object(path)           // processor.cpp:403-428

// 3. Object files from MEMORY (key for our use case!)
load_object(data, size, name)  // processor.cpp:430-461

// 4. LLVM IR modules
load_ir_module(ThreadSafeModule)  // processor.cpp:463-475

// 5. LLVM bitcode
load_bitcode(module, bitcode)     // processor.cpp:477-495

// 6. C++ source code (compile & execute)
eval_string(code)                 // processor.cpp:317-338
```

### The Critical Method: In-Memory Object Loading

```cpp
bool processor::load_object(char const *data, size_t size, std::string const &name) const
{
  /* Skip if already loaded - uses the name as key for idempotency. */
  if(loaded_objects_.contains(name))
    return true;

  auto const ee{ interpreter->getExecutionEngine() };
  auto buffer{ llvm::MemoryBuffer::getMemBuffer(
    llvm::StringRef{ data, size },
    name,
    /* RequiresNullTerminator */ false) };

  auto err = ee->addObjectFile(std::move(buffer));
  if(err) { /* handle error */ }

  loaded_objects_.insert(name);
  register_jit_stack_frames();
  return true;
}
```

**This is exactly what we need!** We just need to generate the object file bytes in memory.

---

## Approach 1: Header-Only / Single-File Libraries

### Concept

For libraries distributed as single-file or header-only (like Flecs), directly include the source:

```cpp
// Via jank C++ interop
(cpp/raw "
#include \"flecs.h\"
#define FLECS_IMPLEMENTATION  // Turns header into implementation
#include \"flecs.h\"
")

// Or paste the entire source
(cpp/raw "<entire flecs.c contents here>")
```

### Pros
- Simplest approach
- No additional tooling needed
- Works with existing `eval_string()` infrastructure

### Cons
- Very large source files slow down compilation
- Header parsing overhead on each startup
- PCH (precompiled headers) can help but limited

### Libraries This Works For
- **Flecs** - Single-file ECS (flecs.c + flecs.h)
- **stb_* libraries** - Single-header libraries (stb_image.h, etc.)
- **Dear ImGui** - Can be compiled as single translation unit
- **miniaudio** - Single-file audio library

---

## Approach 2: Source → Object File → Memory (RECOMMENDED)

### Concept

Use clang's `CompilerInstance` to compile C/C++ source directly to an object file in memory:

```
C/C++ Source Code
       ↓
clang::CompilerInstance (in-memory compilation)
       ↓
llvm::MemoryBuffer (object file bytes)
       ↓
jit::processor::load_object(data, size, name)
       ↓
ORC JIT execution
```

### Implementation Sketch

```cpp
#include <clang/Frontend/CompilerInstance.h>
#include <clang/CodeGen/CodeGenAction.h>
#include <llvm/Support/MemoryBuffer.h>

jtl::result<std::vector<uint8_t>, std::string>
compile_source_to_object(std::string_view source, std::string_view module_name)
{
  // 1. Create compiler instance
  auto compiler = std::make_unique<clang::CompilerInstance>();

  // 2. Configure for in-memory compilation
  auto &opts = compiler->getPreprocessorOpts();
  opts.addRemappedFile(
    "virtual_source.cpp",
    llvm::MemoryBuffer::getMemBuffer(source).release()
  );

  // 3. Set up code generation action
  auto action = std::make_unique<clang::EmitObjAction>();

  // 4. Configure output to memory buffer instead of file
  compiler->getFrontendOpts().OutputFile = ""; // No disk output
  compiler->setOutputStream(std::make_unique<llvm::raw_svector_ostream>(output_buffer));

  // 5. Execute compilation
  if (!compiler->ExecuteAction(*action))
    return err("compilation failed");

  // 6. Return object bytes
  return std::vector<uint8_t>(output_buffer.begin(), output_buffer.end());
}

// Usage:
auto obj_bytes = compile_source_to_object(flecs_source, "flecs").expect_ok();
jit_prc.load_object(
  reinterpret_cast<char const*>(obj_bytes.data()),
  obj_bytes.size(),
  "flecs"
);
```

### Pros
- Fast object loading (pre-compiled)
- Works with any C/C++ library
- In-memory, no disk I/O
- Symbols available immediately after loading

### Cons
- Need to implement the compilation pipeline
- Larger memory footprint during compilation
- Need to handle include paths

### Reference Implementation

See [memzero blog: C to LLVM IR in memory](https://blog.memzero.de/libclang-c-to-llvm-ir/) for detailed example.

---

## Approach 3: Source → LLVM IR → Memory

### Concept

Similar to Approach 2, but emit LLVM IR instead of object code:

```
C/C++ Source Code
       ↓
clang::CompilerInstance + EmitLLVMOnlyAction
       ↓
llvm::Module (LLVM IR in memory)
       ↓
jit::processor::load_ir_module(ThreadSafeModule)
       ↓
ORC JIT optimization & execution
```

### Implementation

```cpp
jtl::result<llvm::orc::ThreadSafeModule, std::string>
compile_source_to_ir(std::string_view source)
{
  // Similar to Approach 2, but use EmitLLVMOnlyAction
  auto action = std::make_unique<clang::EmitLLVMOnlyAction>();

  if (!compiler->ExecuteAction(*action))
    return err("compilation failed");

  auto module = action->takeModule();
  return llvm::orc::ThreadSafeModule(std::move(module), context);
}

// Usage:
auto ir_module = compile_source_to_ir(flecs_source).expect_ok();
jit_prc.load_ir_module(std::move(ir_module));
```

### Pros
- ORC JIT can apply optimizations
- More flexibility for analysis/transformation
- Potentially better debugging support

### Cons
- LLVM IR is larger than object code
- Additional JIT overhead for codegen

---

## Approach 4: Lazy/On-Demand Symbol Resolution

### Concept

Use ORC JIT's lazy compilation feature - only compile symbols when first accessed:

```cpp
// Register source as "compilable material"
jit->registerLazyCompileUnit("flecs", flecs_source);

// Later, when flecs_world_create() is called:
// 1. ORC detects unresolved symbol
// 2. Triggers compilation of flecs source
// 3. Symbol is resolved, call proceeds
```

### Pros
- Fast startup (no upfront compilation)
- Only compile what's actually used
- Memory efficient

### Cons
- First-use latency spike
- More complex implementation
- May not work for libraries with static initializers

---

## Practical Implementation Plan

### Phase 1: Proof of Concept with Flecs

```jank
;; New jank API
(require '[jank.native :as n])

;; Load Flecs from source at runtime
(n/load-source-library
  :name "flecs"
  :source (slurp "/path/to/flecs.c")
  :headers ["/path/to/flecs.h"]
  :compile-flags ["-O2" "-DFLECS_NO_THREAD" "-DFLECS_NO_LOG"])

;; Now use Flecs!
(def world (cpp/raw "ecs_init()"))
```

### Phase 2: Library Registry

```cpp
// C++ implementation
struct source_library {
  std::string name;
  std::string source;
  std::vector<std::string> include_paths;
  std::vector<std::string> compile_flags;
  bool is_loaded{false};

  void load(jit::processor& jit);
};

class library_registry {
public:
  void register_library(source_library lib);
  void load_library(std::string_view name);
  bool is_loaded(std::string_view name) const;

private:
  std::map<std::string, source_library> libraries_;
};
```

### Phase 3: Integration with ~/dev/something Pattern

Based on the `~/dev/something` Makefile pattern:

```makefile
# Current approach: Precompiled objects
flecs.o: vendor/flecs/distr/flecs.c
	$(CC) $(CFLAGS) -c $< -o $@

# New approach: Runtime compilation from source
# No build step needed! Source loaded at runtime.
```

---

## ORC JIT Key APIs for This Feature

### DynamicLibrarySearchGenerator

For resolving symbols from system libraries:

```cpp
// Add generator to search system libraries
auto DLSG = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
    DL.getGlobalPrefix());
MainJD.addGenerator(std::move(DLSG));
```

### StaticLibraryDefinitionGenerator

For loading static libraries (.a files):

```cpp
// Load static library symbols
auto SLG = llvm::orc::StaticLibraryDefinitionGenerator::Load(
    ObjLayer, "/path/to/libflecs.a");
MainJD.addGenerator(std::move(SLG));
```

### addObjectFile (Already Supported!)

```cpp
// Load object bytes directly
auto buffer = llvm::MemoryBuffer::getMemBuffer(obj_data, name);
ee->addObjectFile(std::move(buffer));
```

---

## GSoC 2025: Auto-Loading Dynamic Libraries

There's active LLVM development on this exact feature! From [compiler-research.org](https://compiler-research.org/blogs/gsoc25_sahil_introduction_blog/):

> The primary objective is to enable automatic loading of dynamic libraries for unresolved symbols in Clang-Repl. Since Clang-Repl heavily relies on LLVM's ORC JIT for incremental compilation and execution, the work focuses on extending ORC JIT to support this capability.

This would enable:
```cpp
// Future API (in development)
interp->Parse("#include <imgui.h>");  // Auto-loads libimgui.so if needed
interp->ParseAndExecute("ImGui::CreateContext();");
```

---

## Comparison: Current vs Proposed

| Aspect | Current (Precompiled .o/.dylib) | Proposed (Runtime Source) |
|--------|--------------------------------|---------------------------|
| Build Time | Slow (minutes) | Zero |
| App Startup | Fast (load .o) | Slower (compile source) |
| Iteration | Rebuild + restart | Edit source + reload |
| Disk Usage | .o files stored | Source only |
| Flexibility | Fixed at build | Change at runtime |
| Hot Reload | Limited | Full support |
| iOS Support | Works (AOT) | JIT only (dev mode) |

---

## Recommended Next Steps

1. **Implement `compile_source_to_object()`** using clang::CompilerInstance
2. **Add jank API** for loading libraries from source
3. **Test with Flecs** as first target (single-file, well-tested)
4. **Benchmark** compilation time vs precompiled loading
5. **Add caching** - hash source, cache compiled objects

---

## References

- [ORC Design and Implementation](https://llvm.org/docs/ORCv2.html)
- [JITLink and ORC's ObjectLinkingLayer](https://llvm.org/docs/JITLink.html)
- [C to LLVM IR in Memory (memzero)](https://blog.memzero.de/libclang-c-to-llvm-ir/)
- [JIT C in Memory using LLVM ORC API (memzero)](https://blog.memzero.de/llvm-orc-jit/)
- [GSoC 2024: Out-Of-Process Execution For Clang-Repl](https://blog.llvm.org/posts/2024-10-23-out-of-process-execution-for-clang-repl/)
- [GSoC 2025: Advanced Symbol Resolution for Clang-Repl](https://compiler-research.org/blogs/gsoc25_sahil_introduction_blog/)
- [Clang-Repl Documentation](https://clang.llvm.org/docs/ClangRepl.html)
- [CppInterOp](https://github.com/compiler-research/CppInterOp)
- [LLVM MemoryBuffer API](https://llvm.org/doxygen/classllvm_1_1MemoryBuffer.html)

---

## ~/dev/something Analysis

The `~/dev/something` project demonstrates the current workflow:

1. **Precompiled Objects**: Libraries compiled to .o files
2. **Jank-Clang ABI**: `vybe_flecs_jank.cpp` compiled with jank's clang for ABI compatibility
3. **Shared Library**: `libsdf_deps.dylib` bundles all dependencies
4. **Header FFI**: Direct C++ header includes in jank code via `(cpp/raw ...)`

This pattern works well but requires:
- Build step before running
- Recompilation when library source changes
- Restart to pick up changes

The proposed runtime loading would eliminate these requirements for development.

---

## Conclusion

**Dynamic runtime library loading is absolutely feasible** with jank's existing infrastructure. The key components already exist:

1. ✅ `load_object(data, size, name)` - Load object bytes from memory
2. ✅ `load_ir_module()` - Load LLVM IR modules
3. ✅ `eval_string()` - Compile and execute C++ code
4. ✅ ORC JIT with symbol resolution
5. 🔧 **Need**: Source → Object compilation pipeline

The recommended approach is **Approach 2**: compile C/C++ source to object files in memory using `clang::CompilerInstance`, then load via the existing `load_object()` infrastructure.

This would enable a workflow like:
```jank
;; At runtime, no precompilation needed!
(load-native-library :source "path/to/flecs.c")
(load-native-library :source "path/to/imgui.cpp")
(load-native-library :source "path/to/jolt.cpp")

;; Use immediately
(def world (ecs/init))
(imgui/create-context)
(physics/create-world)
```
