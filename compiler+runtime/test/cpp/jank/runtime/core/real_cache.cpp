#include <jank/runtime/core/real_cache.hpp>
#include <jank/runtime/core/make_box.hpp>
#include <jank/runtime/obj/number.hpp>

/* This must go last; doctest and glog both define CHECK and family. */
#include <doctest/doctest.h>

namespace jank::runtime
{
  TEST_SUITE("real cache")
  {
    TEST_CASE("cache initialization")
    {
      /* Cache should be initialized by context before tests run.
       * If not, initialize it now for subsequent tests. */
      if(!real_cache::initialized)
      {
        real_cache::initialize();
      }
      CHECK(real_cache::initialized);
    }

    TEST_CASE("might_be_cached checks")
    {
      CHECK(real_cache::might_be_cached(0.0));
      CHECK(real_cache::might_be_cached(1.0));
      CHECK(real_cache::might_be_cached(-1.0));
      CHECK(real_cache::might_be_cached(-10.0));
      CHECK(real_cache::might_be_cached(100.0));
      CHECK(!real_cache::might_be_cached(-11.0));
      CHECK(!real_cache::might_be_cached(101.0));
      CHECK(!real_cache::might_be_cached(10000.0));
      CHECK(!real_cache::might_be_cached(-10000.0));
    }

    TEST_CASE("cached integer-valued reals are identical")
    {
      /* Integer-valued reals in cache range should return the same pointer */
      auto const zero1{ make_box(0.0) };
      auto const zero2{ make_box(0.0) };
      CHECK(zero1.data == zero2.data);

      auto const one1{ make_box(1.0) };
      auto const one2{ make_box(1.0) };
      CHECK(one1.data == one2.data);

      auto const neg1{ make_box(-1.0) };
      auto const neg2{ make_box(-1.0) };
      CHECK(neg1.data == neg2.data);

      auto const low1{ make_box(-10.0) };
      auto const low2{ make_box(-10.0) };
      CHECK(low1.data == low2.data);

      auto const high1{ make_box(100.0) };
      auto const high2{ make_box(100.0) };
      CHECK(high1.data == high2.data);
    }

    TEST_CASE("special cached values")
    {
      /* Half should be cached */
      auto const half1{ make_box(0.5) };
      auto const half2{ make_box(0.5) };
      CHECK(half1.data == half2.data);

      /* Negative half should be cached */
      auto const neg_half1{ make_box(-0.5) };
      auto const neg_half2{ make_box(-0.5) };
      CHECK(neg_half1.data == neg_half2.data);
    }

    TEST_CASE("uncached reals are different allocations")
    {
      /* Values outside cache range should allocate new objects */
      auto const big1{ make_box(10000.0) };
      auto const big2{ make_box(10000.0) };
      /* Different allocations */
      CHECK(big1.data != big2.data);
      /* But same value */
      CHECK(big1->data == big2->data);

      /* Non-integer values (except special ones) should allocate */
      auto const frac1{ make_box(0.123) };
      auto const frac2{ make_box(0.123) };
      CHECK(frac1.data != frac2.data);
      CHECK(frac1->data == frac2->data);
    }

    TEST_CASE("cached real values are correct")
    {
      /* Verify the cached reals have correct values */
      for(i64 i{ real_cache::int_cache_low }; i <= real_cache::int_cache_high; ++i)
      {
        auto const boxed{ make_box(static_cast<f64>(i)) };
        CHECK(boxed->data == static_cast<f64>(i));
      }
    }

    TEST_CASE("cache boundaries")
    {
      /* Test exact boundaries */
      auto const at_low{ make_box(static_cast<f64>(real_cache::int_cache_low)) };
      auto const below_low{ make_box(static_cast<f64>(real_cache::int_cache_low - 1)) };
      CHECK(at_low->data == static_cast<f64>(real_cache::int_cache_low));
      CHECK(below_low->data == static_cast<f64>(real_cache::int_cache_low - 1));

      auto const at_high{ make_box(static_cast<f64>(real_cache::int_cache_high)) };
      auto const above_high{ make_box(static_cast<f64>(real_cache::int_cache_high + 1)) };
      CHECK(at_high->data == static_cast<f64>(real_cache::int_cache_high));
      CHECK(above_high->data == static_cast<f64>(real_cache::int_cache_high + 1));

      /* Verify boundary behavior for caching */
      auto const at_low2{ make_box(static_cast<f64>(real_cache::int_cache_low)) };
      CHECK(at_low.data == at_low2.data); /* Same pointer */

      auto const below_low2{ make_box(static_cast<f64>(real_cache::int_cache_low - 1)) };
      CHECK(below_low.data != below_low2.data); /* Different pointers */
    }

    TEST_CASE("static accessor reals")
    {
      /* Verify that static accessors point to the same objects */
      CHECK(real_cache::zero.data == make_box(0.0).data);
      CHECK(real_cache::one.data == make_box(1.0).data);
      CHECK(real_cache::neg_one.data == make_box(-1.0).data);
      CHECK(real_cache::two.data == make_box(2.0).data);
      CHECK(real_cache::ten.data == make_box(10.0).data);
      CHECK(real_cache::hundred.data == make_box(100.0).data);
      CHECK(real_cache::half.data == make_box(0.5).data);
      CHECK(real_cache::neg_half.data == make_box(-0.5).data);
    }
  }
}
