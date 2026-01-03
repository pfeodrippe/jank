#pragma once

#include <jank/error.hpp>
#include <jtl/immutable_string.hpp>
#include <jtl/result.hpp>
#include <jank/runtime/obj/symbol.hpp>

namespace jank::aot
{
  struct processor
  {
    jtl::result<void, error_ref> build_executable(jtl::immutable_string const &module) const;
    jtl::result<void, error_ref> compile_object(jtl::immutable_string const &module_name,
                                                jtl::immutable_string const &cpp_source) const;
  };

  jtl::result<std::string, error_ref> generate_entrypoint_source(runtime::object_ref const var,
                                                                 jtl::immutable_string const &name,
                                                                 runtime::object_ref const schema);
}
