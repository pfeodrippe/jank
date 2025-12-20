#include <chrono>

#include <jank/runtime/core/arena.hpp>

/* This must go last; doctest and glog both define CHECK and family. */
#include <doctest/doctest.h>

namespace jank::runtime
{
  TEST_SUITE("arena allocator")
  {
    TEST_CASE("basic allocation")
    {
      arena a;

      /* Allocate some memory */
      void *p1{ a.alloc(64) };
      CHECK(p1 != nullptr);

      void *p2{ a.alloc(128) };
      CHECK(p2 != nullptr);

      /* Different allocations should be at different addresses */
      CHECK(p1 != p2);

      auto stats{ a.get_arena_stats() };
      CHECK(stats.total_used == 192);  /* 64 + 128 */
      CHECK(stats.chunk_count >= 1);
    }

    TEST_CASE("alignment")
    {
      arena a;

      /* Request 1 byte but 16-byte alignment */
      void *p1{ a.alloc(1, 16) };
      CHECK((reinterpret_cast<uintptr_t>(p1) % 16) == 0);

      /* Request with 32-byte alignment */
      void *p2{ a.alloc(1, 32) };
      CHECK((reinterpret_cast<uintptr_t>(p2) % 32) == 0);

      /* Request with 64-byte alignment */
      void *p3{ a.alloc(1, 64) };
      CHECK((reinterpret_cast<uintptr_t>(p3) % 64) == 0);
    }

    TEST_CASE("reset")
    {
      arena a;

      /* Allocate some memory */
      a.alloc(1000);
      a.alloc(2000);

      auto stats_before{ a.get_stats() };
      CHECK(stats_before.total_used >= 3000);

      /* Reset */
      a.reset();

      auto stats_after{ a.get_arena_stats() };
      CHECK(stats_after.total_used == 0);
      /* Chunks should be retained */
      CHECK(stats_after.chunk_count >= 1);
      CHECK(stats_after.total_allocated >= stats_before.total_allocated);
    }

    TEST_CASE("large allocation")
    {
      arena a;

      /* Allocate something larger than max_small_alloc (4096) */
      void *large{ a.alloc(10000) };
      CHECK(large != nullptr);

      auto stats{ a.get_arena_stats() };
      CHECK(stats.large_alloc_count == 1);
      CHECK(stats.total_used >= 10000);
    }

    TEST_CASE("multiple chunks")
    {
      /* Create arena with small chunks */
      arena a(1024);

      /* Allocate more than one chunk's worth */
      for(int i = 0; i < 10; ++i)
      {
        void *p{ a.alloc(500) };
        CHECK(p != nullptr);
      }

      auto stats{ a.get_arena_stats() };
      CHECK(stats.chunk_count >= 5);  /* 5000 bytes needs at least 5 1KB chunks */
    }

    TEST_CASE("alloc_construct")
    {
      arena a;

      struct test_struct
      {
        int x;
        double y;
        char c;
      };

      auto *ts{ a.alloc_construct<test_struct>(42, 3.14, 'A') };
      CHECK(ts != nullptr);
      CHECK(ts->x == 42);
      CHECK(ts->y == 3.14);
      CHECK(ts->c == 'A');
    }

    TEST_CASE("arena_scope")
    {
      CHECK(current_allocator == nullptr);

      arena a;
      {
        arena_scope scope(&a);
        CHECK(current_allocator == &a);

        /* Nested scope */
        arena a2;
        {
          arena_scope scope2(&a2);
          CHECK(current_allocator == &a2);
        }
        CHECK(current_allocator == &a);
      }
      CHECK(current_allocator == nullptr);
    }

    TEST_CASE("try_allocator_alloc")
    {
      /* Without active allocator */
      CHECK(try_allocator_alloc(100) == nullptr);

      /* With active arena (via allocator interface) */
      arena a;
      {
        arena_scope scope(&a);
        void *p{ try_allocator_alloc(100) };
        CHECK(p != nullptr);
      }

      /* After scope exits */
      CHECK(try_allocator_alloc(100) == nullptr);
    }

    TEST_CASE("benchmark: arena allocation")
    {
      constexpr int iterations{ 100000 };
      constexpr usize alloc_size{ 64 };
      void *last_ptr{ nullptr }; /* Store last allocation to prevent optimization */

      /* Benchmark arena allocation (cold start) */
      arena a;
      auto arena_start{ std::chrono::high_resolution_clock::now() };
      for(int i = 0; i < iterations; ++i)
      {
        last_ptr = a.alloc(alloc_size);
      }
      auto arena_end{ std::chrono::high_resolution_clock::now() };
      auto arena_us{ std::chrono::duration_cast<std::chrono::microseconds>(arena_end - arena_start).count() };

      /* Reset arena and benchmark again (to show reuse benefit) */
      a.reset();
      auto arena_reuse_start{ std::chrono::high_resolution_clock::now() };
      for(int i = 0; i < iterations; ++i)
      {
        last_ptr = a.alloc(alloc_size);
      }
      auto arena_reuse_end{ std::chrono::high_resolution_clock::now() };
      auto arena_reuse_us{ std::chrono::duration_cast<std::chrono::microseconds>(arena_reuse_end - arena_reuse_start).count() };

      MESSAGE("=== Arena Benchmark (", iterations, " allocations of ", alloc_size, " bytes) ===");
      MESSAGE("Arena (cold): ", arena_us, " us (", static_cast<double>(arena_us) / iterations * 1000, " ns/alloc)");
      MESSAGE("Arena (warm/reused): ", arena_reuse_us, " us (", static_cast<double>(arena_reuse_us) / iterations * 1000, " ns/alloc)");
      if(arena_us > 0 && arena_reuse_us > 0)
      {
        MESSAGE("Warm/cold speedup: ", static_cast<double>(arena_us) / static_cast<double>(arena_reuse_us), "x");
      }

      /* Verify we got valid pointers */
      CHECK(last_ptr != nullptr);

      /* Note: warm vs cold timing comparison removed as it's too flaky
       * (depends on CPU state, system load, etc.)
       * The benchmark output above shows the actual performance. */
    }
  }
}
