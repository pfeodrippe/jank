#pragma once

#include <cstdlib>
#include <string_view>

namespace jank::runtime
{
  /* Optimization flags for controlling runtime behavior.
   *
   * These can be set programmatically or via environment variables:
   *   JANK_OPT_INTEGER_CACHE=0|1
   *   JANK_OPT_REAL_CACHE=0|1
   *   JANK_OPT_ARENA=0|1
   *   JANK_OPT_PRIMITIVE_LOOPS=0|1 (future)
   *   JANK_OPT_ESCAPE_ANALYSIS=0|1 (future)
   *
   * Design inspired by:
   * - Zig's build options (composable, explicit)
   * - Odin's context system (implicit but controllable)
   * - JVM's -XX flags for fine-grained tuning
   */
  struct optimization_config
  {
    /* Allocation optimizations */
    bool integer_cache{ true };    /* P0: Cache small integers (-128 to 1024) */
    bool real_cache{ true };       /* P2: Cache common float values */
    bool arena_enabled{ true };    /* P4: Allow user-controlled arena allocation */

    /* Compiler optimizations (future) */
    bool primitive_loops{ false }; /* P3: Unboxed loop variables */
    bool escape_analysis{ false }; /* P5: Stack allocation for non-escaping objects */

    /* Debug/profiling options */
    bool allocation_stats{ false }; /* Track allocation counts per type */
    bool gc_verbose{ false };       /* Verbose GC logging */

    /* Get the global configuration instance. */
    static optimization_config &instance();

    /* Load configuration from environment variables.
     * Called automatically at runtime startup. */
    void load_from_env();

    /* Reset to defaults. */
    void reset_to_defaults();

  private:
    static bool env_bool(char const *name, bool default_value);
  };

  /* Convenience accessors for hot paths. */
  [[gnu::hot]]
  inline bool use_integer_cache()
  {
    return optimization_config::instance().integer_cache;
  }

  [[gnu::hot]]
  inline bool use_real_cache()
  {
    return optimization_config::instance().real_cache;
  }

  [[gnu::hot]]
  inline bool use_arena()
  {
    return optimization_config::instance().arena_enabled;
  }
}
