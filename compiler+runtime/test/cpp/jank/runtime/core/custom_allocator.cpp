#include <chrono>
#include <cstdlib>
#include <vector>

#include <jank/runtime/core/arena.hpp>

/* This must go last; doctest and glog both define CHECK and family. */
#include <doctest/doctest.h>

namespace jank::runtime
{
  /* ==========================================================================
   * Example custom allocator: counting_allocator
   *
   * This demonstrates how users can implement their own allocator by
   * inheriting from the allocator interface. It's a simple allocator
   * that tracks allocation counts.
   * ========================================================================== */
  struct counting_allocator : allocator
  {
    counting_allocator() = default;

    ~counting_allocator() override
    {
      /* Free all remaining allocations */
      for(auto *ptr : allocations_)
      {
        if(ptr)
        {
          std::free(ptr);
        }
      }
    }

    void *alloc(usize const size, usize const alignment = 16) override
    {
      void *ptr{ nullptr };
      if(posix_memalign(&ptr, alignment, size) != 0)
      {
        return nullptr;
      }

      allocations_.push_back(ptr);
      ++alloc_count_;
      total_bytes_ += size;

      return ptr;
    }

    void free(void *ptr, usize /*size*/, usize /*alignment*/) override
    {
      if(!ptr)
      {
        return;
      }

      auto it = std::find(allocations_.begin(), allocations_.end(), ptr);
      if(it != allocations_.end())
      {
        *it = nullptr;
        ++free_count_;
        std::free(ptr);
      }
    }

    void reset() override
    {
      for(auto *ptr : allocations_)
      {
        if(ptr)
        {
          std::free(ptr);
        }
      }
      allocations_.clear();
      alloc_count_ = 0;
      free_count_ = 0;
      total_bytes_ = 0;
    }

    allocator::stats get_stats() const override
    {
      return { total_bytes_, total_bytes_ };
    }

    usize get_alloc_count() const
    {
      return alloc_count_;
    }

    usize get_free_count() const
    {
      return free_count_;
    }

  private:
    usize alloc_count_{};
    usize free_count_{};
    usize total_bytes_{};
    std::vector<void *> allocations_;
  };

  TEST_SUITE("custom allocator")
  {
    TEST_CASE("implements allocator interface")
    {
      counting_allocator ca;

      /* Test that it implements the allocator interface correctly */
      allocator *a = &ca;

      void *p1 = a->alloc(64, 16);
      CHECK(p1 != nullptr);

      void *p2 = a->alloc(128, 16);
      CHECK(p2 != nullptr);
      CHECK(p1 != p2);

      auto stats = a->get_stats();
      CHECK(stats.total_allocated == 192);

      a->free(p1, 64, 16);
      a->free(p2, 128, 16);

      CHECK(ca.get_alloc_count() == 2);
      CHECK(ca.get_free_count() == 2);
    }

    TEST_CASE("works with allocator_scope")
    {
      counting_allocator ca;

      CHECK(current_allocator == nullptr);

      {
        allocator_scope scope(&ca);
        CHECK(current_allocator == &ca);

        /* Allocate through the current_allocator */
        void *ptr = current_allocator->alloc(64, 16);
        CHECK(ptr != nullptr);
        CHECK(ca.get_alloc_count() == 1);
      }

      CHECK(current_allocator == nullptr);
    }

    TEST_CASE("works with try_allocator_alloc")
    {
      counting_allocator ca;

      /* Without active allocator, try_allocator_alloc returns nullptr */
      void *no_alloc = try_allocator_alloc(64);
      CHECK(no_alloc == nullptr);

      /* With active allocator, it uses that allocator */
      {
        allocator_scope scope(&ca);
        void *ptr = try_allocator_alloc(64);
        CHECK(ptr != nullptr);
        CHECK(ca.get_alloc_count() == 1);
      }
    }

    TEST_CASE("alignment")
    {
      counting_allocator ca;

      void *p16 = ca.alloc(1, 16);
      CHECK((reinterpret_cast<uintptr_t>(p16) % 16) == 0);

      void *p32 = ca.alloc(1, 32);
      CHECK((reinterpret_cast<uintptr_t>(p32) % 32) == 0);

      void *p64 = ca.alloc(1, 64);
      CHECK((reinterpret_cast<uintptr_t>(p64) % 64) == 0);
    }

    TEST_CASE("benchmark: custom allocator vs arena")
    {
      constexpr size_t num_allocs = 100000;
      constexpr size_t alloc_size = 64;

      /* Benchmark counting_allocator */
      counting_allocator ca;
      auto start_custom = std::chrono::high_resolution_clock::now();
      for(size_t i = 0; i < num_allocs; ++i)
      {
        void *ptr = ca.alloc(alloc_size);
        (void)ptr;
      }
      auto end_custom = std::chrono::high_resolution_clock::now();
      auto custom_time
        = std::chrono::duration_cast<std::chrono::microseconds>(end_custom - start_custom).count();

      /* Benchmark arena (should be faster due to bump allocation) */
      arena a;
      auto start_arena = std::chrono::high_resolution_clock::now();
      for(size_t i = 0; i < num_allocs; ++i)
      {
        void *ptr = a.alloc(alloc_size);
        (void)ptr;
      }
      auto end_arena = std::chrono::high_resolution_clock::now();
      auto arena_time
        = std::chrono::duration_cast<std::chrono::microseconds>(end_arena - start_arena).count();

      MESSAGE("=== Custom Allocator Benchmark (" << num_allocs << " allocations of " << alloc_size
                                                 << " bytes) ===");
      MESSAGE("Counting allocator: " << custom_time << " us ("
                                     << (static_cast<double>(custom_time) / num_allocs * 1000)
                                     << " ns/alloc)");
      MESSAGE("Arena: " << arena_time << " us ("
                        << (static_cast<double>(arena_time) / num_allocs * 1000) << " ns/alloc)");

      if(arena_time > 0)
      {
        MESSAGE("Arena speedup: " << (static_cast<double>(custom_time) / arena_time) << "x");
      }

      /* Arena should be at least somewhat faster than malloc-based allocator */
      /* Note: We're not enforcing this as a test because benchmark times vary */
    }

    TEST_CASE("polymorphic dispatch via allocator*")
    {
      /* This test verifies that the vtable dispatch is working correctly */
      counting_allocator ca;
      arena a;

      /* Both can be used through allocator* */
      allocator *allocs[] = { &ca, &a };

      for(allocator *alloc : allocs)
      {
        void *p1 = alloc->alloc(64, 16);
        CHECK(p1 != nullptr);

        void *p2 = alloc->alloc(128, 16);
        CHECK(p2 != nullptr);
        CHECK(p1 != p2);

        auto stats = alloc->get_stats();
        CHECK(stats.total_used >= 192);

        /* Verify counting_allocator tracked allocations before reset */
        if(alloc == &ca)
        {
          CHECK(ca.get_alloc_count() == 2);
        }

        alloc->reset();
        auto stats_after = alloc->get_stats();
        CHECK(stats_after.total_used == 0);
      }
    }
  }
}
