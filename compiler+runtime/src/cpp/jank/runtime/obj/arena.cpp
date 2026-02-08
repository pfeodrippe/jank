#include <jank/runtime/obj/arena.hpp>
#include <jank/runtime/context.hpp>
#include <jank/runtime/core/make_box.hpp>
#include <jank/runtime/obj/persistent_hash_map.hpp>
#include <jank/runtime/obj/keyword.hpp>

namespace jank::runtime::obj
{
  arena_obj::arena_obj()
    : cpp_arena{}
  {
  }

  arena_obj::arena_obj(usize const chunk_size)
    : cpp_arena{ chunk_size }
  {
  }

  bool arena_obj::equal(object const &o) const
  {
    return &base == &o;
  }

  jtl::immutable_string arena_obj::to_string() const
  {
    jtl::string_builder buff;
    to_string(buff);
    return buff.release();
  }

  void arena_obj::to_string(jtl::string_builder &buff) const
  {
    auto const stats{ cpp_arena.get_arena_stats() };
    buff("#<arena used=");
    buff(stats.total_used);
    buff(" allocated=");
    buff(stats.total_allocated);
    buff(" chunks=");
    buff(stats.chunk_count);
    buff(">");
  }

  jtl::immutable_string arena_obj::to_code_string() const
  {
    return to_string();
  }

  uhash arena_obj::to_hash() const
  {
    return static_cast<uhash>(reinterpret_cast<uintptr_t>(this));
  }

  void arena_obj::reset()
  {
    cpp_arena.reset();
  }

  object_ref arena_obj::get_stats() const
  {
    auto const stats{ cpp_arena.get_arena_stats() };

    /* Build a map with stats */
    auto m{ persistent_hash_map::empty() };
    m = m->assoc(__rt_ctx->intern_keyword("total-allocated").expect_ok(),
                 make_box(static_cast<i64>(stats.total_allocated)));
    m = m->assoc(__rt_ctx->intern_keyword("total-used").expect_ok(),
                 make_box(static_cast<i64>(stats.total_used)));
    m = m->assoc(__rt_ctx->intern_keyword("chunk-count").expect_ok(),
                 make_box(static_cast<i64>(stats.chunk_count)));
    m = m->assoc(__rt_ctx->intern_keyword("large-alloc-count").expect_ok(),
                 make_box(static_cast<i64>(stats.large_alloc_count)));
    return m;
  }
}
