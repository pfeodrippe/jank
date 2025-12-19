#include <chrono>

#include <jank/runtime/core/integer_cache.hpp>
#include <jank/runtime/core/make_box.hpp>
#include <jank/runtime/obj/number.hpp>

/* This must go last; doctest and glog both define CHECK and family. */
#include <doctest/doctest.h>

namespace jank::runtime
{
  TEST_SUITE("integer cache benchmark")
  {
    TEST_CASE("benchmark cached vs uncached allocations")
    {
      /* Ensure cache is initialized */
      if(!integer_cache::initialized)
      {
        integer_cache::initialize();
      }

      constexpr i64 iterations{ 1000000 };
      volatile i64 sink{ 0 }; /* Prevent optimization */

      /* Benchmark cached allocations (values in range [-128, 1024]) */
      auto const cached_start{ std::chrono::high_resolution_clock::now() };
      for(i64 i{ 0 }; i < iterations; ++i)
      {
        /* Use values that cycle through cache range */
        auto const val{ i % (integer_cache::cache_high - integer_cache::cache_low + 1) + integer_cache::cache_low };
        auto const boxed{ make_box(val) };
        sink = boxed->data;
      }
      auto const cached_end{ std::chrono::high_resolution_clock::now() };
      auto const cached_us{ std::chrono::duration_cast<std::chrono::microseconds>(cached_end - cached_start).count() };

      /* Benchmark uncached allocations (values outside range) */
      auto const uncached_start{ std::chrono::high_resolution_clock::now() };
      for(i64 i{ 0 }; i < iterations; ++i)
      {
        /* Use values outside cache range - start at 10000 */
        auto const val{ 10000 + i };
        auto const boxed{ make_box(val) };
        sink = boxed->data;
      }
      auto const uncached_end{ std::chrono::high_resolution_clock::now() };
      auto const uncached_us{ std::chrono::duration_cast<std::chrono::microseconds>(uncached_end - uncached_start).count() };

      /* Report via MESSAGE */
      MESSAGE("=== Integer Cache Benchmark (1M iterations) ===");
      MESSAGE("Cached (in range):     ", cached_us, " us");
      MESSAGE("Uncached (out of range): ", uncached_us, " us");
      if(uncached_us > 0)
      {
        MESSAGE("Speedup: ", (static_cast<double>(uncached_us) / static_cast<double>(cached_us)), "x");
      }

      /* Cached should be significantly faster */
      CHECK(cached_us < uncached_us);

      /* Use sink to prevent optimization */
      CHECK(sink != -999999);
    }

    TEST_CASE("benchmark loop simulation")
    {
      /* Simulates a typical loop like:
       * (loop [i 0 sum 0]
       *   (if (< i 1000000)
       *     (recur (inc i) (+ sum i))
       *     sum))
       */
      if(!integer_cache::initialized)
      {
        integer_cache::initialize();
      }

      constexpr i64 loop_iterations{ 1000000 };
      volatile i64 sink{ 0 };

      /* Simulate loop with cached integers (i stays in cache range for most iterations) */
      auto const start{ std::chrono::high_resolution_clock::now() };

      obj::integer_ref i_box{ make_box(static_cast<i64>(0)) };
      obj::integer_ref sum_box{ make_box(static_cast<i64>(0)) };

      for(i64 iter{ 0 }; iter < loop_iterations; ++iter)
      {
        /* Simulate: i = (inc i) */
        i64 const new_i{ i_box->data + 1 };
        i_box = make_box(new_i);

        /* Simulate: sum = (+ sum i) */
        i64 const new_sum{ sum_box->data + new_i };
        sum_box = make_box(new_sum);
      }

      auto const end{ std::chrono::high_resolution_clock::now() };
      auto const duration_us{ std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() };

      sink = sum_box->data;

      MESSAGE("=== Loop Simulation (1M iterations) ===");
      MESSAGE("Total time: ", duration_us, " us");
      MESSAGE("Per iteration: ", (static_cast<double>(duration_us) / loop_iterations), " us");
      MESSAGE("Final sum: ", sink);

      /* Verify correctness */
      i64 expected_sum{ 0 };
      for(i64 i{ 1 }; i <= loop_iterations; ++i)
      {
        expected_sum += i;
      }
      CHECK(sink == expected_sum);
    }
  }
}
