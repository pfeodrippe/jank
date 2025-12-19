#include <jank/arena_native.hpp>
#include <jank/runtime/context.hpp>
#include <jank/runtime/convert/function.hpp>
#include <jank/runtime/core/make_box.hpp>
#include <jank/runtime/core/math.hpp>
#include <jank/runtime/core/arena.hpp>
#include <jank/runtime/obj/arena.hpp>
#include <jank/runtime/obj/native_function_wrapper.hpp>
#include <jank/runtime/obj/persistent_hash_map.hpp>
#include <jank/runtime/obj/keyword.hpp>

namespace
{
  using namespace jank;
  using namespace jank::runtime;

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

  return jank_nil.erase();
}
