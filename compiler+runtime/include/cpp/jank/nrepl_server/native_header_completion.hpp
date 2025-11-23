#pragma once

#include <string>
#include <vector>

#include <jank/runtime/ns.hpp>

namespace jank::nrepl_server::asio
{
  std::vector<std::string> enumerate_native_header_functions(
    jank::runtime::ns::native_alias const &alias,
    std::string const &prefix);
}
