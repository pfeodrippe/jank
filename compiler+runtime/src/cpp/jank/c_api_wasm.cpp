// WASM-specific C API implementation
// This file provides the same C API as c_api.cpp but without LLVM dependencies

#include <cstdarg>
#include <array>
#include <stdexcept>
#include <locale>

#include <jank/c_api.h>
#include <jank/runtime/visit.hpp>
#include <jank/runtime/context.hpp>
#include <jank/runtime/core.hpp>
#include <jank/runtime/core/meta.hpp>
#include <jank/runtime/obj/native_pointer_wrapper.hpp>
#include <jank/runtime/obj/native_function_wrapper.hpp>
#include <jank/error/runtime.hpp>
#include <jank/profile/time.hpp>
#include <jank/util/scope_exit.hpp>
#include <jank/util/fmt/print.hpp>

using namespace jank;
using namespace jank::runtime;

template <typename Is>
struct make_function_arity;

template <usize I>
struct make_function_arity_arg
{
  using type = object *;
};

template <size_t... Is>
struct make_function_arity<std::index_sequence<Is...>>
{
  using type = object *(*)(object *, typename make_function_arity_arg<Is>::type...);
};

template <>
struct make_function_arity<std::index_sequence<>>
{
  using type = object *(*)(object *);
};

template <usize N>
using function_arity = typename make_function_arity<std::make_index_sequence<N>>::type;

