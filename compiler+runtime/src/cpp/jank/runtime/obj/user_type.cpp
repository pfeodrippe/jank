#include <jank/runtime/obj/user_type.hpp>
#include <jank/runtime/obj/nil.hpp>
#include <jank/runtime/obj/persistent_string.hpp>
#include <jank/runtime/obj/persistent_hash_map.hpp>
#include <jank/runtime/obj/persistent_vector.hpp>
#include <jank/runtime/obj/keyword.hpp>
#include <jank/runtime/obj/native_function_wrapper.hpp>
#include <jank/runtime/behavior/callable.hpp>
#include <jank/runtime/behavior/metadatable.hpp>
#include <jank/runtime/core.hpp>
#include <jank/runtime/rtti.hpp>
#include <jank/runtime/context.hpp>
#include <jank/util/fmt.hpp>

namespace jank::runtime::obj
{
  user_type::user_type(user_type_vtable const *const vt, object_ref const d)
    : vtable{ vt }
    , data{ d }
  {
  }

  user_type::user_type(user_type_vtable const *const vt, object_ref const d, object_ref const m)
    : vtable{ vt }
    , data{ d }
    , meta{ m }
  {
  }

  bool user_type::equal(object const &other) const
  {
    if(other.type != object_type::user_type)
    {
      return false;
    }

    auto const o{ expect_object<user_type>(&other) };

    /* If custom equal function is provided, use it. */
    if(!vtable->equal_fn.is_nil())
    {
      return truthy(dynamic_call(vtable->equal_fn, const_cast<user_type *>(this), o));
    }

    /* Default: compare vtables and data. */
    return vtable == o->vtable && runtime::equal(data, o->data);
  }

  void user_type::to_string(jtl::string_builder &buff) const
  {
    if(!vtable->to_string_fn.is_nil())
    {
      auto const result{ dynamic_call(vtable->to_string_fn, const_cast<user_type *>(this)) };
      buff(runtime::to_string(result));
      return;
    }

    util::format_to(buff, "#<{} {}>", vtable->type_name, &base);
  }

  jtl::immutable_string user_type::to_string() const
  {
    jtl::string_builder buff;
    to_string(buff);
    return buff.release();
  }

  jtl::immutable_string user_type::to_code_string() const
  {
    return to_string();
  }

  uhash user_type::to_hash() const
  {
    if(!vtable->to_hash_fn.is_nil())
    {
      auto const result{ dynamic_call(vtable->to_hash_fn, const_cast<user_type *>(this)) };
      return static_cast<uhash>(to_int(result));
    }

    /* Default: hash based on address. */
    return static_cast<uhash>(reinterpret_cast<uintptr_t>(&base));
  }

  user_type_ref user_type::with_meta(object_ref const m) const
  {
    if(!vtable->with_meta_fn.is_nil())
    {
      auto const result{ dynamic_call(vtable->with_meta_fn, const_cast<user_type *>(this), m) };
      return expect_object<user_type>(result);
    }

    auto const new_meta{ behavior::detail::validate_meta(m) };
    return make_box<user_type>(vtable, data, new_meta);
  }

  object_ref user_type::get(object_ref const key) const
  {
    if(!vtable->get_fn.is_nil())
    {
      return dynamic_call(vtable->get_fn, const_cast<user_type *>(this), key);
    }
    throw std::runtime_error{ "user_type '" + vtable->type_name + "' does not implement get" };
  }

  object_ref user_type::get(object_ref const key, object_ref const fallback) const
  {
    if(!vtable->get_fn.is_nil())
    {
      auto const result{ dynamic_call(vtable->get_fn, const_cast<user_type *>(this), key) };
      if(result.is_nil())
      {
        return fallback;
      }
      return result;
    }
    throw std::runtime_error{ "user_type '" + vtable->type_name + "' does not implement get" };
  }

