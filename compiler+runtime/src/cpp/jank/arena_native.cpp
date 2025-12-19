#include <jank/arena_native.hpp>
#include <jank/runtime/context.hpp>
#include <jank/runtime/convert/function.hpp>
#include <jank/runtime/core/make_box.hpp>
#include <jank/runtime/core/math.hpp>
#include <jank/runtime/core/arena.hpp>
#include <jank/runtime/obj/arena.hpp>
#include <jank/runtime/obj/native_function_wrapper.hpp>
#include <jank/runtime/obj/native_pointer_wrapper.hpp>
#include <jank/runtime/obj/persistent_hash_map.hpp>
#include <jank/runtime/obj/keyword.hpp>

namespace
{
  using namespace jank;
  using namespace jank::runtime;

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
    throw std::runtime_error("Expected an allocator (arena or debug-allocator)");
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

  /* Enter arena scope - sets thread-local arena */
  object_ref enter_arena(object_ref const arena)
  {
    auto const a{ try_object<obj::arena_obj>(arena) };
    current_arena = &a->cpp_arena;
    return jank_nil;
  }

  /* Exit arena scope - restores previous arena */
  object_ref exit_arena()
  {
    current_arena = nullptr;
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
  intern_fn("allocator-reset!", &allocator_reset);
  intern_fn("allocator-stats", &allocator_stats);
  intern_fn("allocator?", &is_allocator);
  intern_fn("enter!", &enter_allocator);
  intern_fn("exit!", &exit_allocator);

  return jank_nil.erase();
}
