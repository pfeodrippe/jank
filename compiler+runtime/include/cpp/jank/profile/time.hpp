#pragma once

#include <jank/type.hpp>

namespace jank::profile
{
  void configure();
  bool is_enabled();
  void enter(jtl::immutable_string_view const &region);
  void exit(jtl::immutable_string_view const &region);
  /* Only profile if not already inside clojure.core (avoids profiling internal core-to-core calls). */
  void enter_core(jtl::immutable_string_view const &region);
  void exit_core(jtl::immutable_string_view const &region);
  void report(jtl::immutable_string_view const &boundary);

  struct timer
  {
    timer() = delete;
    timer(jtl::immutable_string_view const &region);
    ~timer();

    void report(jtl::immutable_string_view const &boundary) const;

    jtl::immutable_string region;
  };
}
