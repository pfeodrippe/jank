#pragma once

#include <immer/config.hpp>
#include <immer/heap/tags.hpp>
#include <gc/gc.h>

#include <jank/runtime/core/allocator_fwd.hpp>

namespace jank::runtime
{
  /*!
   * Custom heap for immer that respects jank's current_allocator.
   *
   * When current_allocator is set (e.g., inside with-arena), allocations
   * go through the custom allocator. Otherwise, falls back to Boehm GC.
   *
   * This allows persistent data structures (vectors, maps) to be allocated
   * from arenas when desired, while maintaining GC compatibility by default.
   */
  class jank_heap
  {
  public:
    /*!
     * Allocate memory that may contain references to other GC objects.
     * Uses current_allocator if set, otherwise GC_malloc.
     */
    [[gnu::hot]]
    static void *allocate(std::size_t n)
    {
      /* Check thread-local allocator first (fast path when not set) */
      if(current_allocator) [[unlikely]]
      {
        return current_allocator->alloc(n, 16);
      }

      /* Fall back to GC (normal path) */
      auto p = GC_malloc(n);
      if(IMMER_UNLIKELY(!p))
      {
        IMMER_THROW(std::bad_alloc{});
      }
      return p;
    }

    /*!
     * Allocate memory that contains NO references (atomic/leaf data).
     * GC won't scan this memory for pointers.
     */
    [[gnu::hot]]
    static void *allocate(std::size_t n, immer::norefs_tag)
    {
      if(current_allocator) [[unlikely]]
      {
        /* Arena doesn't distinguish - it's all unscanned anyway */
        return current_allocator->alloc(n, 16);
      }

      auto p = GC_malloc_atomic(n);
      if(IMMER_UNLIKELY(!p))
      {
        IMMER_THROW(std::bad_alloc{});
      }
      return p;
    }

    /*!
     * Deallocate memory.
     *
     * When allocator is active: no-op (allocator handles cleanup on reset/destroy)
     * When no allocator: use GC_free for explicit deallocation
     *
     * Note: Arenas register their memory with GC_add_roots() so GC knows about
     * pointers from arena memory to GC memory. This prevents premature collection.
     */
    static void deallocate(std::size_t size, void *data)
    {
      (void)size;
      if(current_allocator) [[unlikely]]
      {
        /* Allocator handles cleanup on reset/destruction - no-op here */
        return;
      }
      /* Normal path: GC-allocated memory */
      GC_free(data);
    }

    static void deallocate(std::size_t size, void *data, immer::norefs_tag)
    {
      deallocate(size, data);
    }
  };

} // namespace jank::runtime
