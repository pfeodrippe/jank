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
   * Includes both object-like and function-like macros that are defined
   * in the specified header file. */
  std::vector<std::string>
  enumerate_native_header_macros(jank::runtime::ns::native_alias const &alias,
                                 std::string const &prefix);

  /* Check if a name is a defined macro (object-like only).
   * Returns true if the macro is defined and was declared in the specified header. */
  bool is_native_header_macro(jank::runtime::ns::native_alias const &alias,
                              std::string const &name);

  /* Check if a name is a function-like macro.
   * Returns true if the macro is defined, is function-like, and was declared in the specified header. */
  bool is_native_header_function_like_macro(jank::runtime::ns::native_alias const &alias,
                                            std::string const &name);

  /* Get the number of parameters for a function-like macro.
   * Returns nullopt if the macro is not defined or is not function-like. */
  std::optional<size_t>
  get_native_header_macro_param_count(jank::runtime::ns::native_alias const &alias,
                                      std::string const &name);

  /* Get macro info as a string (the token expansion).
   * Works for both object-like and function-like macros. */
  std::optional<std::string>
  get_native_header_macro_expansion(jank::runtime::ns::native_alias const &alias,
                                    std::string const &name);
}
