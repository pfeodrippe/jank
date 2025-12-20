#include <jank/arena_native.hpp>
#include <jank/runtime/context.hpp>
#include <jank/runtime/convert/function.hpp>
#include <jank/runtime/core/make_box.hpp>
#include <jank/runtime/core/math.hpp>
#include <jank/runtime/core/arena.hpp>
#include <jank/runtime/core/seq.hpp>
#include <jank/runtime/obj/arena.hpp>
#include <jank/runtime/obj/native_function_wrapper.hpp>
#include <jank/runtime/obj/native_pointer_wrapper.hpp>
#include <jank/runtime/obj/persistent_hash_map.hpp>
#include <jank/runtime/obj/keyword.hpp>

/* Forward declare dynamic_call to avoid including callable.hpp */
namespace jank::runtime
{
  object_ref dynamic_call(object_ref const source, object_ref, object_ref, object_ref);
  object_ref dynamic_call(object_ref const source, object_ref, object_ref, object_ref, object_ref);
  object_ref dynamic_call(object_ref const source, object_ref);
}

namespace
{
  using namespace jank;
  using namespace jank::runtime;

  /* ==========================================================================
   * jank_allocator: Bridge for pure-jank allocators
   *
   * This allows users to implement allocators entirely in jank code.
   * The user provides a map with :alloc-fn, :free-fn, :reset-fn, :stats-fn keys.
   * ========================================================================== */
  struct jank_allocator : allocator
  {
    /* The jank functions to call */
    object_ref alloc_fn;
    object_ref free_fn;
    object_ref reset_fn;
    object_ref stats_fn;

    /* User state passed to all functions */
    object_ref state;

    jank_allocator(object_ref alloc_fn_,
                   object_ref free_fn_,
                   object_ref reset_fn_,
                   object_ref stats_fn_,
                   object_ref state_)
      : alloc_fn{ alloc_fn_ }
      , free_fn{ free_fn_ }
      , reset_fn{ reset_fn_ }
      , stats_fn{ stats_fn_ }
      , state{ state_ }
    {
    }

    void *alloc(usize const size, usize const alignment) override
    {
      /* Call (alloc-fn state size alignment) -> returns pointer as integer */
      auto const result = dynamic_call(alloc_fn,
                                       state,
                                       make_box(static_cast<i64>(size)),
                                       make_box(static_cast<i64>(alignment)));
      return reinterpret_cast<void *>(static_cast<uintptr_t>(to_int(result)));
    }

    void free(void *ptr, usize const size, usize const alignment) override
    {
      if(free_fn.is_nil())
      {
        return;
      }
      /* Call (free-fn state ptr size alignment) */
      dynamic_call(free_fn,
                   state,
                   make_box(static_cast<i64>(reinterpret_cast<uintptr_t>(ptr))),
                   make_box(static_cast<i64>(size)),
                   make_box(static_cast<i64>(alignment)));
    }

    void reset() override
    {
      if(reset_fn.is_nil())
      {
        return;
      }
      /* Call (reset-fn state) */
      dynamic_call(reset_fn, state);
    }

    allocator::stats get_stats() const override
    {
      if(stats_fn.is_nil())
      {
        return {};
      }
      /* Call (stats-fn state) -> returns map with :total-allocated :total-used */
      auto const result = dynamic_call(stats_fn, state);

      allocator::stats s;
      /* Use generic get() that works with any map type */
      auto const total_alloc = get(result, __rt_ctx->intern_keyword("total-allocated").expect_ok());
      auto const total_used = get(result, __rt_ctx->intern_keyword("total-used").expect_ok());

      if(!total_alloc.is_nil())
      {
        s.total_allocated = static_cast<usize>(to_int(total_alloc));
      }
      if(!total_used.is_nil())
      {
        s.total_used = static_cast<usize>(to_int(total_used));
      }
      return s;
    }
  };

  /* Helper to get allocator* from any allocator object (arena_obj or native_pointer_wrapper) */
  allocator *get_allocator(object_ref const o)
  {
    if(o->type == object_type::arena)
    {
      return &try_object<obj::arena_obj>(o)->cpp_arena;
    }
    else if(o->type == object_type::native_pointer_wrapper)
    {
      return static_cast<allocator *>(try_object<obj::native_pointer_wrapper>(o)->data);
    }
    throw std::runtime_error("Expected an allocator (arena or native_pointer_wrapper)");
  }

  /* Create a new arena with optional chunk size */
  object_ref create_arena()
  {
    return make_box<obj::arena_obj>();
  }

  object_ref create_arena_with_size(object_ref const size)
  {
    return make_box<obj::arena_obj>(static_cast<usize>(to_int(size)));
  }

  /* Reset arena, reusing memory */
  object_ref reset_arena(object_ref const arena)
  {
    auto const a{ try_object<obj::arena_obj>(arena) };
    a->reset();
    return jank_nil;
  }

  /* Get arena statistics */
  object_ref arena_stats(object_ref const arena)
  {
    auto const a{ try_object<obj::arena_obj>(arena) };
    return a->get_stats();
  }

  /* Check if object is an arena */
  object_ref is_arena(object_ref const o)
  {
    if(o.is_nil())
    {
      return jank_false;
    }
    /* Check if it's our arena_obj type */
    return make_box(o->type == object_type::arena);
  }

  /* Enter arena scope - sets thread-local allocator to this arena */
  object_ref enter_arena(object_ref const arena)
  {
    auto const a{ try_object<obj::arena_obj>(arena) };
    current_allocator = &a->cpp_arena;
    return jank_nil;
  }

  /* Exit arena scope - restores previous allocator */
  object_ref exit_arena()
  {
    current_allocator = nullptr;
    return jank_nil;
  }

  /* Generic allocator functions that work with any allocator (arena or debug-allocator) */

