#pragma once

#include <jank/runtime/object.hpp>
#include <jank/runtime/core/arena.hpp>

namespace jank::runtime::obj
{
  /* Arena object - wraps the C++ arena allocator for use from jank code.
   *
   * Usage from jank:
   *   (require '[jank.arena :as arena])
   *   (def a (arena/create))           ; Create arena with default size
   *   (def a (arena/create 1048576))   ; Create arena with 1MB
   *   (arena/with-arena a (do-work))   ; Use arena for allocations in scope
   *   (arena/reset! a)                 ; Reset arena, reuse memory
   *   (arena/stats a)                  ; Get allocation stats
   */
  using arena_obj_ref = oref<struct arena_obj>;

  struct arena_obj : gc
  {
    static constexpr object_type obj_type{ object_type::arena };
    static constexpr bool pointer_free{ false };

    object base{ obj_type };

    /* The underlying C++ arena */
    arena cpp_arena;

    arena_obj();
    explicit arena_obj(usize chunk_size);

    /* Object interface */
    bool equal(object const &o) const;
    jtl::immutable_string to_string() const;
    void to_string(jtl::string_builder &buff) const;
    jtl::immutable_string to_code_string() const;
    uhash to_hash() const;

    /* Arena operations */
    void reset();
    object_ref get_stats() const;
  };
}
