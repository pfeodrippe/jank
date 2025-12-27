# Fast iOS Cross-Compilation Plan

## Date: 2025-12-26

## Problem

The standalone compile-server for iOS is **too slow**: ~1.6 seconds for `(+ 1 2)`.

**Root cause**: Every evaluation spawns a new `clang` process:
```
[compile-server] Cross-compiling: clang-22 -c -target arm64-apple-ios17.0-simulator...
```

This incurs:
1. Process spawn overhead (~100-200ms)
2. PCH parsing overhead (~300-500ms each time)
3. Clang initialization overhead

## Current Architecture

```
┌─────────────────────────────────────────────────────────┐
│                  compile-server (macOS)                  │
│                                                          │
│  1. Receive jank code from iOS                          │
│  2. Generate C++ code                                   │
│  3. Spawn clang process  ← SLOW (per-evaluation)        │
│  4. Clang parses PCH     ← SLOW (per-evaluation)        │
│  5. Compile to ARM64 .o                                 │
│  6. Send object file to iOS                             │
└─────────────────────────────────────────────────────────┘
```

## Two Approaches to Fix

### Approach 1: Fix the Existing `persistent_compiler`

**Location**: `include/cpp/jank/compile_server/persistent_compiler.hpp`

The persistent_compiler was designed exactly for this purpose but is **disabled**:
```cpp
bool is_initialized() const
{
  // TODO: Persistent compiler is disabled until CreateFromArgs issues are fixed
  return false;
}
```

**How it should work**:
1. Create a `clang::CompilerInstance` once at startup
2. Configure it with iOS cross-compilation flags
3. Load PCH once
4. For each compilation:
   - Create in-memory source buffer
   - Run `EmitObjAction` to generate object code
   - Return object bytes directly (no file I/O)

**Current issue**: `clang::CompilerInvocation::CreateFromArgs` is failing.

**Benefits**:
- Simpler code path
- No need for JIT execution
- Direct object file emission
- Estimated 4-10x speedup (per the comments)

**Challenges**:
- Need to debug CreateFromArgs failure
- May need to manually construct CompilerInvocation instead

### Approach 2: Use CppInterOp with Cross-Compilation

**How jank normally uses CppInterOp**:
```cpp
// In jit/processor.cpp
interpreter.reset(static_cast<Cpp::Interpreter *>(
  Cpp::CreateInterpreter(args, {}, vfs, static_cast<int>(llvm::CodeModel::Large))));
```

This creates a persistent Clang interpreter that:
- Loads PCH once at startup
- Keeps Clang in memory
- Compiles incrementally
- Executes code on the host

**For iOS cross-compilation, we'd need to**:
1. Create a CppInterOp interpreter with iOS-specific flags:
   ```cpp
   std::vector<char const*> ios_args = {
     "-target", "arm64-apple-ios17.0-simulator",
     "-isysroot", "/path/to/iPhoneSimulator.sdk",
     "-include-pch", "/path/to/ios.pch",
     // ... include paths
   };
   auto* ios_interpreter = Cpp::CreateInterpreter(ios_args, {}, {});
   ```

2. After `Declare()` or `Process()`, extract the LLVM module:
   - CppInterOp doesn't expose this directly
   - Would need to modify CppInterOp or use internal APIs

3. Emit the module as ARM64 object code:
   - Use LLVM's TargetMachine to emit object code
   - Write to memory buffer

**Benefits**:
- Leverages existing CppInterOp infrastructure
- Proven incremental compilation
- Same approach jank uses for host JIT

**Challenges**:
- CppInterOp is designed for JIT execution, not object emission
- Would need to modify CppInterOp or access internal Clang APIs
- May need to handle cross-compilation edge cases in LLVM

## Recommended Approach: Fix persistent_compiler First

**Reasoning**:
1. It's already designed for exactly this use case (cross-compile to object files)
2. The code is mostly written, just needs debugging
3. No need to modify CppInterOp
4. Cleaner separation of concerns

## Investigation Results (2025-12-26)

### Attempt 1: Fix Frontend Flags

Changed driver flags to frontend (cc1) flags:
- `-target` → `-triple`
- `-c` → removed (handled by EmitObjAction)
- `-fPIC` → `-pic-level 2`
- Removed `-Xclang` prefix

**Result**: Server crashed without logging error. The `CompilerInvocation::CreateFromArgs` still has issues.

### Root Cause Analysis

The problem is more complex than just flag conversion:
1. `CreateFromArgs` expects a very specific set of cc1 flags
2. Missing required flags like `-emit-obj`, `-main-file-name`, `-mrelocation-model`
3. The output stream setup might be incorrect

### Next Steps

1. Use `clang::driver::Driver` to convert driver args to cc1 args
2. Or manually construct `CompilerInvocation` without `CreateFromArgs`
3. Debug by adding more logging to see exactly where it crashes

## Implementation Steps

### Step 1: Debug CreateFromArgs Failure

```cpp
// In persistent_compiler::compile()
bool success = clang::CompilerInvocation::CreateFromArgs(*invocation, args, diags);
if(!success)
{
  return { false, {}, "Failed to create compiler invocation: " + diag_output };
}
```

**Investigation needed**:
1. Print the exact args being passed
2. Check diagnostic output for specific errors
3. Common issues:
   - Missing required arguments
   - Conflicting flags
   - Path issues with sysroot/SDK

### Step 2: Alternative - Construct CompilerInvocation Manually

If CreateFromArgs is problematic, construct the invocation directly:

