#include <cmath>

#include <jank/runtime/core/real_cache.hpp>
#include <jank/runtime/obj/number.hpp>

namespace jank::runtime
{
  /* Static member definitions */
  obj::real_ref real_cache::int_cache[int_cache_size]{};
  obj::real_ref real_cache::zero{};
  obj::real_ref real_cache::one{};
  obj::real_ref real_cache::neg_one{};
  obj::real_ref real_cache::half{};
  obj::real_ref real_cache::neg_half{};
  obj::real_ref real_cache::two{};
  obj::real_ref real_cache::ten{};
  obj::real_ref real_cache::hundred{};
  obj::real_ref real_cache::pi{};
  obj::real_ref real_cache::e{};
  bool real_cache::initialized{ false };

  void real_cache::initialize()
  {
    if(initialized)
    {
      return;
    }

    /* Pre-allocate integer-valued reals in the cache range. */
    for(i64 i{ int_cache_low }; i <= int_cache_high; ++i)
    {
      auto const index{ static_cast<usize>(i - int_cache_low) };
      int_cache[index] = new(PointerFreeGC) obj::real{ static_cast<f64>(i) };
    }

    /* Set up convenient references to common values */
    zero = int_cache[static_cast<usize>(0 - int_cache_low)];
    one = int_cache[static_cast<usize>(1 - int_cache_low)];
    neg_one = int_cache[static_cast<usize>(-1 - int_cache_low)];
    two = int_cache[static_cast<usize>(2 - int_cache_low)];
    ten = int_cache[static_cast<usize>(10 - int_cache_low)];
    hundred = int_cache[static_cast<usize>(100 - int_cache_low)];

    /* Pre-allocate special fractional values */
    half = new(PointerFreeGC) obj::real{ 0.5 };
    neg_half = new(PointerFreeGC) obj::real{ -0.5 };
    pi = new(PointerFreeGC) obj::real{ 3.14159265358979323846 };
    e = new(PointerFreeGC) obj::real{ 2.71828182845904523536 };

    initialized = true;
  }

  bool real_cache::is_integer(f64 const value) noexcept
  {
    return std::floor(value) == value && std::isfinite(value);
  }

  obj::real_ref real_cache::get(f64 const value)
  {
    /* Fast path: cache is initialized and value might be cached */
    if(initialized && might_be_cached(value)) [[likely]]
    {
      /* Check if it's an exact integer in range */
      if(is_integer(value))
      {
        auto const int_value{ static_cast<i64>(value) };
        auto const index{ static_cast<usize>(int_value - int_cache_low) };
        return int_cache[index];
      }
    }

    /* Check for special fractional values using bit-exact comparison.
     * Since 0.5 and -0.5 are exactly representable in IEEE 754,
     * we can safely compare them. */
    if(initialized)
    {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfloat-equal"
      if(value == 0.5)
      {
        return half;
      }
      if(value == -0.5)
      {
        return neg_half;
      }
#pragma clang diagnostic pop
      /* Note: We don't check for pi/e as exact equality with doubles is unreliable */
    }

    /* Slow path: allocate new real */
    return new(PointerFreeGC) obj::real{ value };
  }
}