extern "C"
{
  jank_object_ref jank_eval(jank_object_ref const s)
  {
    auto const s_obj(try_object<obj::persistent_string>(reinterpret_cast<object *>(s)));
    return __rt_ctx->eval_string(s_obj->data).erase();
  }

  jank_object_ref jank_read_string(jank_object_ref const s)
  {
    auto const s_obj(try_object<obj::persistent_string>(reinterpret_cast<object *>(s)));
    return __rt_ctx->read_string(s_obj->data).erase();
  }

  jank_object_ref jank_read_string_c(char const * const s)
  {
    return __rt_ctx->read_string(s).erase();
  }

  /* Evaluate a string and return the result as a code string.
   * This is useful for WASM where we want to see the result printed. */
  char const *jank_eval_string_c(char const * const s)
  {
    auto const result = __rt_ctx->eval_string(s);
    static thread_local jtl::immutable_string result_str;
    result_str = runtime::to_code_string(result);
    return result_str.c_str();
  }

  void jank_ns_set_symbol_counter(char const * const ns, jank_u64 const count)
  {
    auto const ns_obj(__rt_ctx->intern_ns(ns));
    ns_obj->symbol_counter.store(count);
  }

  jank_object_ref jank_var_intern(jank_object_ref const ns, jank_object_ref const name)
  {
    auto const ns_obj(try_object<obj::persistent_string>(reinterpret_cast<object *>(ns)));
    __rt_ctx->intern_ns(ns_obj->data);

    auto const name_obj(try_object<obj::persistent_string>(reinterpret_cast<object *>(name)));
    return __rt_ctx->intern_var(ns_obj->data, name_obj->data).expect_ok().erase();
  }

  jank_object_ref jank_var_intern_c(char const * const ns, char const * const name)
  {
    __rt_ctx->intern_ns(ns);
    return __rt_ctx->intern_var(ns, name).expect_ok().erase();
  }

  jank_object_ref jank_var_bind_root(jank_object_ref const var, jank_object_ref const val)
  {
    auto const var_obj(try_object<runtime::var>(reinterpret_cast<object *>(var)));
    auto const val_obj(reinterpret_cast<object *>(val));
    return var_obj->bind_root(val_obj).erase();
  }

  jank_object_ref jank_keyword_intern(jank_object_ref const ns, jank_object_ref const name)
  {
    if(!ns)
    {
      auto const name_obj(try_object<obj::persistent_string>(reinterpret_cast<object *>(name)));
      return __rt_ctx->intern_keyword(name_obj->data).expect_ok().erase();
    }
    auto const ns_obj(try_object<obj::persistent_string>(reinterpret_cast<object *>(ns)));
    auto const name_obj(try_object<obj::persistent_string>(reinterpret_cast<object *>(name)));
    return __rt_ctx->intern_keyword(ns_obj->data, name_obj->data).expect_ok().erase();
  }

  jank_object_ref jank_deref(jank_object_ref const o)
  {
    return runtime::deref(reinterpret_cast<object *>(o)).erase();
  }

  jank_object_ref jank_call0(jank_object_ref const f)
  {
    return runtime::dynamic_call(reinterpret_cast<object *>(f)).erase();
  }

  jank_object_ref jank_call1(jank_object_ref const f, jank_object_ref const a1)
  {
    return runtime::dynamic_call(reinterpret_cast<object *>(f), reinterpret_cast<object *>(a1))
      .erase();
  }

  jank_object_ref
  jank_call2(jank_object_ref const f, jank_object_ref const a1, jank_object_ref const a2)
  {
    return runtime::dynamic_call(reinterpret_cast<object *>(f),
                                 reinterpret_cast<object *>(a1),
                                 reinterpret_cast<object *>(a2))
      .erase();
  }

  jank_object_ref jank_call3(jank_object_ref const f,
                             jank_object_ref const a1,
                             jank_object_ref const a2,
                             jank_object_ref const a3)
  {
    return runtime::dynamic_call(reinterpret_cast<object *>(f),
                                 reinterpret_cast<object *>(a1),
                                 reinterpret_cast<object *>(a2),
                                 reinterpret_cast<object *>(a3))
      .erase();
  }

  jank_object_ref jank_call4(jank_object_ref const f,
                             jank_object_ref const a1,
                             jank_object_ref const a2,
                             jank_object_ref const a3,
                             jank_object_ref const a4)
  {
    return runtime::dynamic_call(reinterpret_cast<object *>(f),
                                 reinterpret_cast<object *>(a1),
                                 reinterpret_cast<object *>(a2),
                                 reinterpret_cast<object *>(a3),
                                 reinterpret_cast<object *>(a4))
      .erase();
  }

  jank_object_ref jank_call5(jank_object_ref const f,
                             jank_object_ref const a1,
                             jank_object_ref const a2,
                             jank_object_ref const a3,
                             jank_object_ref const a4,
                             jank_object_ref const a5)
  {
    return runtime::dynamic_call(reinterpret_cast<object *>(f),
                                 reinterpret_cast<object *>(a1),
                                 reinterpret_cast<object *>(a2),
                                 reinterpret_cast<object *>(a3),
                                 reinterpret_cast<object *>(a4),
                                 reinterpret_cast<object *>(a5))
      .erase();
  }

  jank_object_ref jank_call6(jank_object_ref const f,
                             jank_object_ref const a1,
                             jank_object_ref const a2,
                             jank_object_ref const a3,
                             jank_object_ref const a4,
                             jank_object_ref const a5,
                             jank_object_ref const a6)
  {
    return runtime::dynamic_call(reinterpret_cast<object *>(f),
                                 reinterpret_cast<object *>(a1),
                                 reinterpret_cast<object *>(a2),
                                 reinterpret_cast<object *>(a3),
                                 reinterpret_cast<object *>(a4),
                                 reinterpret_cast<object *>(a5),
                                 reinterpret_cast<object *>(a6))
      .erase();
  }

  jank_object_ref jank_integer_create(jank_i64 const i)
  {
    return make_box(i).erase();
  }

  jank_i64 jank_to_integer(jank_object_ref const o)
  {
    return runtime::to_int(reinterpret_cast<object *>(o));
  }

  jank_object_ref jank_real_create(jank_f64 const d)
  {
    return make_box(d).erase();
  }

  jank_f64 jank_to_real(jank_object_ref const o)
  {
    return visit_number_like(
      [](auto const typed_o) -> f64 {
        using T = typename decltype(typed_o)::value_type;

        if constexpr(std::same_as<T, obj::integer>)
        {
          return static_cast<f64>(typed_o->data);
        }
        else if constexpr(std::same_as<T, obj::real>)
        {
          return typed_o->data;
        }
        else if constexpr(std::same_as<T, obj::ratio>)
        {
          return typed_o->to_real();
        }
        else if constexpr(std::same_as<T, obj::big_integer>)
        {
          // In WASM, native_big_integer is just long long, so simple cast works
          return static_cast<f64>(typed_o->data);
        }
        else
        {
          throw std::runtime_error{ util::format("invalid real: {}", typed_o->to_code_string()) };
        }
      },
      reinterpret_cast<object *>(o));
  }

  jank_object_ref jank_string_create(char const * const s)
  {
    return make_box(s).erase();
  }

  char const *jank_to_string(jank_object_ref const o)
  {
    return try_object<obj::persistent_string>(reinterpret_cast<object *>(o))->data.c_str();
  }

  jank_object_ref jank_pointer_create(void * const p)
  {
    return make_box<obj::native_pointer_wrapper>(p).erase();
  }

  void *jank_to_pointer(jank_object_ref const o)
  {
    return try_object<obj::native_pointer_wrapper>(reinterpret_cast<object *>(o))->data;
  }

  jank_object_ref jank_const_nil()
  {
    return jank_nil.erase();
  }

  jank_object_ref jank_const_true()
  {
    return jank_true.erase();
  }

  jank_object_ref jank_const_false()
  {
    return jank_false.erase();
  }

  jank_bool jank_truthy(jank_object_ref const o)
  {
    return runtime::truthy(reinterpret_cast<object *>(o));
  }

  jank_object_ref jank_native_function_wrapper_create(
    void * const callback,
    void * const context,
    jank_object_ref (*invoke)(void *, void *, jank_object_ref const *, jank_usize),
    jank_u8 const /* arg_count - not used in this simplified version */)
  {
    // Create a function_type from a std::function
    std::function<object_ref(object_ref const *, usize)> fn
      = [callback, context, invoke](object_ref const * const args, usize const size) -> object_ref {
      return reinterpret_cast<object *>(
        invoke(callback, context, reinterpret_cast<jank_object_ref const *>(args), size));
    };
    return make_box<obj::native_function_wrapper>(obj::detail::function_type{ std::move(fn) })
      .erase();
  }

  void *jank_native_function_wrapper_get_pointer(jank_object_ref const /* o */)
  {
    // Not fully supported in WASM - return nullptr
    return nullptr;
  }

  void jank_throw(jank_object_ref const o)
  {
    throw runtime::object_ref{ reinterpret_cast<object *>(o) };
  }

  void jank_profile_enter(char const * const label)
  {
    profile::enter(label);
  }

  void jank_profile_exit(char const * const label)
  {
    profile::exit(label);
  }

  void jank_profile_report(char const * const label)
  {
    profile::report(label);
  }

  void jank_module_set_loaded(char const * const module)
  {
    runtime::__rt_ctx->module_loader.set_is_loaded(module);
  }

  int jank_init(int const argc,
                char const ** const argv,
                jank_bool const init_default_ctx,
                int (*fn)(int const, char const ** const))
  {
    return jank_init_with_pch(argc, argv, init_default_ctx, nullptr, 0, fn);
  }

  int jank_init_with_pch(int const argc,
                         char const ** const argv,
                         jank_bool const init_default_ctx,
                         char const * const /* pch_data - not used in WASM */,
                         jank_usize const /* pch_size - not used in WASM */,
                         int (*fn)(int const, char const ** const))
  {
    try
    {
      /* The GC needs to enabled even before arg parsing, since our native types,
       * like strings, use the GC for allocations. It can still be configured later. */
      GC_set_all_interior_pointers(1);
      GC_enable();
      GC_init();

      // Note: No LLVM initialization in WASM build - JIT not available

      if(init_default_ctx)
      {
        runtime::__rt_ctx = new(GC) runtime::context{};
      }

      return fn(argc, argv);
    }
    catch(std::exception const &e)
    {
      util::println("Error: {}", e.what());
      return 1;
    }
    catch(...)
    {
      util::println("Unknown error");
      return 1;
    }
  }
}
