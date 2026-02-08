# WASM LLVM Examples

This directory contains example code, tests, and results for the WASM compiler build (Phase 0).

## 📋 Quick Links

- **[PHASE_0_RESULTS.md](./PHASE_0_RESULTS.md)** - Complete Phase 0 build results and technical details
- **[PROOF_IT_WORKS.md](./PROOF_IT_WORKS.md)** - How xeus-cpp proves clang-repl WASM works
- **[WASM_COMPILER_PLAN.md](../compiler+runtime/WASM_COMPILER_PLAN.md)** - Full implementation plan

## Purpose

These examples demonstrate how to use the LLVM 22 + CppInterOp WASM builds created in Phase 0 of the [WASM Compiler Plan](../compiler+runtime/WASM_COMPILER_PLAN.md).

## ✅ What We Built

- **LLVM 22 for WASM** (~500MB) with clangInterpreter, lldWasm, lldCommon
- **CppInterOp for WASM** (83MB module) - Full C++ interpreter in WebAssembly
- **Proof of concept** - Verified via xeus-cpp (C++ Jupyter kernel in browser)

## Build Artifacts Location

The actual build artifacts are in `/Users/pfeodrippe/dev/jank/wasm-llvm-build/` (gitignored).

## Examples

### test_interpreter.cpp

A minimal test that demonstrates:
- Creating a Clang Interpreter in WASM
- Using the LLVM 22 IncrementalCompilerBuilder API
- Parsing and executing C++ code at runtime in WebAssembly

**Compile (object file only):**
```bash
em++ -O2 -std=c++17 -fno-exceptions \
  -I/Users/pfeodrippe/dev/jank/wasm-llvm-build/llvm-build/include \
  -I/Users/pfeodrippe/dev/llvm-project/llvm/include \
  -I/Users/pfeodrippe/dev/llvm-project/clang/include \
  -I/Users/pfeodrippe/dev/jank/wasm-llvm-build/llvm-build/tools/clang/include \
  -c test_interpreter.cpp -o test_interpreter.o
```

## Notes

- Full linking requires resolving all LLVM/Clang library dependencies
- The test demonstrates that the build infrastructure works
- Actual integration into jank will happen in Phase 1+

## Related Documentation

- [WASM Compiler Plan](../compiler+runtime/WASM_COMPILER_PLAN.md) - Complete implementation plan
- [Phase 0 Summary](../compiler+runtime/WASM_COMPILER_PLAN.md#phase-0-completion-summary) - What was built
