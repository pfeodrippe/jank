#include <jank/jit/interpreter.hpp>

#if !defined(JANK_TARGET_EMSCRIPTEN) || defined(JANK_HAS_CPPINTEROP)

  #include <Interpreter/Compatibility.h>
  #include <clang/Interpreter/CppInterOp.h>

  #ifdef JANK_TARGET_WASM
    #include <memory>
    #include <vector>

namespace jank::jit
{
  /* Standalone interpreter for WASM - created on first use */
  static std::unique_ptr<Cpp::Interpreter> wasm_interpreter;
  static bool wasm_interpreter_init_attempted = false;

  bool init_wasm_interpreter()
  {
    if(wasm_interpreter)
    {
      return true;
    }

    if(wasm_interpreter_init_attempted)
    {
      return false;
    }

    wasm_interpreter_init_attempted = true;

    /* WASM-specific interpreter arguments */
    std::vector<char const *> args{
      "-std=c++20",
      "-target",
      "wasm32-unknown-emscripten",
      "-DJANK_TARGET_WASM=1",
      "-DJANK_TARGET_EMSCRIPTEN=1",
      /* Suppress warnings for cleaner output */
      "-w",
      /* System header paths (embedded in WASM via --embed-file) */
      "-isystem",
      "/include/c++/v1",
      "-isystem",
      "/include/clang/22/include",
      "-isystem",
      "/include",
    };

    /* Create the interpreter */
    auto *interp = static_cast<Cpp::Interpreter *>(Cpp::CreateInterpreter(args, {}));

    if(!interp)
    {
      return false;
    }

    wasm_interpreter.reset(interp);
    return true;
  }

  Cpp::Interpreter *get_interpreter()
  {
    if(!wasm_interpreter && !wasm_interpreter_init_attempted)
    {
      init_wasm_interpreter();
    }
    return wasm_interpreter.get();
  }

  bool has_interpreter()
  {
    return wasm_interpreter != nullptr;
  }
}

  #else /* Native build */

    #include <jank/runtime/context.hpp>

namespace jank::jit
{
  Cpp::Interpreter *get_interpreter()
  {
    if(!runtime::__rt_ctx)
    {
      return nullptr;
    }
    return runtime::__rt_ctx->jit_prc.interpreter.get();
  }

  bool init_wasm_interpreter()
  {
    /* No-op for native builds - interpreter is created via jit::processor */
    return true;
  }

  bool has_interpreter()
  {
    return runtime::__rt_ctx && runtime::__rt_ctx->jit_prc.interpreter;
  }
}

  #endif /* JANK_TARGET_WASM */

#endif /* CppInterOp available */
