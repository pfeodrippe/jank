#include <cstdlib>
#include <cstring>

#include <jank/runtime/core/arena.hpp>

namespace jank::runtime
{
  /* ----- chunk implementation ----- */

  arena::chunk::chunk(usize const size)
  {
    /* Allocate directly from system (bypass GC completely).
     * This gives users full control over the memory lifetime. */
    start = static_cast<char *>(std::malloc(size));
    current = start;
    end = start + size;
  }

  arena::chunk::~chunk()
  {
    /* Free the chunk memory (we own it, not the GC) */
    if(start)
    {
      std::free(start);
      start = nullptr;
      current = nullptr;
      end = nullptr;
    }
  }

  void *arena::chunk::try_alloc(usize const size, usize const alignment)
  {
    /* Align current pointer */
    auto const aligned_addr = (reinterpret_cast<uintptr_t>(current) + alignment - 1) & ~(alignment - 1);
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

  /* ----- arena_scope implementation ----- */

  arena_scope::arena_scope(arena *a)
    : previous_{ current_arena }
  {
    current_arena = a;
  }

  arena_scope::~arena_scope()
  {
    current_arena = previous_;
  }

  /* ----- try_arena_alloc implementation ----- */
  /* Non-inline version for use by oref.hpp (which forward-declares this).
   * The inline version in arena.hpp is used when arena.hpp is included directly. */
  void *try_arena_alloc(usize size, usize alignment)
  {
    if(current_arena) [[unlikely]]
    {
      return current_arena->alloc(size, alignment);
    }
    return nullptr;
  }
}
