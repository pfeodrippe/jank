#pragma once

/* This header provides a unified way to access the CppInterOp interpreter
 * that works for both native and WASM builds.
 *
 * Native builds: Uses runtime::__rt_ctx->jit_prc.interpreter
 * WASM builds: Uses a standalone interpreter instance (initialized on first use)
 */

#if !defined(JANK_TARGET_EMSCRIPTEN) || defined(JANK_HAS_CPPINTEROP)

namespace Cpp
{
  class Interpreter;
}

namespace jank::jit
{
  /* Returns the CppInterOp interpreter instance.
   * In native builds, this returns __rt_ctx->jit_prc.interpreter.
   * In WASM builds, this returns a standalone interpreter (created on first call).
   * Returns nullptr if interpreter is not available. */
  Cpp::Interpreter *get_interpreter();

  /* For WASM: Initialize the standalone interpreter.
   * No-op for native builds (interpreter is created via jit::processor).
   * Returns true on success, false on failure. */
  bool init_wasm_interpreter();

  /* For WASM: Check if interpreter is initialized.
   * Always returns true for native builds. */
  bool has_interpreter();
}

#else

/* Stub declarations when CppInterOp is not available */
namespace Cpp
{
  class Interpreter;
}

namespace jank::jit
{
  inline Cpp::Interpreter *get_interpreter()
  {
    return nullptr;
  }

  inline bool init_wasm_interpreter()
  {
    return false;
  }

  inline bool has_interpreter()
  {
    return false;
  }
}

#endif