```cpp
auto invocation = std::make_shared<clang::CompilerInvocation>();

// Set target options
auto& target_opts = invocation->getTargetOpts();
target_opts.Triple = "arm64-apple-ios17.0-simulator";

// Set language options
auto& lang_opts = invocation->getLangOpts();
lang_opts.CPlusPlus = true;
lang_opts.CPlusPlus20 = true;

// Set header search options
auto& header_opts = invocation->getHeaderSearchOpts();
header_opts.Sysroot = "/path/to/iPhoneSimulator.sdk";
for(auto const& inc : include_paths) {
  header_opts.AddPath(inc, clang::frontend::Angled, false, true);
}

// Set preprocessor options for PCH
auto& pp_opts = invocation->getPreprocessorOpts();
pp_opts.ImplicitPCHInclude = pch_path;

// Set frontend options
auto& fe_opts = invocation->getFrontendOpts();
fe_opts.ProgramAction = clang::frontend::EmitObj;
```

### Step 3: Test Performance

After fixing, measure:
```bash
time clj-nrepl-eval -p 5558 "(+ 1 2)"
```

Target: < 200ms for simple expressions (vs current 1.6s)

### Step 4: If persistent_compiler Still Fails, Try CppInterOp Approach

Would need to:
1. Create iOS-targeted interpreter
2. Access `clang::Interpreter::getCompilerInstance()`
3. After each `Declare()`, access the latest `llvm::Module`
4. Use `llvm::TargetMachine::addPassesToEmitFile()` for object emission

## Additional Optimizations

### 1. PCH for Generated Code Patterns

Pre-compile common jank codegen patterns:
```cpp
// ios_jank_pch_extras.hpp
#include <jank/runtime/object.hpp>
#include <jank/runtime/core.hpp>
// Common templates that get instantiated often
template jank::runtime::object_ref jank::runtime::make_box<int64_t>(int64_t);
// etc.
```

### 2. Object Code Caching

Cache compiled object files by content hash:
```cpp
std::string code_hash = compute_hash(cpp_code);
if(auto cached = object_cache.find(code_hash)) {
  return cached->object_data;
}
// ... compile and cache
```

### 3. Parallel Compilation

For multi-form evaluations, compile in parallel:
```cpp
std::vector<std::future<compile_result>> futures;
for(auto const& form : forms) {
  futures.push_back(std::async(std::launch::async, [&] {
    return persistent_compiler.compile(form);
  }));
}
```

## Files to Modify

1. `include/cpp/jank/compile_server/persistent_compiler.hpp`
   - Debug/fix CreateFromArgs
   - Or implement manual CompilerInvocation construction

2. `include/cpp/jank/compile_server/server.hpp`
   - Integrate persistent_compiler into compilation path
   - Fall back to clang CLI if persistent_compiler fails

3. `src/cpp/compile_server_main.cpp`
   - Initialize persistent_compiler at startup
   - Configure with iOS SDK paths

## Success Criteria

- `(+ 1 2)` evaluates in < 200ms (currently ~1600ms)
- Complex expressions like `(defn foo [] 42)` in < 500ms
- No regressions in correctness
- Server startup time acceptable (< 5s for PCH loading)

## Implementation Results (2025-12-26 Evening)

### What Was Fixed

1. **persistent_compiler is now working** with Clang 22 API:
   - Fixed `DiagnosticOptions` to use new Clang 22 API (not IntrusiveRefCntPtr)
   - Fixed `TextDiagnosticPrinter` constructor signature
   - Fixed `DiagnosticsEngine` constructor signature
   - Fixed `CompilerInstance` to take invocation in constructor
   - Uses `clang::driver::Driver` API to properly convert driver args to cc1 args
   - Added filesystem check for PCH existence

2. **Timing instrumentation added**:
   - Setup time (Driver/CompilerInvocation creation): ~0ms
   - Compile time (actual clang execution): ~1500ms

### Current Performance

```
[persistent-compiler] Compiled compile_2 (34936 bytes) - setup: 0ms, compile: 1534ms, total: 1537ms
```

- **Setup: 0ms** - Persistent compiler eliminates Driver/CompilerInvocation overhead
- **Compile: 1534ms** - Clang parsing headers + codegen (WITHOUT PCH!)
- **Total: ~1.6s** - Same as before because the bottleneck is header parsing

### Why Still Slow?

The iOS-specific PCH file does not exist:
```
config.pch_path = jank_resource_dir + "/incremental.pch";
// File does not exist for iOS target
```

Without PCH, clang must parse ALL jank runtime headers from scratch for every compilation:
- `jank/runtime/object.hpp`
- `jank/runtime/core.hpp`
- All template instantiations
- ~500KB of headers per compile

### Next Steps for True Performance Improvement

#### Option 1: Build iOS-Specific PCH

Need to build a precompiled header for `arm64-apple-ios17.0-simulator` target:

```bash
clang++ -target arm64-apple-ios17.0-simulator \
  -isysroot /path/to/iPhoneSimulator.sdk \
  -std=gnu++20 \
  -Xclang -emit-pch \
  -Xclang -fincremental-extensions \
  -o ios-incremental.pch \
  prelude.hpp
```

This would need to be done during build and bundled with the compile-server.

**Challenge**: The PCH must be built with the EXACT same flags used for compilation.

#### Option 2: Investigate clang::IncrementalCompilerBuilder

Clang's incremental compilation mode (used by clang-repl) might work for cross-compilation:
- Keep AST in memory between compilations
- Only reparse the new code
- Similar to how CppInterOp works for JIT

#### Option 3: Caching Compiled Object Files

For repeated evaluations, cache the object file by content hash:
```cpp
std::string code_hash = compute_hash(cpp_code);
if(auto cached = object_cache.find(code_hash)) {
  return cached->object_data;
}
```

### Files Modified

1. `include/cpp/jank/compile_server/persistent_compiler.hpp`
   - Fixed Clang 22 API compatibility
   - Added timing instrumentation
   - Added filesystem include
   - Fixed PCH existence check
