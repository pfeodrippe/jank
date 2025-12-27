#include <cstdlib>

#include <jank/runtime/core/optimization.hpp>

/* This must go last; doctest and glog both define CHECK and family. */
#include <doctest/doctest.h>

namespace jank::runtime
{
  TEST_SUITE("optimization config")
  {
    TEST_CASE("default values")
    {
      optimization_config config;
      config.reset_to_defaults();

      CHECK(config.integer_cache == true);
      CHECK(config.real_cache == true);
      CHECK(config.arena_enabled == true);
      CHECK(config.primitive_loops == false);
      CHECK(config.escape_analysis == false);
      CHECK(config.allocation_stats == false);
      CHECK(config.gc_verbose == false);
    }

    TEST_CASE("singleton instance")
    {
      auto &config1{ optimization_config::instance() };
      auto &config2{ optimization_config::instance() };
      CHECK(&config1 == &config2);
    }

    TEST_CASE("convenience accessors")
    {
      auto &config{ optimization_config::instance() };
      config.reset_to_defaults();

      CHECK(use_integer_cache() == true);
      CHECK(use_real_cache() == true);
      CHECK(use_arena() == true);

      /* Modify and verify */
      config.integer_cache = false;
      CHECK(use_integer_cache() == false);

      config.reset_to_defaults();
      CHECK(use_integer_cache() == true);
    }

    TEST_CASE("environment variable loading")
    {
      auto &config{ optimization_config::instance() };
      config.reset_to_defaults();

      /* Set env vars */
      setenv("JANK_OPT_INTEGER_CACHE", "0", 1);
      setenv("JANK_OPT_REAL_CACHE", "false", 1);
      setenv("JANK_OPT_ARENA", "no", 1);
      setenv("JANK_OPT_PRIMITIVE_LOOPS", "1", 1);
      setenv("JANK_OPT_ALLOCATION_STATS", "yes", 1);

      config.load_from_env();

      CHECK(config.integer_cache == false);
      CHECK(config.real_cache == false);
      CHECK(config.arena_enabled == false);
      CHECK(config.primitive_loops == true);
      CHECK(config.allocation_stats == true);

      /* Cleanup */
      unsetenv("JANK_OPT_INTEGER_CACHE");
      unsetenv("JANK_OPT_REAL_CACHE");
      unsetenv("JANK_OPT_ARENA");
      unsetenv("JANK_OPT_PRIMITIVE_LOOPS");
      unsetenv("JANK_OPT_ALLOCATION_STATS");

      config.reset_to_defaults();
    }
  }
}
