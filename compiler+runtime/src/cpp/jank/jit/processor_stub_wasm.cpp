/* WASM JIT processor - provides basic eval support via CppInterOp interpreter */

#include <jank/jit/processor.hpp>
#include <jank/jit/interpreter.hpp>
#include <jank/util/fmt/print.hpp>

/* WASM builds always use CppInterOp */
#include <Interpreter/Compatibility.h>
#include <clang/Interpreter/CppInterOp.h>
#include <llvm/Support/Error.h>
#include <stdexcept>

namespace jank::jit
{
  processor::processor(jtl::immutable_string const &)
  {
    /* WASM builds use the standalone interpreter from jit::get_interpreter()
     * The interpreter is created lazily on first use */

    /* Ensure interpreter is initialized */
    if(!init_wasm_interpreter())
    {
      util::println("Warning: Failed to initialize WASM interpreter");
    }

    /* Get the interpreter instance - this is a standalone interpreter,
     * not the one from runtime::context (which doesn't exist in WASM) */
    interpreter.reset(get_interpreter());
  }

  processor::~processor()
  {
    /* Don't delete the interpreter - it's managed by jit::get_interpreter() */
    interpreter.release();
  }

  void processor::eval_string(jtl::immutable_string const &s) const
  {
    if(!interpreter)
    {
      throw std::runtime_error("WASM interpreter not available");
    }

    auto err = interpreter->ParseAndExecute({ s.data(), s.size() });
    if(err)
    {
      llvm::logAllUnhandledErrors(std::move(err), llvm::errs(), "error: ");
      throw std::runtime_error("Failed to evaluate C++ code in WASM");
    }
  }

  void processor::load_object(jtl::immutable_string_view const &) const
  {
    /* Object file loading not supported in WASM */
    throw std::runtime_error("load_object not supported in WASM");
  }

  jtl::string_result<void> processor::load_dynamic_library(jtl::immutable_string const &) const
  {
    /* Dynamic library loading not supported in WASM */
    return jtl::err(jtl::immutable_string("load_dynamic_library not supported in WASM"));
  }

  void processor::load_ir_module(llvm::orc::ThreadSafeModule &&) const
  {
    /* IR module loading not supported in WASM */
    throw std::runtime_error("load_ir_module not supported in WASM");
  }

  void
  processor::load_bitcode(jtl::immutable_string const &, jtl::immutable_string_view const &) const
  {
    /* Bitcode loading not supported in WASM */
    throw std::runtime_error("load_bitcode not supported in WASM");
  }

  jtl::string_result<void> processor::remove_symbol(jtl::immutable_string const &) const
  {
    /* Symbol removal not supported in WASM */
    return jtl::err(jtl::immutable_string("remove_symbol not supported in WASM"));
  }

  jtl::string_result<void *> processor::find_symbol(jtl::immutable_string const &) const
  {
    /* Symbol lookup not implemented for WASM yet */
    return jtl::err(jtl::immutable_string("find_symbol not yet implemented in WASM"));
  }

  jtl::result<void, jtl::immutable_string>
  processor::load_dynamic_libs(native_vector<jtl::immutable_string> const &) const
  {
    /* Dynamic library loading not supported in WASM */
    return jtl::err(jtl::immutable_string("load_dynamic_libs not supported in WASM"));
  }

  jtl::option<jtl::immutable_string>
  processor::find_dynamic_lib(jtl::immutable_string const &) const
  {
    /* Dynamic library finding not supported in WASM */
    return jtl::none;
  }
}
