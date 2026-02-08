#include <jank/runtime/core/integer_cache.hpp>
#include <jank/runtime/obj/number.hpp>

namespace jank::runtime
{
  /* Static member definitions */
  obj::integer_ref integer_cache::cache[cache_size]{};
  bool integer_cache::initialized{ false };

  void integer_cache::initialize()
  {
    if(initialized)
    {
      return;
    }

    /* Pre-allocate all integers in the cache range.
     * These are allocated once and never freed. */
    for(i64 i{ cache_low }; i <= cache_high; ++i)
    {
      auto const index{ static_cast<usize>(i - cache_low) };
      /* Allocate directly without going through make_box to avoid recursion */
      cache[index] = new(PointerFreeGC) obj::integer{ i };
    }

    initialized = true;
  }

  obj::integer_ref integer_cache::get(i64 const value)
  {
    /* Fast path: value is in cache range and cache is initialized */
    if(initialized && in_range(value)) [[likely]]
    {
      auto const index{ static_cast<usize>(value - cache_low) };
      return cache[index];
    }

    /* Slow path: allocate new integer (either outside range or cache not initialized) */
    return new(PointerFreeGC) obj::integer{ value };
  }
}
