# iOS JIT - Prelude Header Parsing Crash

Date: 2025-12-22

## Problem
When trying to JIT compile on iOS without a precompiled header (PCH), the app crashes when parsing the prelude header.

## Crash Stack
```
clang::Decl::castFromDeclContext(clang::DeclContext const*) + 0
clang::Sema::forRedeclarationInCurContext() const + 16
clang::Sema::ActOnTag(...) + 2876
clang::Parser::ParseClassSpecifier(...) + 6480
clang::Parser::ParseDeclarationSpecifiers(...) + 3408
...
clang::IncrementalParser::Parse(llvm::StringRef) + 624
clang::Interpreter::Parse(llvm::StringRef) + 388
Cpp::Interpreter::process(...) + 60
jank::jit::processor::processor(...)
```

## Analysis
- The DeclContext is null when trying to parse a namespace
- This suggests the interpreter isn't fully initialized
- x0 register is 0 (null) when `castFromDeclContext` is called

## Attempted Solutions
1. Added `-std=gnu++20` to iOS JIT flags - fixed C++20 concept parsing errors
2. Added iOS SDK sysroot for C++ standard library headers
3. Tried to parse prelude header after interpreter creation

## Root Cause Hypothesis
The CppInterOp/Clang interpreter for iOS might:
1. Not be fully compatible with our LLVM build
2. Need additional initialization steps
3. Have issues with the incremental parsing mode

## Recommendations
1. **Pre-build PCH on host**: Instead of parsing headers at runtime, build the PCH during the iOS build process (cross-compile from macOS to iOS)
2. **Investigate LLVM build**: Check if the iOS LLVM was built with all required components
3. **Debug interpreter initialization**: Add more logging to understand what's happening during CreateInterpreter

## Files Modified
- `CMakeLists.txt`: Added `-std=gnu++20` to iOS flags
- `jit/processor.cpp`: Added prelude parsing when no PCH available
- `util/environment_ios.cpp`: iOS-specific environment paths
- `util/clang_ios.cpp`: iOS-specific clang utilities

## Current State
JIT builds and links successfully, but crashes when trying to parse the prelude header at runtime.
