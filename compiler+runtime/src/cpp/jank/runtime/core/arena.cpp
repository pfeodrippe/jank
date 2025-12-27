#include <cstdlib>
#include <cstring>

#include <gc/gc.h>
#include <jank/runtime/core/arena.hpp>

namespace jank::runtime
{
  /* Thread-local current allocator - definition (declared extern in allocator_fwd.hpp).
   * visibility("default") + used is required for Linux emulated TLS - the __emutls_v.* symbol
   * must be visible and not stripped for the ORC JIT to resolve it. */
  [[gnu::visibility("default"), gnu::used]]
  thread_local allocator *current_allocator{ nullptr };

  /* ----- chunk implementation ----- */

  arena::chunk::chunk(usize const size)
  {
    /* Allocate directly from system (bypass GC completely).
     * This gives users full control over the memory lifetime. */
    start = static_cast<char *>(std::malloc(size));
    current = start;
    end = start + size;

    /* Register this memory region with GC as a root.
     * This tells GC that this region may contain pointers to GC-managed objects,
     * so GC won't collect those objects while this arena chunk exists.
     * This is essential because immer's structural sharing can mix arena-allocated
     * nodes with GC-allocated nodes in the same data structure. */
    GC_add_roots(start, end);
  }

  arena::chunk::~chunk()
  {
    /* Free the chunk memory (we own it, not the GC) */
    if(start)
    {
      /* Unregister from GC before freeing */
      GC_remove_roots(start, end);
      std::free(start);
      start = nullptr;
      current = nullptr;
      end = nullptr;
    }
  }

  void *arena::chunk::try_alloc(usize const size, usize const alignment)
  {
    /* Align current pointer */
    auto const aligned_addr
      = (reinterpret_cast<uintptr_t>(current) + alignment - 1) & ~(alignment - 1);
    auto *aligned = reinterpret_cast<char *>(aligned_addr);

    /* Check if allocation fits */
    if(aligned + size <= end)
    {
      current = aligned + size;
      return aligned;
    }

    return nullptr;
  }

  void arena::chunk::reset()
  {
    current = start;
  }

  /* ----- arena implementation ----- */

  arena::arena()
    : arena(default_chunk_size)
  {
  }

  arena::arena(usize const chunk_size)
    : chunk_size_{ chunk_size }
  {
    add_chunk();
  }

  arena::~arena()
  {
    /* Free all chunks (they own their memory) */
    chunk *c{ first_chunk_ };
    while(c)
    {
      chunk *next{ c->next };
      delete c;
      c = next;
    }

    /* Free large allocations (we own this memory, not the GC) */
    large_alloc *la{ large_allocs_ };
    while(la)
    {
      large_alloc *next{ la->next };
      if(la->ptr)
      {
        /* Unregister from GC before freeing */
        GC_remove_roots(la->ptr, static_cast<char *>(la->ptr) + la->size);
        std::free(la->ptr);
      }
      delete la;
      la = next;
    }
  }

  void arena::add_chunk()
  {
    auto *c = new chunk(chunk_size_);
    c->next = first_chunk_;
    first_chunk_ = c;
    current_chunk_ = c;
    stats_.total_allocated += chunk_size_;
    ++stats_.chunk_count;
  }

  void *arena::alloc(usize const size, usize const alignment)
  {
    /* For large allocations, allocate directly (bypass GC) */
    if(size > max_small_alloc)
    {
      void *ptr{ std::malloc(size) };
      if(ptr)
      {
        /* Register large allocation with GC as a root */
        GC_add_roots(ptr, static_cast<char *>(ptr) + size);

        auto *la = new large_alloc;
        la->ptr = ptr;
        la->size = size;
        la->next = large_allocs_;
        large_allocs_ = la;
        stats_.total_allocated += size;
        stats_.total_used += size;
        ++stats_.large_alloc_count;
      }
      return ptr;
    }

    /* Try current chunk first (fast path) */
    if(current_chunk_)
    {
      void *ptr{ current_chunk_->try_alloc(size, alignment) };
      if(ptr)
      {
        stats_.total_used += size;
        return ptr;
      }
    }

    /* Need a new chunk */
    add_chunk();
    void *ptr{ current_chunk_->try_alloc(size, alignment) };
    if(ptr)
    {
      stats_.total_used += size;
    }
    return ptr;
  }

  void arena::reset()
  {
    /* Reset all chunks (don't free, reuse them) */
    chunk *c{ first_chunk_ };
    while(c)
    {
      c->reset();
      c = c->next;
    }
    current_chunk_ = first_chunk_;

    /* Free large allocations (we allocated with malloc, so we free with free) */
    large_alloc *la{ large_allocs_ };
    while(la)
    {
      large_alloc *next{ la->next };
      if(la->ptr)
      {
        /* Unregister from GC before freeing */
        GC_remove_roots(la->ptr, static_cast<char *>(la->ptr) + la->size);
        std::free(la->ptr);
      }
      delete la;
      la = next;
    }
    large_allocs_ = nullptr;

    /* Reset stats for used memory, keep allocated */
    stats_.total_used = 0;
    stats_.large_alloc_count = 0;
  }

  allocator::stats arena::get_stats() const
  {
    return { stats_.total_allocated, stats_.total_used };
  }

  arena::arena_stats arena::get_arena_stats() const
  {
    return stats_;
  }

  /* ----- allocator_scope implementation ----- */

  allocator_scope::allocator_scope(allocator *a)
    : previous_{ current_allocator }
  {
    current_allocator = a;
  }

  allocator_scope::~allocator_scope()
  {
    current_allocator = previous_;
  }

  /* ----- try_allocator_alloc implementation ----- */
  void *try_allocator_alloc(usize size, usize alignment)
  {
    if(current_allocator) [[unlikely]]
    {
      return current_allocator->alloc(size, alignment);
    }
    return nullptr;
  }
}
