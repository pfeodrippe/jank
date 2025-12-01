#pragma once

#include <optional>
#include <string>
#include <vector>

#include <jank/runtime/ns.hpp>

namespace jank::nrepl_server::asio
{
  /* Enumerate functions, types, and variables from a native header. */
  std::vector<std::string>
  enumerate_native_header_symbols(jank::runtime::ns::native_alias const &alias,
                                  std::string const &prefix);

  /* Enumerate macros from a native header.
   * Only includes object-like macros (not function-like) that are defined
   * in the specified header file. */
  std::vector<std::string>
  enumerate_native_header_macros(jank::runtime::ns::native_alias const &alias,
                                 std::string const &prefix);

  /* Check if a name is a defined macro.
   * Returns true if the macro is defined and was declared in the specified header. */
  bool is_native_header_macro(jank::runtime::ns::native_alias const &alias,
                              std::string const &name);

  /* Get macro info as a string (the token expansion).
   * Returns nullopt if the macro is not defined or is function-like. */
  std::optional<std::string>
  get_native_header_macro_expansion(jank::runtime::ns::native_alias const &alias,
                                    std::string const &name);
}