  object_ref user_type::get_entry(object_ref const key) const
  {
    if(!vtable->get_entry_fn.is_nil())
    {
      return dynamic_call(vtable->get_entry_fn, const_cast<user_type *>(this), key);
    }

    /* Default: use get and wrap in vector. */
    if(!vtable->get_fn.is_nil())
    {
      auto const val{ get(key) };
      if(val.is_nil())
      {
        return jank_nil;
      }
      return make_box<persistent_vector>(std::in_place, key, val);
    }

    throw std::runtime_error{ "user_type '" + vtable->type_name + "' does not implement get_entry" };
  }

  bool user_type::contains(object_ref const key) const
  {
    if(!vtable->contains_fn.is_nil())
    {
      return truthy(dynamic_call(vtable->contains_fn, const_cast<user_type *>(this), key));
    }

    /* Default: check if get returns nil. */
    if(!vtable->get_fn.is_nil())
    {
      return !get(key).is_nil();
    }

    throw std::runtime_error{ "user_type '" + vtable->type_name + "' does not implement contains" };
  }

  object_ref user_type::assoc(object_ref const key, object_ref const val) const
  {
    if(!vtable->assoc_fn.is_nil())
    {
      return dynamic_call(vtable->assoc_fn, const_cast<user_type *>(this), key, val);
    }
    throw std::runtime_error{ "user_type '" + vtable->type_name + "' does not implement assoc" };
  }

  object_ref user_type::dissoc(object_ref const key) const
  {
    if(!vtable->dissoc_fn.is_nil())
    {
      return dynamic_call(vtable->dissoc_fn, const_cast<user_type *>(this), key);
    }
    throw std::runtime_error{ "user_type '" + vtable->type_name + "' does not implement dissoc" };
  }

  usize user_type::count() const
  {
    if(!vtable->count_fn.is_nil())
    {
      auto const result{ dynamic_call(vtable->count_fn, const_cast<user_type *>(this)) };
      return static_cast<usize>(to_int(result));
    }
    throw std::runtime_error{ "user_type '" + vtable->type_name + "' does not implement count" };
  }

  object_ref user_type::conj(object_ref const val) const
  {
    if(!vtable->conj_fn.is_nil())
    {
      return dynamic_call(vtable->conj_fn, const_cast<user_type *>(this), val);
    }
    throw std::runtime_error{ "user_type '" + vtable->type_name + "' does not implement conj" };
  }

  /* Callable implementation - delegates to call_fn with args as a vector. */
  object_ref user_type::call()
  {
    if(!vtable->call_fn.is_nil())
    {
      return dynamic_call(vtable->call_fn, this);
    }
    throw std::runtime_error{ "user_type '" + vtable->type_name + "' is not callable" };
  }

  object_ref user_type::call(object_ref const a1)
  {
    if(!vtable->call_fn.is_nil())
    {
      return dynamic_call(vtable->call_fn, this, a1);
    }
    throw std::runtime_error{ "user_type '" + vtable->type_name + "' is not callable" };
  }

  object_ref user_type::call(object_ref const a1, object_ref const a2)
  {
    if(!vtable->call_fn.is_nil())
    {
      return dynamic_call(vtable->call_fn, this, a1, a2);
    }
    throw std::runtime_error{ "user_type '" + vtable->type_name + "' is not callable" };
  }

  object_ref user_type::call(object_ref const a1, object_ref const a2, object_ref const a3)
  {
    if(!vtable->call_fn.is_nil())
    {
      return dynamic_call(vtable->call_fn, this, a1, a2, a3);
    }
    throw std::runtime_error{ "user_type '" + vtable->type_name + "' is not callable" };
  }

  object_ref user_type::call(object_ref const a1,
                             object_ref const a2,
                             object_ref const a3,
                             object_ref const a4)
  {
    if(!vtable->call_fn.is_nil())
    {
      return dynamic_call(vtable->call_fn, this, a1, a2, a3, a4);
    }
    throw std::runtime_error{ "user_type '" + vtable->type_name + "' is not callable" };
  }

