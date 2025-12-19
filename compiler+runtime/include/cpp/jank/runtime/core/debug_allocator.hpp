#pragma once

#include <unordered_map>
#include <mutex>

#include <jank/runtime/core/arena.hpp>

namespace jank::runtime
{
  /* Debug allocator for detecting memory issues.
   *
   * Implements the allocator interface and provides:
   * - Double-free detection
   * - Memory leak detection
   * - Allocation statistics
   *
   * Unlike arena, this allocator tracks individual allocations and
   * can detect when memory is freed twice or not freed at all.
   *
   * Usage from jank:
   *   (require '[jank.debug-allocator-native :as dbg])
   *   (def d (dbg/create))
   *   (dbg/enter! d)
   *   ; ... allocations happen here ...
   *   (dbg/exit!)
   *   (dbg/has-leaks? d)     ; Check if there are leaks
   *   (dbg/detect-leaks d)   ; Get leak details
   *   (dbg/stats d)          ; Get allocation stats
   */
  struct debug_allocator : allocator
  {
    /* Allocation tracking info */
    struct alloc_info
    {
      void *ptr{};
      usize size{};
      usize alignment{};
      bool is_freed{};
    };

    /* Extended stats for debug allocator */
    struct debug_stats
    {
      usize total_allocated{};     /* Total bytes allocated */
      usize total_freed{};         /* Total bytes freed */
      usize current_live{};        /* Currently live bytes */
      usize allocation_count{};    /* Number of allocations */
      usize free_count{};          /* Number of frees */
      usize double_free_count{};   /* Number of double-free attempts detected */
      usize leak_count{};          /* Number of leaks (updated on detect_leaks) */
    };

    debug_allocator() = default;
    ~debug_allocator();

    /* Non-copyable, non-movable */
    debug_allocator(debug_allocator const &) = delete;
    debug_allocator(debug_allocator &&) = delete;
    debug_allocator &operator=(debug_allocator const &) = delete;
    debug_allocator &operator=(debug_allocator &&) = delete;

    /* Allocate memory with tracking (implements allocator interface) */
    void *alloc(usize size, usize alignment = 16) override;

    /* Free memory with double-free detection (implements allocator interface) */
    void free(void *ptr, usize size, usize alignment) override;

    /* Reset allocator state (implements allocator interface) */
    void reset() override;

    /* Get basic stats (implements allocator interface) */
    allocator::stats get_stats() const override;

    /* Get extended debug stats */
    debug_stats get_debug_stats() const;

    /* Check if there are any leaks (unfreed allocations) */
    bool has_leaks() const;

    /* Detect leaks and return count. Updates leak_count in stats. */
    usize detect_leaks();

    /* Get details about leaked allocations */
    native_vector<alloc_info> get_leaked_allocations() const;

    /* Get details about double-free attempts */
    usize get_double_free_count() const;

  private:
    /* Map from pointer to allocation info */
    std::unordered_map<void *, alloc_info> allocations_;
    mutable std::mutex mutex_;
    debug_stats stats_{};
  };

  /* Scoped debug allocator usage */
  struct debug_allocator_scope
  {
    explicit debug_allocator_scope(debug_allocator *d);
    ~debug_allocator_scope();

    /* Non-copyable, non-movable */
    debug_allocator_scope(debug_allocator_scope const &) = delete;
    debug_allocator_scope(debug_allocator_scope &&) = delete;
    debug_allocator_scope &operator=(debug_allocator_scope const &) = delete;
    debug_allocator_scope &operator=(debug_allocator_scope &&) = delete;

  private:
    allocator *previous_;
  };
}
