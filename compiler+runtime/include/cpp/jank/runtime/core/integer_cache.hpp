#pragma once

#include <jank/runtime/object.hpp>

namespace jank::runtime
{
  namespace obj
  {
    struct integer;
    using integer_ref = oref<integer>;
  }

  /* Integer cache similar to JVM's Integer cache.
   * Caches boxed integers in the range [cache_low, cache_high] to avoid
   * allocating new objects for commonly used values like loop counters.
   *
   * This significantly reduces allocation pressure in tight loops like:
   *   (loop [i 0 sum 0]
   *     (if (< i 1000000)
   *       (recur (inc i) (+ sum i))
   *       sum))
   *
   * Without cache: ~4 million allocations
   * With cache: ~0 allocations (for i, most sum values still allocate)
   */
  struct integer_cache
  {
    /* Cache range: [-128, 1024]
     * - JVM uses [-128, 127] by default
     * - We use a higher upper bound to cover common loop iteration counts
     * - Small negative numbers are useful for decrements and sentinels
     */
    static constexpr i64 cache_low{ -128 };
    static constexpr i64 cache_high{ 1024 };
    static constexpr usize cache_size{ static_cast<usize>(cache_high - cache_low + 1) };

    /* The actual cache array. Initialized at runtime startup. */
    static obj::integer_ref cache[cache_size];

    /* Whether the cache has been initialized. */
    static bool initialized;

    /* Initialize the cache. Must be called before any make_box(i64) calls.
     * This is called from context initialization. */
    static void initialize();

    /* Get a cached integer or allocate a new one.
     * This is the fast path for integer boxing. */
    [[gnu::hot, gnu::flatten]]
    static obj::integer_ref get(i64 value);

    /* Check if a value is in the cache range. */
    [[gnu::const]]
    static constexpr bool in_range(i64 value) noexcept
    {
      return value >= cache_low && value <= cache_high;
    }
  };
}