  object_ref user_type::call(object_ref const a1,
                             object_ref const a2,
                             object_ref const a3,
                             object_ref const a4,
                             object_ref const a5)
  {
    if(!vtable->call_fn.is_nil())
    {
      return dynamic_call(vtable->call_fn, this, a1, a2, a3, a4, a5);
    }
    throw std::runtime_error{ "user_type '" + vtable->type_name + "' is not callable" };
  }

  object_ref user_type::call(object_ref const a1,
                             object_ref const a2,
                             object_ref const a3,
                             object_ref const a4,
                             object_ref const a5,
                             object_ref const a6)
  {
    if(!vtable->call_fn.is_nil())
    {
      return dynamic_call(vtable->call_fn, this, a1, a2, a3, a4, a5, a6);
    }
    throw std::runtime_error{ "user_type '" + vtable->type_name + "' is not callable" };
  }

  object_ref user_type::call(object_ref const a1,
                             object_ref const a2,
                             object_ref const a3,
                             object_ref const a4,
                             object_ref const a5,
                             object_ref const a6,
                             object_ref const a7)
  {
    if(!vtable->call_fn.is_nil())
    {
      return dynamic_call(vtable->call_fn, this, a1, a2, a3, a4, a5, a6, a7);
    }
    throw std::runtime_error{ "user_type '" + vtable->type_name + "' is not callable" };
  }

  object_ref user_type::call(object_ref const a1,
                             object_ref const a2,
                             object_ref const a3,
                             object_ref const a4,
                             object_ref const a5,
                             object_ref const a6,
                             object_ref const a7,
                             object_ref const a8)
  {
    if(!vtable->call_fn.is_nil())
    {
      return dynamic_call(vtable->call_fn, this, a1, a2, a3, a4, a5, a6, a7, a8);
    }
    throw std::runtime_error{ "user_type '" + vtable->type_name + "' is not callable" };
  }

  object_ref user_type::call(object_ref const a1,
                             object_ref const a2,
                             object_ref const a3,
                             object_ref const a4,
                             object_ref const a5,
                             object_ref const a6,
                             object_ref const a7,
                             object_ref const a8,
                             object_ref const a9)
  {
    if(!vtable->call_fn.is_nil())
    {
      return dynamic_call(vtable->call_fn, this, a1, a2, a3, a4, a5, a6, a7, a8, a9);
    }
    throw std::runtime_error{ "user_type '" + vtable->type_name + "' is not callable" };
  }

  object_ref user_type::call(object_ref const a1,
                             object_ref const a2,
                             object_ref const a3,
                             object_ref const a4,
                             object_ref const a5,
                             object_ref const a6,
                             object_ref const a7,
                             object_ref const a8,
                             object_ref const a9,
                             object_ref const a10)
  {
    if(!vtable->call_fn.is_nil())
    {
      /* dynamic_call only supports up to 10 args (source + 10), so use apply_to for 11. */
      auto const args{ make_box<persistent_vector>(std::in_place, this, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10) };
      return apply_to(vtable->call_fn, args);
    }
    throw std::runtime_error{ "user_type '" + vtable->type_name + "' is not callable" };
  }

  object_ref user_type::this_object_ref()
  {
    return this;
  }

  behavior::callable::arity_flag_t user_type::get_arity_flags() const
  {
    /* Support variadic calls if call_fn is provided. */
    if(!vtable->call_fn.is_nil())
    {
      return 0xFF; /* All arities 0-7 (max for 8-bit flag). */
    }
    return 0;
  }

