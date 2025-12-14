#include <stdexcept>

#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>

#include <jank/jit/processor.hpp>

namespace
{
  constexpr char const *unsupported_msg{ "JIT is unavailable when targeting emscripten." };
}

using jtl::err;
using jtl::none;

namespace jank::jit
{
  processor::processor(jtl::immutable_string const &)
  {
  }

  processor::~processor() = default;

  void processor::eval_string(jtl::immutable_string const &) const
  {
    throw std::runtime_error{ unsupported_msg };
  }

  void processor::load_object(jtl::immutable_string_view const &) const
  {
    throw std::runtime_error{ unsupported_msg };
  }

  jtl::string_result<void> processor::load_dynamic_library(jtl::immutable_string const &) const
  {
    return err(unsupported_msg);
  }

  void processor::load_ir_module(llvm::orc::ThreadSafeModule &&) const
  {
    throw std::runtime_error{ unsupported_msg };
  }

  void
  processor::load_bitcode(jtl::immutable_string const &, jtl::immutable_string_view const &) const
  {
    throw std::runtime_error{ unsupported_msg };
  }

  jtl::string_result<void> processor::remove_symbol(jtl::immutable_string const &) const
  {
    return err(unsupported_msg);
  }

  jtl::string_result<void *> processor::find_symbol(jtl::immutable_string const &) const
  {
    return err(unsupported_msg);
  }

  jtl::result<void, jtl::immutable_string>
  processor::load_dynamic_libs(native_vector<jtl::immutable_string> const &) const
  {
    return err(jtl::immutable_string{ unsupported_msg });
  }

  jtl::option<jtl::immutable_string>
  processor::find_dynamic_lib(jtl::immutable_string const &) const
  {
    return none;
  }
}
