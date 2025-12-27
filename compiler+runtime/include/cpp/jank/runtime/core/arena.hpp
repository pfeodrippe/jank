#pragma once

#include <jank/runtime/object.hpp>
#include <jank/runtime/core/allocator_fwd.hpp>

namespace jank::runtime
{
  /* Scoped allocator usage - sets thread-local current allocator */
  struct allocator_scope
  {
    explicit allocator_scope(allocator *a);
    ~allocator_scope();

    /* Non-copyable, non-movable */
    allocator_scope(allocator_scope const &) = delete;
    allocator_scope(allocator_scope &&) = delete;
    allocator_scope &operator=(allocator_scope const &) = delete;
    allocator_scope &operator=(allocator_scope &&) = delete;

  private:
    allocator *previous_;
  };

  /* Check if an allocator is active and allocate from it if so.
   * Returns nullptr if no allocator is active (use GC instead).
   * This is the ONLY allocation entry point - all allocators use this interface.
   * Defined in arena.cpp (not inline) for JIT compatibility. */
  [[gnu::hot]]
  void *try_allocator_alloc(usize size, usize alignment = 16);

  /* ----- Arena implementation ----- */

  /* Arena allocator for user-controlled temporary allocations.
   *
   * Provides fast bump-pointer allocation that BYPASSES the GC.
   * Memory is allocated directly from the system (malloc), giving users
   * complete control over allocation lifetime within a scope.
   *
   * Usage patterns:
   * 1. with-arena scope: All allocations within scope use the arena
   * 2. Explicit reset: Call reset() to reuse arena memory
   * 3. Automatic cleanup: Arena destructor frees all memory
   *
   * IMPORTANT: Arena memory is NOT scanned by the GC. Only use arenas for:
   * - Pointer-free types (integers, reals, characters)
   * - Short-lived allocations that don't escape the scope
   * - Performance-critical code where GC overhead is unacceptable
   *
   * Objects allocated in the arena should NOT be stored in GC-managed
   * data structures, as the GC cannot track them.
   */
  struct arena : allocator
  {
    static constexpr usize default_chunk_size{ 64 * 1024 }; /* 64KB chunks */
    static constexpr usize max_small_alloc{ 4096 }; /* Max size for bump allocation */

    arena();
    explicit arena(usize chunk_size);
    ~arena();

    /* Non-copyable, non-movable */
    arena(arena const &) = delete;
    arena(arena &&) = delete;
    arena &operator=(arena const &) = delete;
    arena &operator=(arena &&) = delete;

    /* Allocate memory from the arena (implements allocator interface).
     * For small allocations: bump pointer (very fast)
     * For large allocations: direct system allocation (tracked separately)
     */
    [[gnu::hot, gnu::malloc, gnu::assume_aligned(16)]]
    void *alloc(usize size, usize alignment = 16) override;

    /* Allocate and construct an object in the arena. */
    template <typename T, typename... Args>
    [[gnu::hot]]
    T *alloc_construct(Args &&...args)
    {
      void *mem{ alloc(sizeof(T), alignof(T)) };
      return new(mem) T{ std::forward<Args>(args)... };
    }

    /* Reset the arena, invalidating all allocations.
     * Keeps the memory allocated for reuse (implements allocator interface). */
    void reset() override;

    /* Get statistics about the arena (implements allocator interface). */
    allocator::stats get_stats() const override;

    /* Extended arena-specific stats */
    struct arena_stats
    {
      usize total_allocated{}; /* Total bytes allocated from system */
      usize total_used{}; /* Total bytes given to users */
      usize chunk_count{}; /* Number of chunks */
      usize large_alloc_count{}; /* Number of large allocations */
    };

    arena_stats get_arena_stats() const;

  private:
    struct chunk
    {
      chunk *next{};
      char *start{};
      char *current{};
      char *end{};

      chunk(usize size);
      ~chunk();

      [[gnu::hot]]
      void *try_alloc(usize size, usize alignment);
      void reset();
    };

    /* Large allocations tracked separately */
    struct large_alloc
    {
      large_alloc *next{};
      void *ptr{};
      usize size{};
    };

    void add_chunk();

    usize chunk_size_;
    chunk *current_chunk_{};
    chunk *first_chunk_{};
    large_alloc *large_allocs_{};
    arena_stats stats_{};
  };

  /* Scoped arena usage - convenience wrapper that uses allocator_scope.
   * Since arena inherits from allocator, this just sets current_allocator. */
  struct arena_scope
  {
    explicit arena_scope(arena *a)
      : scope_{ a }
    {
    }

  private:
    allocator_scope scope_;
  };
}
