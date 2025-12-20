#include <cstdlib>

#include <jank/runtime/core/debug_allocator.hpp>
#include <jank/util/fmt/print.hpp>

namespace jank::runtime
{
  debug_allocator::~debug_allocator()
  {
    /* Check for leaks on destruction */
    auto const leaks = detect_leaks();
    if(leaks > 0)
    {
      util::println("debug_allocator: {} leaked allocations detected on destruction", leaks);
    }

    /* Free any remaining allocations */
    for(auto &[ptr, info] : allocations_)
    {
      if(!info.is_freed && info.ptr)
      {
        std::free(info.ptr);
      }
    }
  }

  void *debug_allocator::alloc(usize const size, usize const alignment)
  {
    std::lock_guard<std::mutex> lock(mutex_);

    /* Allocate with alignment */
    void *ptr{ nullptr };
    if(posix_memalign(&ptr, alignment, size) != 0)
    {
      return nullptr;
    }

    /* Track the allocation */
    alloc_info info;
    info.ptr = ptr;
    info.size = size;
    info.alignment = alignment;
    info.is_freed = false;

    allocations_[ptr] = info;

    /* Update stats */
    stats_.total_allocated += size;
    stats_.current_live += size;
    ++stats_.allocation_count;

    return ptr;
  }

  void debug_allocator::free(void *ptr, usize /*size*/, usize /*alignment*/)
  {
    if(!ptr)
    {
      return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = allocations_.find(ptr);
    if(it == allocations_.end())
    {
      /* Unknown pointer - might be from a different allocator */
      util::println("debug_allocator: WARNING - freeing unknown pointer {}",
                    reinterpret_cast<uintptr_t>(ptr));
      return;
    }

    if(it->second.is_freed)
    {
      /* Double-free detected! */
      ++stats_.double_free_count;
      util::println("debug_allocator: DOUBLE-FREE detected for pointer {} (size {})",
                    reinterpret_cast<uintptr_t>(ptr),
                    it->second.size);
      return;
    }

    /* Mark as freed and update stats */
    it->second.is_freed = true;
    stats_.total_freed += it->second.size;
    stats_.current_live -= it->second.size;
    ++stats_.free_count;

    /* Actually free the memory */
    std::free(ptr);
  }

  void debug_allocator::reset()
  {
    std::lock_guard<std::mutex> lock(mutex_);

    /* Free all non-freed allocations */
    for(auto &[ptr, info] : allocations_)
    {
      if(!info.is_freed && info.ptr)
      {
        std::free(info.ptr);
      }
    }

    allocations_.clear();
    stats_ = {};
  }

  allocator::stats debug_allocator::get_stats() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return { stats_.total_allocated, stats_.current_live };
  }

  debug_allocator::debug_stats debug_allocator::get_debug_stats() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
  }

  bool debug_allocator::has_leaks() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for(auto const &[ptr, info] : allocations_)
    {
      if(!info.is_freed)
      {
        return true;
      }
    }
    return false;
  }

  usize debug_allocator::detect_leaks()
  {
    std::lock_guard<std::mutex> lock(mutex_);

    usize count{ 0 };
    for(auto const &[ptr, info] : allocations_)
    {
      if(!info.is_freed)
      {
        ++count;
      }
    }
    stats_.leak_count = count;
    return count;
  }

  native_vector<debug_allocator::alloc_info> debug_allocator::get_leaked_allocations() const
  {
    std::lock_guard<std::mutex> lock(mutex_);

    native_vector<alloc_info> leaks;
    for(auto const &[ptr, info] : allocations_)
    {
      if(!info.is_freed)
      {
        leaks.push_back(info);
      }
    }
    return leaks;
  }

  usize debug_allocator::get_double_free_count() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_.double_free_count;
  }

  /* ----- debug_allocator_scope implementation ----- */

  debug_allocator_scope::debug_allocator_scope(debug_allocator *d)
    : previous_{ current_allocator }
  {
    current_allocator = d;
  }

  debug_allocator_scope::~debug_allocator_scope()
  {
    current_allocator = previous_;
  }
}
