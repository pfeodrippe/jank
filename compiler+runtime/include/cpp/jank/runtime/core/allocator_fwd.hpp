#pragma once

#include <jtl/primitive.hpp>

namespace jank::runtime
{
  /* Abstract allocator interface that users can implement.
   *
   * This allows complete user control over memory allocation strategy.
   * Implementations can use:
   * - Bump-pointer allocation (arena)
   * - Pool allocation
   * - Custom memory pools
   * - Specialized allocators for specific use cases
   *
   * The allocator is set as thread-local and checked by make_box operations.
   */
  struct allocator
  {
    virtual ~allocator() = default;

    /* Allocate memory with given size and alignment.
     * Returns nullptr on failure. */
    [[gnu::hot]]
    virtual void *alloc(jtl::usize size, jtl::usize alignment) = 0;

    /* Free previously allocated memory.
     * For arena allocators, this is a no-op since arenas don't support individual frees.
     * For pool allocators, this returns the memory to the pool. */
    virtual void free(void * /*ptr*/, jtl::usize /*size*/, jtl::usize /*alignment*/)
    {
    }

    /* Optional: reset the allocator (e.g., for arena reuse) */
    virtual void reset()
    {
    }

    /* Optional: get allocation statistics */
    struct stats
    {
      jtl::usize total_allocated{};
      jtl::usize total_used{};
    };

    virtual stats get_stats() const
    {
      return {};
    }
  };

  /* Thread-local current allocator (nullptr = use GC).
   * Declared extern here, defined in arena.cpp to avoid JIT issues with inline thread_local.
   * visibility("default") + used is required for Linux emulated TLS - the __emutls_v.* symbol
   * must be visible and not stripped for the ORC JIT to resolve it. */
  [[gnu::visibility("default"), gnu::used]]
  extern thread_local allocator *current_allocator;

} // namespace jank::runtime
