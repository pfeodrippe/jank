#pragma once

#include <jank/runtime/object.hpp>
#include <iostream>

namespace jank::runtime
{
  template <typename T>
  requires behavior::object_like<T>
  [[gnu::always_inline, gnu::flatten, gnu::hot]]
  constexpr bool isa(object const * const o)
  {
    jank_debug_assert(o);
    return o->type == T::obj_type;
  }

  template <typename T>
  requires behavior::object_like<T>
  [[gnu::always_inline, gnu::flatten, gnu::hot]]
  constexpr oref<T> dyn_cast(object_ref const o)
  {
    if(o->type != T::obj_type)
    {
      return {};
    }
    return reinterpret_cast<T *>(reinterpret_cast<char *>(o.data) - offsetof(T, base));
  }

  template <typename T>
  requires behavior::object_like<T>
  [[gnu::always_inline, gnu::flatten, gnu::hot]]
  oref<T> try_object(object_ref const o)
  {
    if(o->type != T::obj_type)
    {
      jtl::string_builder sb;
      sb("invalid object type (expected ");
      sb(object_type_str(T::obj_type));
      sb(" found ");
      sb(object_type_str(o->type));
      sb(")");
      throw std::runtime_error{ sb.str() };
    }
    return reinterpret_cast<T *>(reinterpret_cast<char *>(o.data) - offsetof(T, base));
  }

  /* This is dangerous. You probably don't want it. Just use `try_object` or `visit_object`.
   * However, if you're absolutely certain you know the type of an erased object, I guess
   * you can use this. */
  template <typename T>
  requires behavior::object_like<T>
  [[gnu::always_inline, gnu::flatten, gnu::hot]]
  constexpr oref<T> expect_object(object_ref const o)
  {
    if constexpr(T::obj_type != object_type::nil)
    {
      if(!o.is_some())
      {
        std::cerr << "[EXPECT_OBJECT] null ref for type "
                  << object_type_str(T::obj_type) << "\n";
      }
      jank_debug_assert(o.is_some());
    }
    if(o->type != T::obj_type)
    {
      std::cerr << "[EXPECT_OBJECT] type mismatch: got "
                << static_cast<int>(o->type) << " (" << object_type_str(o->type) << ")"
                << " expected " << static_cast<int>(T::obj_type)
                << " (" << object_type_str(T::obj_type) << ")"
                << " ptr=" << static_cast<void const *>(o.data) << "\n";
    }
    jank_debug_assert(o->type == T::obj_type);
    return reinterpret_cast<T *>(reinterpret_cast<char *>(o.data) - offsetof(T, base));
  }

  template <typename T>
  requires behavior::object_like<T>
  [[gnu::always_inline, gnu::flatten, gnu::hot]]
  constexpr oref<T> expect_object(object const * const o)
  {
    jank_debug_assert(o);
    jank_debug_assert(o->type == T::obj_type);
    return reinterpret_cast<T *>(reinterpret_cast<char *>(const_cast<object *>(o))
                                 - offsetof(T, base));
  }
}
