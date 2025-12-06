# jank Project Overview

## What is jank?
jank is a Clojure dialect that compiles to C++. It aims for Clojure compatibility while providing native performance through C++ interop.

## Tech Stack
- **Language**: C++20
- **Build System**: CMake + Ninja
- **Compiler Infrastructure**: LLVM/Clang (CppInterOp for C++ reflection)
- **Platform**: macOS (Darwin), Linux

## Key Directories
- `compiler+runtime/src/cpp/jank/` - Compiler source
  - `analyze/` - Semantic analysis
  - `codegen/` - C++ code generation
  - `read/` - Lexer and parser
  - `runtime/` - Runtime objects and functions
- `compiler+runtime/include/cpp/jank/` - Header files
- `compiler+runtime/test/` - Test files
- `compiler+runtime/ai/` - AI session documentation

## Compilation Pipeline
1. **Read** (lex.cpp, parse.cpp) - Parse jank source into AST
2. **Analyze** (processor.cpp) - Semantic analysis, type inference
3. **Codegen** (codegen/processor.cpp) - Generate C++ code
4. **JIT/Compile** - Use Clang to compile generated C++

## Type System
- Runtime types: `object_ref` (boxed objects)
- Primitives: `i64`, `f64`, `bool`, etc.
- Type tracked in `local_binding.type` field
- `needs_box` flag determines boxing behavior
