#pragma once

#include <jank/runtime/object.hpp>

namespace jank::runtime
{
  namespace obj
  {
    struct real;
    using real_ref = oref<real>;
  }

  /* Real number cache for commonly used floating-point values.
   *
   * Similar to integer cache, this caches common real values
   * to reduce allocation pressure for typical numeric code.
   *
   * Cached values:
   * - Small integers as floats: -10.0 to 100.0
   * - Common fractions: 0.0, 0.5, 1.0, etc.
   * - Special values accessible directly
   */
  struct real_cache
  {
    /* Cache range for integer-valued reals */
    static constexpr i64 int_cache_low{ -10 };
    static constexpr i64 int_cache_high{ 100 };
    static constexpr usize int_cache_size{ static_cast<usize>(int_cache_high - int_cache_low + 1) };

    /* The cache array for integer-valued reals */
    static obj::real_ref int_cache[int_cache_size];

    /* Special cached values */
    static obj::real_ref zero; /* 0.0 */
    static obj::real_ref one; /* 1.0 */
    static obj::real_ref neg_one; /* -1.0 */
    static obj::real_ref half; /* 0.5 */
    static obj::real_ref neg_half; /* -0.5 */
    static obj::real_ref two; /* 2.0 */
    static obj::real_ref ten; /* 10.0 */
    static obj::real_ref hundred; /* 100.0 */
    static obj::real_ref pi; /* 3.14159... */
    static obj::real_ref e; /* 2.71828... */

    /* Whether the cache has been initialized. */
    static bool initialized;

    /* Initialize the cache. Must be called at runtime startup. */
    static void initialize();

    /* Get a cached real or allocate a new one. */
    [[gnu::hot, gnu::flatten]]
    static obj::real_ref get(f64 value);

    /* Check if a value might be cached (quick pre-check). */
    [[gnu::const]]
    static constexpr bool might_be_cached(f64 value) noexcept
    {
      /* Only integer-valued reals in range might be cached */
      return value >= static_cast<f64>(int_cache_low) && value <= static_cast<f64>(int_cache_high);
    }

  private:
    /* Check if a float is an exact integer */
    [[gnu::const]]
    static bool is_integer(f64 value) noexcept;
  };
}
