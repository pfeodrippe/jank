#pragma once

#include <jtl/option.hpp>

#include <jank/type.hpp>
#include <jank/runtime/object.hpp>
#include <jank/runtime/behavior/callable.hpp>

namespace jank::runtime::obj
{
  using user_type_ref = oref<struct user_type>;

  /* Behavior flags for user-defined types. */
  enum class behavior_flag : u32
  {
    none = 0,
    seqable = 1 << 0,
    sequenceable = 1 << 1,
    countable = 1 << 2,
    conjable = 1 << 3,
    assoc_readable = 1 << 4,
    assoc_writable = 1 << 5,
    map_like = 1 << 6,
    callable = 1 << 7,
    metadatable = 1 << 8,
  };

  inline behavior_flag operator|(behavior_flag a, behavior_flag b)
  {
    return static_cast<behavior_flag>(static_cast<u32>(a) | static_cast<u32>(b));
  }

  inline behavior_flag operator&(behavior_flag a, behavior_flag b)
  {
    return static_cast<behavior_flag>(static_cast<u32>(a) & static_cast<u32>(b));
  }

  inline behavior_flag &operator|=(behavior_flag &a, behavior_flag b)
  {
    a = a | b;
    return a;
  }

  inline bool has_flag(behavior_flag flags, behavior_flag flag)
  {
    return (static_cast<u32>(flags) & static_cast<u32>(flag)) != 0;
  }

  /* vtable for user-defined types.
   * Each function pointer is nullable - nullptr means the behavior is not implemented.
   * The stored jank functions are called via dynamic_call when the C++ vtable functions are invoked.
   */
  struct user_type_vtable : gc
  {
    jtl::immutable_string type_name;

    /* Stored jank function implementations. */
    object_ref get_fn{};
    object_ref get_entry_fn{};
    object_ref contains_fn{};
    object_ref assoc_fn{};
    object_ref dissoc_fn{};
    object_ref seq_fn{};
    object_ref count_fn{};
    object_ref conj_fn{};
    object_ref call_fn{};
    object_ref with_meta_fn{};
    object_ref meta_fn{};
    object_ref to_string_fn{};
    object_ref to_hash_fn{};
    object_ref equal_fn{};

    /* Behavior flags. */
    behavior_flag flags{ behavior_flag::none };
  };

  /* A user-defined type instance.
   * Uses a vtable for dynamic dispatch to user-provided jank functions.
   */
  struct user_type
    : gc
    , behavior::callable
  {
    static constexpr object_type obj_type{ object_type::user_type };
    static constexpr bool pointer_free{ false };

    user_type() = default;
    user_type(user_type_vtable const *vt, object_ref d);
    user_type(user_type_vtable const *vt, object_ref d, object_ref m);

    /* behavior::object_like */
    bool equal(object const &other) const;
    jtl::immutable_string to_string() const;
    void to_string(jtl::string_builder &buff) const;
    jtl::immutable_string to_code_string() const;
    uhash to_hash() const;

    /* behavior::metadatable */
    user_type_ref with_meta(object_ref m) const;

    /* behavior::associatively_readable */
    object_ref get(object_ref key) const;
    object_ref get(object_ref key, object_ref fallback) const;
    object_ref get_entry(object_ref key) const;
    bool contains(object_ref key) const;

    /* behavior::associatively_writable */
    object_ref assoc(object_ref key, object_ref val) const;
    object_ref dissoc(object_ref key) const;

    /* Note: seq() and fresh_seq() are not implemented because user_type's seq()
     * would return object_ref (type-erased), which doesn't satisfy the C++ seqable
     * concept's assumptions. Users can implement seqable behavior in jank via :seq-fn. */

    /* behavior::countable */
    usize count() const;

    /* behavior::conjable */
    object_ref conj(object_ref val) const;

    /* behavior::callable */
    object_ref call() override;
    object_ref call(object_ref) override;
    object_ref call(object_ref, object_ref) override;
    object_ref call(object_ref, object_ref, object_ref) override;
    object_ref call(object_ref, object_ref, object_ref, object_ref) override;
    object_ref call(object_ref, object_ref, object_ref, object_ref, object_ref) override;
    object_ref
      call(object_ref, object_ref, object_ref, object_ref, object_ref, object_ref) override;
    object_ref
      call(object_ref, object_ref, object_ref, object_ref, object_ref, object_ref, object_ref)
        override;
    object_ref call(object_ref,
                    object_ref,
                    object_ref,
                    object_ref,
                    object_ref,
                    object_ref,
                    object_ref,
                    object_ref) override;
    object_ref call(object_ref,
                    object_ref,
                    object_ref,
                    object_ref,
                    object_ref,
                    object_ref,
                    object_ref,
                    object_ref,
                    object_ref) override;
    object_ref call(object_ref,
                    object_ref,
                    object_ref,
                    object_ref,
                    object_ref,
                    object_ref,
                    object_ref,
                    object_ref,
                    object_ref,
                    object_ref) override;
    object_ref this_object_ref() override;
    arity_flag_t get_arity_flags() const override;

    object base{ obj_type };
    user_type_vtable const *vtable{};
    object_ref data;
    jtl::option<object_ref> meta;
  };

  /* Factory function that returns a factory for creating user_type instances.
   * Usage: (def MyType (make-user-type "MyType" (fn [data] {:get ...})))
   *        (def instance (MyType {:field 1})) */
  object_ref make_user_type(object_ref type_name, object_ref constructor_fn);
}
