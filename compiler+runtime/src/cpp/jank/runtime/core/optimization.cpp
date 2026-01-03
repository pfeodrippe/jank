#include <cstdlib>
#include <cstring>

#include <jank/runtime/core/optimization.hpp>

namespace jank::runtime
{
  optimization_config &optimization_config::instance()
  {
    static optimization_config config;
    return config;
  }

  bool optimization_config::env_bool(char const *name, bool const default_value)
  {
    char const *val{ std::getenv(name) };
    if(!val)
    {
      return default_value;
    }
    /* Accept 0, false, no, off as false; everything else as true */
    if(std::strcmp(val, "0") == 0 || std::strcmp(val, "false") == 0 || std::strcmp(val, "no") == 0
       || std::strcmp(val, "off") == 0)
    {
      return false;
    }
    return true;
  }

  void optimization_config::load_from_env()
  {
    integer_cache = env_bool("JANK_OPT_INTEGER_CACHE", integer_cache);
    real_cache = env_bool("JANK_OPT_REAL_CACHE", real_cache);
    arena_enabled = env_bool("JANK_OPT_ARENA", arena_enabled);
    primitive_loops = env_bool("JANK_OPT_PRIMITIVE_LOOPS", primitive_loops);
    escape_analysis = env_bool("JANK_OPT_ESCAPE_ANALYSIS", escape_analysis);
    allocation_stats = env_bool("JANK_OPT_ALLOCATION_STATS", allocation_stats);
    gc_verbose = env_bool("JANK_OPT_GC_VERBOSE", gc_verbose);
  }

  void optimization_config::reset_to_defaults()
  {
    integer_cache = true;
    real_cache = true;
    arena_enabled = true;
    primitive_loops = false;
    escape_analysis = false;
    allocation_stats = false;
    gc_verbose = false;
  }
}
