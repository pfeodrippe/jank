#include <jank/debug_allocator_native.hpp>
#include <jank/runtime/context.hpp>
#include <jank/runtime/convert/function.hpp>
#include <jank/runtime/core/make_box.hpp>
#include <jank/runtime/core/debug_allocator.hpp>
#include <jank/runtime/obj/native_function_wrapper.hpp>
#include <jank/runtime/obj/native_pointer_wrapper.hpp>
#include <jank/runtime/obj/persistent_hash_map.hpp>
#include <jank/runtime/obj/keyword.hpp>

namespace
{
  using namespace jank;
  using namespace jank::runtime;

  /* Helper to extract debug_allocator from native_pointer_wrapper */
  debug_allocator *get_debug_allocator(object_ref const o)
  {
    auto const wrapper{ try_object<obj::native_pointer_wrapper>(o) };
    return static_cast<debug_allocator *>(wrapper->data);
  }

  /* Create a new debug allocator */
  object_ref create_debug_allocator()
  {
    auto *dbg = new debug_allocator();
    return make_box<obj::native_pointer_wrapper>(dbg);
  }

  /* Check if object is a debug allocator - just checks if it's a native_pointer_wrapper */
  object_ref is_debug_allocator(object_ref const o)
  {
    if(o.is_nil())
    {
      return jank_false;
    }
    /* We can only check that it's a native_pointer_wrapper, not what type of pointer it is */
    return make_box(o->type == object_type::native_pointer_wrapper);
  }

  /* Debug-allocator-specific: Check if there are any leaks */
  object_ref debug_allocator_has_leaks(object_ref const dbg_obj)
  {
    auto *dbg = get_debug_allocator(dbg_obj);
    return make_box(dbg->has_leaks());
  }

  /* Debug-allocator-specific: Detect leaks and return count */
  object_ref debug_allocator_detect_leaks(object_ref const dbg_obj)
  {
    auto *dbg = get_debug_allocator(dbg_obj);
    return make_box(static_cast<i64>(dbg->detect_leaks()));
  }

  /* Debug-allocator-specific: Get double-free count */
  object_ref debug_allocator_double_free_count(object_ref const dbg_obj)
  {
    auto *dbg = get_debug_allocator(dbg_obj);
    return make_box(static_cast<i64>(dbg->get_double_free_count()));
  }

  /* Debug-allocator-specific: Extended stats with debug info */
  object_ref debug_allocator_debug_stats(object_ref const dbg_obj)
  {
    auto *dbg = get_debug_allocator(dbg_obj);
    auto const s = dbg->get_debug_stats();

    return obj::persistent_hash_map::create_unique(
      std::make_pair(__rt_ctx->intern_keyword("total-allocated").expect_ok(),
                     make_box(static_cast<i64>(s.total_allocated))),
      std::make_pair(__rt_ctx->intern_keyword("total-freed").expect_ok(),
                     make_box(static_cast<i64>(s.total_freed))),
      std::make_pair(__rt_ctx->intern_keyword("current-live").expect_ok(),
                     make_box(static_cast<i64>(s.current_live))),
      std::make_pair(__rt_ctx->intern_keyword("allocation-count").expect_ok(),
                     make_box(static_cast<i64>(s.allocation_count))),
      std::make_pair(__rt_ctx->intern_keyword("free-count").expect_ok(),
                     make_box(static_cast<i64>(s.free_count))),
      std::make_pair(__rt_ctx->intern_keyword("double-free-count").expect_ok(),
                     make_box(static_cast<i64>(s.double_free_count))),
      std::make_pair(__rt_ctx->intern_keyword("leak-count").expect_ok(),
                     make_box(static_cast<i64>(s.leak_count))));
  }
}

jank_object_ref jank_load_jank_debug_allocator_native()
{
  using namespace jank;
  using namespace jank::runtime;

  auto const ns(__rt_ctx->intern_ns("jank.debug-allocator-native"));

  auto const intern_fn([=](jtl::immutable_string const &name, auto const fn) {
    ns->intern_var(name)->bind_root(
      make_box<obj::native_function_wrapper>(convert_function(fn))
        ->with_meta(obj::persistent_hash_map::create_unique(
          std::make_pair(__rt_ctx->intern_keyword("name").expect_ok(),
                         make_box(obj::symbol{ ns->to_string(), name }.to_string())))));
  });

  /* Create function */
  intern_fn("create", &create_debug_allocator);
  intern_fn("debug-allocator?", &is_debug_allocator);

  /* Debug-allocator-specific functions (use jank.arena-native for generic alloc!/free!/reset!/stats) */
  intern_fn("has-leaks?", &debug_allocator_has_leaks);
  intern_fn("detect-leaks", &debug_allocator_detect_leaks);
  intern_fn("double-free-count", &debug_allocator_double_free_count);
  intern_fn("debug-stats", &debug_allocator_debug_stats);

  return jank_nil().erase().data;
}
