#include <jank/runtime/core/integer_cache.hpp>
#include <jank/runtime/core/make_box.hpp>
#include <jank/runtime/obj/number.hpp>

/* This must go last; doctest and glog both define CHECK and family. */
#include <doctest/doctest.h>

namespace jank::runtime
{
  TEST_SUITE("integer cache")
  {
    TEST_CASE("cache initialization")
    {
      /* Cache should be initialized by context before tests run.
       * If not, initialize it now for subsequent tests. */
      if(!integer_cache::initialized)
      {
        integer_cache::initialize();
      }
      CHECK(integer_cache::initialized);
    }

    TEST_CASE("in_range checks")
    {
      CHECK(integer_cache::in_range(0));
      CHECK(integer_cache::in_range(1));
      CHECK(integer_cache::in_range(-1));
      CHECK(integer_cache::in_range(-128));
      CHECK(integer_cache::in_range(1024));
      CHECK(!integer_cache::in_range(-129));
      CHECK(!integer_cache::in_range(1025));
      CHECK(!integer_cache::in_range(10000));
      CHECK(!integer_cache::in_range(-10000));
    }

    TEST_CASE("cached integers are identical")
    {
      /* Values in cache range should return the same pointer */
      auto const zero1{ make_box(static_cast<i64>(0)) };
      auto const zero2{ make_box(static_cast<i64>(0)) };
      CHECK(zero1.data == zero2.data);

      auto const one1{ make_box(static_cast<i64>(1)) };
      auto const one2{ make_box(static_cast<i64>(1)) };
      CHECK(one1.data == one2.data);

      auto const neg1{ make_box(static_cast<i64>(-1)) };
      auto const neg2{ make_box(static_cast<i64>(-1)) };
      CHECK(neg1.data == neg2.data);

      auto const low1{ make_box(static_cast<i64>(-128)) };
      auto const low2{ make_box(static_cast<i64>(-128)) };
      CHECK(low1.data == low2.data);

      auto const high1{ make_box(static_cast<i64>(1024)) };
      auto const high2{ make_box(static_cast<i64>(1024)) };
      CHECK(high1.data == high2.data);
    }

    TEST_CASE("uncached integers are different allocations")
    {
      /* Values outside cache range should allocate new objects */
      auto const big1{ make_box(static_cast<i64>(10000)) };
      auto const big2{ make_box(static_cast<i64>(10000)) };
      /* Different allocations */
      CHECK(big1.data != big2.data);
      /* But same value */
      CHECK(big1->data == big2->data);

      auto const neg_big1{ make_box(static_cast<i64>(-10000)) };
      auto const neg_big2{ make_box(static_cast<i64>(-10000)) };
      CHECK(neg_big1.data != neg_big2.data);
      CHECK(neg_big1->data == neg_big2->data);
    }

    TEST_CASE("cached integer values are correct")
    {
      /* Verify the cached integers have correct values */
      for(i64 i{ integer_cache::cache_low }; i <= integer_cache::cache_high; ++i)
      {
        auto const boxed{ make_box(i) };
        CHECK(boxed->data == i);
      }
    }

    TEST_CASE("make_box with int uses cache")
    {
      auto const from_int1{ make_box(42) };
      auto const from_int2{ make_box(42) };
      CHECK(from_int1.data == from_int2.data);
    }

    TEST_CASE("cache boundaries")
    {
      /* Test exact boundaries */
      auto const at_low{ make_box(integer_cache::cache_low) };
      auto const below_low{ make_box(integer_cache::cache_low - 1) };
      CHECK(at_low->data == integer_cache::cache_low);
      CHECK(below_low->data == integer_cache::cache_low - 1);

      auto const at_high{ make_box(integer_cache::cache_high) };
      auto const above_high{ make_box(integer_cache::cache_high + 1) };
      CHECK(at_high->data == integer_cache::cache_high);
      CHECK(above_high->data == integer_cache::cache_high + 1);

      /* Verify boundary behavior for caching */
      auto const at_low2{ make_box(integer_cache::cache_low) };
      CHECK(at_low.data == at_low2.data); /* Same pointer */

      auto const below_low2{ make_box(integer_cache::cache_low - 1) };
      CHECK(below_low.data != below_low2.data); /* Different pointers */
    }
  }
}