  /* Helper to create a user_type instance from vtable_map.
   * Called by the factory function returned by make_user_type. */
  static user_type_ref create_user_type_instance(jtl::immutable_string const &type_name,
                                                 object_ref const vtable_map)
  {
    auto vt = make_box<user_type_vtable>();
    vt->type_name = type_name;

    /* Extract functions from the vtable map. */
    auto const get_kw{ __rt_ctx->intern_keyword("get").expect_ok() };
    auto const get_entry_kw{ __rt_ctx->intern_keyword("get-entry").expect_ok() };
    auto const contains_kw{ __rt_ctx->intern_keyword("contains").expect_ok() };
    auto const assoc_kw{ __rt_ctx->intern_keyword("assoc").expect_ok() };
    auto const dissoc_kw{ __rt_ctx->intern_keyword("dissoc").expect_ok() };
    auto const seq_kw{ __rt_ctx->intern_keyword("seq").expect_ok() };
    auto const count_kw{ __rt_ctx->intern_keyword("count").expect_ok() };
    auto const conj_kw{ __rt_ctx->intern_keyword("conj").expect_ok() };
    auto const call_kw{ __rt_ctx->intern_keyword("call").expect_ok() };
    auto const with_meta_kw{ __rt_ctx->intern_keyword("with-meta").expect_ok() };
    auto const meta_kw{ __rt_ctx->intern_keyword("meta").expect_ok() };
    auto const to_string_kw{ __rt_ctx->intern_keyword("to-string").expect_ok() };
    auto const to_hash_kw{ __rt_ctx->intern_keyword("to-hash").expect_ok() };
    auto const equal_kw{ __rt_ctx->intern_keyword("equal").expect_ok() };

    vt->get_fn = runtime::get(vtable_map, get_kw);
    vt->get_entry_fn = runtime::get(vtable_map, get_entry_kw);
    vt->contains_fn = runtime::get(vtable_map, contains_kw);
    vt->assoc_fn = runtime::get(vtable_map, assoc_kw);
    vt->dissoc_fn = runtime::get(vtable_map, dissoc_kw);
    vt->seq_fn = runtime::get(vtable_map, seq_kw);
    vt->count_fn = runtime::get(vtable_map, count_kw);
    vt->conj_fn = runtime::get(vtable_map, conj_kw);
    vt->call_fn = runtime::get(vtable_map, call_kw);
    vt->with_meta_fn = runtime::get(vtable_map, with_meta_kw);
    vt->meta_fn = runtime::get(vtable_map, meta_kw);
    vt->to_string_fn = runtime::get(vtable_map, to_string_kw);
    vt->to_hash_fn = runtime::get(vtable_map, to_hash_kw);
    vt->equal_fn = runtime::get(vtable_map, equal_kw);

    /* Set behavior flags based on which functions are provided. */
    if(!vt->seq_fn.is_nil())
    {
      vt->flags |= behavior_flag::seqable;
    }
    if(!vt->count_fn.is_nil())
    {
      vt->flags |= behavior_flag::countable;
    }
    if(!vt->conj_fn.is_nil())
    {
      vt->flags |= behavior_flag::conjable;
    }
    if(!vt->get_fn.is_nil())
    {
      vt->flags |= behavior_flag::assoc_readable;
    }
    if(!vt->assoc_fn.is_nil())
    {
      vt->flags |= behavior_flag::assoc_writable;
    }
    if(!vt->get_fn.is_nil() && !vt->assoc_fn.is_nil() && !vt->dissoc_fn.is_nil())
    {
      vt->flags |= behavior_flag::map_like;
    }
    if(!vt->call_fn.is_nil())
    {
      vt->flags |= behavior_flag::callable;
    }

    return make_box<user_type>(vt.data, jank_nil);
  }

  /* Returns a factory function that creates user_type instances.
   * Usage: (def MyType (make-user-type "MyType" (fn [data] {:get ...})))
   *        (def instance (MyType {:field 1})) */
  object_ref make_user_type(object_ref const type_name, object_ref const constructor_fn)
  {
    auto const name_str{ runtime::to_string(type_name) };

    /* Return a native function that creates instances when called with data. */
    std::function<object_ref(object_ref)> factory_fn{
      [name_str, constructor_fn](object_ref const data) -> object_ref {
        auto const vtable_map{ dynamic_call(constructor_fn, data) };
        return create_user_type_instance(name_str, vtable_map);
      }
    };
    return make_box<native_function_wrapper>(std::move(factory_fn));
  }
}
