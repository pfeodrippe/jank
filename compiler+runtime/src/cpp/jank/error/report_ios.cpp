// Stub for error::report on iOS
// FTXUI is not available on iOS, so we just print to stderr

#include <jank/error.hpp>
#include <jtl/immutable_string.hpp>
#include <iostream>

namespace jank::error
{
  void report(error_ref e)
  {
    // Simple stderr output for iOS (no FTXUI TUI)
    std::cerr << "[jank error] " << e->message << std::endl;
    if(e->cause)
    {
      std::cerr << "  caused by: " << e->cause->message << std::endl;
    }
  }

  void warn(jtl::immutable_string const &msg)
  {
    // Simple stderr warning output for iOS
    std::cerr << "\033[0;33mwarning:\033[0m " << msg << std::endl;
  }
}