  /* Allocate memory from any allocator, returns pointer as integer */
  object_ref allocator_alloc(object_ref const alloc_obj, object_ref const size_obj)
  {
    auto *alloc = get_allocator(alloc_obj);
    auto const size = static_cast<usize>(to_int(size_obj));
    void *ptr = alloc->alloc(size, 16);
    return make_box(static_cast<i64>(reinterpret_cast<uintptr_t>(ptr)));
  }

  /* Free memory from any allocator, takes pointer as integer */
  object_ref allocator_free(object_ref const alloc_obj, object_ref const ptr_obj)
  {
    auto *alloc = get_allocator(alloc_obj);
    auto const ptr_val = static_cast<uintptr_t>(to_int(ptr_obj));
    void *ptr = reinterpret_cast<void *>(ptr_val);
    alloc->free(ptr, 0, 16);
    return jank_nil;
  }

  /* Free a jank object through the allocator.
   * This extracts the raw pointer from the object and frees it.
   * WARNING: After calling this, the object is INVALID and must not be used! */
  object_ref allocator_free_object(object_ref const alloc_obj, object_ref const obj_to_free)
  {
    auto *alloc = get_allocator(alloc_obj);
    void *ptr = obj_to_free.data;
    alloc->free(ptr, 0, 16);
    return jank_nil;
  }

  /* Reset any allocator */
  object_ref allocator_reset(object_ref const alloc_obj)
  {
    auto *alloc = get_allocator(alloc_obj);
    alloc->reset();
    return jank_nil;
  }

  /* Get stats from any allocator */
  object_ref allocator_stats(object_ref const alloc_obj)
  {
    auto *alloc = get_allocator(alloc_obj);
    auto const s = alloc->get_stats();
    return obj::persistent_hash_map::create_unique(
      std::make_pair(__rt_ctx->intern_keyword("total-allocated").expect_ok(),
                     make_box(static_cast<i64>(s.total_allocated))),
      std::make_pair(__rt_ctx->intern_keyword("total-used").expect_ok(),
                     make_box(static_cast<i64>(s.total_used))));
  }

  /* Check if object is any allocator */
  object_ref is_allocator(object_ref const o)
  {
    if(o.is_nil())
    {
      return jank_false;
    }
    return make_box(o->type == object_type::arena || o->type == object_type::native_pointer_wrapper);
  }

  /* Create a jank-defined allocator from user functions.
   * Takes a map with keys:
   *   :alloc-fn  - (fn [state size alignment] ...) -> pointer as integer (REQUIRED)
   *   :free-fn   - (fn [state ptr size alignment] ...) (optional)
   *   :reset-fn  - (fn [state] ...) (optional)
   *   :stats-fn  - (fn [state] ...) -> {:total-allocated n :total-used m} (optional)
   *   :state     - user state passed to all functions (optional, defaults to nil)
   */
  object_ref create_jank_allocator(object_ref const config)
  {
    /* Use generic get() that works with any map type (persistent_hash_map or persistent_array_map) */
    auto const alloc_fn = get(config, __rt_ctx->intern_keyword("alloc-fn").expect_ok());
    auto const free_fn = get(config, __rt_ctx->intern_keyword("free-fn").expect_ok());
    auto const reset_fn = get(config, __rt_ctx->intern_keyword("reset-fn").expect_ok());
    auto const stats_fn = get(config, __rt_ctx->intern_keyword("stats-fn").expect_ok());
    auto const state = get(config, __rt_ctx->intern_keyword("state").expect_ok());

    if(alloc_fn.is_nil())
    {
      throw std::runtime_error("create-jank-allocator requires :alloc-fn");
    }

    auto *alloc = new jank_allocator(alloc_fn, free_fn, reset_fn, stats_fn, state);
    return make_box<obj::native_pointer_wrapper>(alloc);
  }

  /* Enter allocator scope - sets thread-local current_allocator */
  object_ref enter_allocator(object_ref const alloc_obj)
  {
    current_allocator = get_allocator(alloc_obj);
    return jank_nil;
  }

  /* Exit allocator scope */
  object_ref exit_allocator()
  {
    current_allocator = nullptr;
    return jank_nil;
  }
}

jank_object_ref jank_load_jank_arena_native()
{
  using namespace jank;
  using namespace jank::runtime;

  auto const ns(__rt_ctx->intern_ns("jank.arena-native"));

  auto const intern_fn([=](jtl::immutable_string const &name, auto const fn) {
    ns->intern_var(name)->bind_root(
      make_box<obj::native_function_wrapper>(convert_function(fn))
        ->with_meta(obj::persistent_hash_map::create_unique(std::make_pair(
          __rt_ctx->intern_keyword("name").expect_ok(),
          make_box(obj::symbol{ ns->to_string(), name }.to_string())))));
  });

  intern_fn("create", &create_arena);
  intern_fn("create-with-size", &create_arena_with_size);
  intern_fn("reset!", &reset_arena);
  intern_fn("stats", &arena_stats);
  intern_fn("arena?", &is_arena);
  intern_fn("enter-arena!", &enter_arena);
  intern_fn("exit-arena!", &exit_arena);

  /* Generic allocator functions that work with any allocator */
  intern_fn("alloc!", &allocator_alloc);
  intern_fn("free!", &allocator_free);
  intern_fn("free-object!", &allocator_free_object);
  intern_fn("allocator-reset!", &allocator_reset);
  intern_fn("allocator-stats", &allocator_stats);
  intern_fn("allocator?", &is_allocator);
  intern_fn("enter!", &enter_allocator);
  intern_fn("exit!", &exit_allocator);

  /* Create custom allocator from jank functions */
  intern_fn("create-jank-allocator", &create_jank_allocator);

  return jank_nil.erase();
}
