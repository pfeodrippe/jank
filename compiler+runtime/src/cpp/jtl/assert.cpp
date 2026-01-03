#include <jank/util/cpptrace.hpp>

#include <jtl/assert.hpp>
#include <jank/util/fmt/print.hpp>

#if defined(__APPLE__)
#  include <TargetConditionals.h>
#  if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#    include <os/log.h>
#    define JANK_IOS_LOGGING 1
#  endif
#endif

namespace jtl
{
  void do_assertion_panic(immutable_string_view const &msg)
  {
#if JANK_IOS_LOGGING
    os_log_error(OS_LOG_DEFAULT, "JANK ASSERTION: %{public}s", std::string(msg).c_str());
#endif
    jank::util::println(stderr, "{}", msg);
    cpptrace::generate_trace().print();
    ::abort();
  }

  void do_assertion_throw(immutable_string_view const &msg)
  {
    throw std::runtime_error{ std::string{ msg } };
  }
}
