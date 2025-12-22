// Stubs for nrepl_server native header functions on iOS
// These features are not available on iOS due to missing Boost Asio

#include <jank/runtime/ns.hpp>
#include <vector>
#include <string>

namespace jank::nrepl_server::asio
{
  bool is_native_header_macro(runtime::ns::native_alias const &, std::string const &)
  {
    // Native header macro completion not supported on iOS
    return false;
  }

  bool is_native_header_function_like_macro(runtime::ns::native_alias const &, std::string const &)
  {
    // Native header function-like macro completion not supported on iOS
    return false;
  }

  std::vector<std::string>
  enumerate_native_header_symbols(runtime::ns::native_alias const &, std::string const &)
  {
    // Native header symbol enumeration not supported on iOS
    return {};
  }
}
