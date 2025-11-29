#include <jank/runtime/hot_reload.hpp>

#ifdef __EMSCRIPTEN__
  #include <dlfcn.h>
  #include <emscripten.h>
#endif

#include <jank/runtime/obj/native_function_wrapper.hpp>
#include <jank/runtime/obj/number.hpp>
#include <jank/runtime/obj/keyword.hpp>
#include <jank/runtime/obj/persistent_vector.hpp>
#include <jank/runtime/obj/persistent_hash_set.hpp>
#include <jank/runtime/obj/persistent_string.hpp>
#include <jank/runtime/obj/nil.hpp>
#include <jank/runtime/context.hpp>
#include <jank/runtime/core.hpp>
#include <jank/runtime/rtti.hpp>
#include <jank/runtime/behavior/callable.hpp>
#include <jank/util/fmt.hpp>

namespace jank::runtime
{
  /* Patch symbol metadata structure.
   * SIDE_MODULE patches export this via jank_patch_symbols(). */
  struct patch_symbol
  {
    char const *qualified_name; // e.g. "user/my-func"
    char const *signature; // e.g. "1" for arity-1, "2" for arity-2, etc.
    void *fn_ptr; // Function pointer
  };

  using patch_symbols_fn = patch_symbol *(*)(int *count);

  hot_reload_registry &hot_reload_registry::instance()
  {
    static hot_reload_registry inst;
    return inst;
  }

  int hot_reload_registry::load_patch(std::string const &module_path,
                                      std::string const &symbol_name)
  {
#ifdef __EMSCRIPTEN__
    printf("[hot-reload] Loading patch: %s (symbol: %s)\n",
           module_path.c_str(),
           symbol_name.c_str());

    /* Load the WASM side module via dlopen. */
    void *handle = dlopen(module_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if(!handle)
    {
      char const *error = dlerror();
      printf("[hot-reload] dlopen failed: %s\n", error ? error : "unknown error");
      return -1;
    }

    /* Get the patch metadata function using the UNIQUE symbol name. */
    patch_symbols_fn get_symbols
      = reinterpret_cast<patch_symbols_fn>(dlsym(handle, symbol_name.c_str()));

    if(!get_symbols)
    {
      printf("[hot-reload] Warning: Symbol %s not found in %s\n",
             symbol_name.c_str(),
             module_path.c_str());
      printf("[hot-reload] This patch may need manual symbol registration.\n");
      dlclose(handle);
      return -1;
    }

    /* Get all symbols from the patch. */
    int symbol_count = 0;
    patch_symbol *symbols = get_symbols(&symbol_count);

    if(!symbols || symbol_count == 0)
    {
      printf("[hot-reload] Warning: jank_patch_symbols returned no symbols\n");
      dlclose(handle);
      return -1;
    }

    /* Register each symbol. */
    std::vector<std::string> symbol_names;
    for(int i = 0; i < symbol_count; ++i)
    {
      int result
        = register_symbol(symbols[i].qualified_name, symbols[i].fn_ptr, symbols[i].signature);
      if(result == 0)
      {
        symbol_names.push_back(symbols[i].qualified_name);
      }
    }

    /* Track the loaded module. */
    module_info info;
    info.handle = handle;
    info.path = module_path;
    info.symbols = std::move(symbol_names);
    loaded_modules_.push_back(std::move(info));

    printf("[hot-reload] Successfully loaded %d symbols from %s\n",
           symbol_count,
           module_path.c_str());
    return 0;
#else
    (void)module_path;
    (void)symbol_name;
    printf("[hot-reload] ERROR: Hot-reload is only supported in WASM builds\n");
    return -1;
#endif
  }

  int hot_reload_registry::register_symbol(std::string const &qualified_name,
                                           void *fn_ptr,
                                           std::string const &signature)
  {
    printf("[hot-reload] Registering: %s (sig: %s, ptr: %p)\n",
           qualified_name.c_str(),
           signature.c_str(),
           fn_ptr);

    /* Parse the qualified name to get namespace and symbol.
     * Format: "namespace/symbol-name" e.g. "user/my-func" */
    auto slash_pos = qualified_name.find('/');
    if(slash_pos == std::string::npos)
    {
      printf("[hot-reload] ERROR: Invalid qualified name (missing /): %s\n",
             qualified_name.c_str());
      return -1;
    }

    std::string ns_name = qualified_name.substr(0, slash_pos);
    std::string sym_name = qualified_name.substr(slash_pos + 1);

    /* Look up or create the namespace. */
    auto ns_sym = make_box<obj::symbol>(ns_name);
    auto ns = __rt_ctx->find_ns(ns_sym);
    if(ns.is_nil())
    {
      printf("[hot-reload] Creating namespace: %s\n", ns_name.c_str());
      ns = __rt_ctx->intern_ns(ns_sym);
    }

    /* Look up or create the var. */
    auto sym = make_box<obj::symbol>(sym_name);
    auto var = ns->intern_var(sym);

    /* Parse the signature to determine arity.
     * For now, we support simple numeric signatures like "1", "2", etc. */
    int arity = std::atoi(signature.c_str());

    /* Create a native_function_wrapper based on arity.
     * We need to create a std::function that matches the arity. */
    obj::native_function_wrapper_ref wrapper;

    using object_ref = runtime::object_ref;

    switch(arity)
    {
      case 0:
        {
          using fn_type = object_ref (*)();
          auto fn = reinterpret_cast<fn_type>(fn_ptr);
          wrapper = make_box<obj::native_function_wrapper>(
            obj::detail::function_type{ std::function<object_ref()>{ fn } });
          break;
        }
      case 1:
        {
          using fn_type = object_ref (*)(object_ref);
          auto fn = reinterpret_cast<fn_type>(fn_ptr);
          wrapper = make_box<obj::native_function_wrapper>(
            obj::detail::function_type{ std::function<object_ref(object_ref)>{ fn } });
          break;
        }
      case 2:
        {
          using fn_type = object_ref (*)(object_ref, object_ref);
          auto fn = reinterpret_cast<fn_type>(fn_ptr);
          wrapper = make_box<obj::native_function_wrapper>(
            obj::detail::function_type{ std::function<object_ref(object_ref, object_ref)>{ fn } });
          break;
        }
      case 3:
        {
          using fn_type = object_ref (*)(object_ref, object_ref, object_ref);
          auto fn = reinterpret_cast<fn_type>(fn_ptr);
          wrapper = make_box<obj::native_function_wrapper>(obj::detail::function_type{
            std::function<object_ref(object_ref, object_ref, object_ref)>{ fn } });
          break;
        }
      case 4:
        {
          using fn_type = object_ref (*)(object_ref, object_ref, object_ref, object_ref);
          auto fn = reinterpret_cast<fn_type>(fn_ptr);
          wrapper = make_box<obj::native_function_wrapper>(obj::detail::function_type{
            std::function<object_ref(object_ref, object_ref, object_ref, object_ref)>{ fn } });
          break;
        }
      default:
        printf("[hot-reload] ERROR: Unsupported arity: %d (max 4 currently supported)\n", arity);
        return -1;
    }

    /* Bind the wrapper to the var's root. */
    var->bind_root(wrapper);

    registered_symbols_++;
    printf("[hot-reload] Successfully registered %s with arity %d\n",
           qualified_name.c_str(),
           arity);

    return 0;
  }

  hot_reload_registry::stats hot_reload_registry::get_stats() const
  {
    stats s;
    s.loaded_modules = loaded_modules_.size();
    s.registered_symbols = registered_symbols_;
    for(auto const &mod : loaded_modules_)
    {
      s.module_paths.push_back(mod.path);
    }
    return s;
  }

  /* C API exports for WebAssembly. */
  extern "C"
  {
#ifdef __EMSCRIPTEN__
    EMSCRIPTEN_KEEPALIVE
#endif
    int jank_hot_reload_load_patch(char const *path, char const *symbol_name)
    {
      return hot_reload_registry::instance().load_patch(path, symbol_name);
    }

#ifdef __EMSCRIPTEN__
    EMSCRIPTEN_KEEPALIVE
#endif
    char const *jank_hot_reload_get_stats()
    {
      auto stats = hot_reload_registry::instance().get_stats();

      /* Build JSON string manually to avoid brace escaping issues. */
      std::string json = "{\"loaded_modules\":";
      json += std::to_string(stats.loaded_modules);
      json += ",\"registered_symbols\":";
      json += std::to_string(stats.registered_symbols);
      json += ",\"module_paths\":[";

      for(size_t i = 0; i < stats.module_paths.size(); ++i)
      {
        if(i > 0)
        {
          json += ",";
        }
        json += "\"" + stats.module_paths[i] + "\"";
      }

      json += "]}";

      /* Allocate persistent string (caller must free with free()). */
      char *result = static_cast<char *>(malloc(json.size() + 1));
      strcpy(result, json.c_str());
      return result;
    }

#ifdef __EMSCRIPTEN__
    EMSCRIPTEN_KEEPALIVE
#endif
    void *jank_box_integer(int64_t value)
    {
      auto boxed = make_box<obj::integer>(static_cast<jank::i64>(value));
      return boxed.erase();
    }

#ifdef __EMSCRIPTEN__
    EMSCRIPTEN_KEEPALIVE
#endif
    int64_t jank_unbox_integer(void *obj)
    {
      if(!obj)
      {
        return 0;
      }
      object_ref ref{ reinterpret_cast<object *>(obj) };
      auto int_obj = dyn_cast<obj::integer>(ref);
      if(int_obj.is_nil())
      {
        return 0;
      }
      return static_cast<int64_t>(int_obj->data);
    }

#ifdef __EMSCRIPTEN__
    EMSCRIPTEN_KEEPALIVE
#endif
    void *jank_add_integers(void *a, void *b)
    {
      int64_t val_a = jank_unbox_integer(a);
      int64_t val_b = jank_unbox_integer(b);
      return jank_box_integer(val_a + val_b);
    }

    /* ===== Full runtime helpers for real jank code ===== */

#ifdef __EMSCRIPTEN__
    EMSCRIPTEN_KEEPALIVE
#endif
    void *jank_call_var(char const *ns, char const *name, int argc, void **args)
    {
      /* Look up the var. */
      auto var = __rt_ctx->find_var(ns, name);
      if(var.is_nil())
      {
        printf("[hot-reload] ERROR: Var not found: %s/%s\n", ns, name);
        return jank_nil.erase();
      }

      /* Deref the var to get the function. */
      auto fn = var->deref();

      /* Convert args to object_refs. */
      switch(argc)
      {
        case 0:
          return dynamic_call(fn).erase();
        case 1:
          return dynamic_call(fn, object_ref{ reinterpret_cast<object *>(args[0]) }).erase();
        case 2:
          return dynamic_call(fn,
                              object_ref{ reinterpret_cast<object *>(args[0]) },
                              object_ref{ reinterpret_cast<object *>(args[1]) })
            .erase();
        case 3:
          return dynamic_call(fn,
                              object_ref{ reinterpret_cast<object *>(args[0]) },
                              object_ref{ reinterpret_cast<object *>(args[1]) },
                              object_ref{ reinterpret_cast<object *>(args[2]) })
            .erase();
        case 4:
          return dynamic_call(fn,
                              object_ref{ reinterpret_cast<object *>(args[0]) },
                              object_ref{ reinterpret_cast<object *>(args[1]) },
                              object_ref{ reinterpret_cast<object *>(args[2]) },
                              object_ref{ reinterpret_cast<object *>(args[3]) })
            .erase();
        case 5:
          return dynamic_call(fn,
                              object_ref{ reinterpret_cast<object *>(args[0]) },
                              object_ref{ reinterpret_cast<object *>(args[1]) },
                              object_ref{ reinterpret_cast<object *>(args[2]) },
                              object_ref{ reinterpret_cast<object *>(args[3]) },
                              object_ref{ reinterpret_cast<object *>(args[4]) })
            .erase();
        default:
          printf("[hot-reload] ERROR: jank_call_var only supports up to 5 args, got %d\n", argc);
          return jank_nil.erase();
      }
    }

#ifdef __EMSCRIPTEN__
    EMSCRIPTEN_KEEPALIVE
#endif
    void *jank_deref_var(char const *ns, char const *name)
    {
      auto var = __rt_ctx->find_var(ns, name);
      if(var.is_nil())
      {
        return jank_nil.erase();
      }
      return var->deref().erase();
    }

#ifdef __EMSCRIPTEN__
    EMSCRIPTEN_KEEPALIVE
#endif
    void *jank_make_keyword(char const *ns, char const *name)
    {
      std::string ns_str = (ns && ns[0]) ? ns : "";
      auto kw = __rt_ctx->intern_keyword(ns_str, name, true).expect_ok();
      return kw.erase();
    }

#ifdef __EMSCRIPTEN__
    EMSCRIPTEN_KEEPALIVE
#endif
    void *jank_make_vector(int argc, void **elements)
    {
      if(argc == 0)
      {
        return make_box<obj::persistent_vector>().erase();
      }

      /* Build vector by conj'ing elements one at a time. */
      auto vec = make_box<obj::persistent_vector>();
      for(int i = 0; i < argc; ++i)
      {
        vec = vec->conj(object_ref{ reinterpret_cast<object *>(elements[i]) });
      }
      return vec.erase();
    }

#ifdef __EMSCRIPTEN__
    EMSCRIPTEN_KEEPALIVE
#endif
    void *jank_make_set(int argc, void **elements)
    {
      if(argc == 0)
      {
        return make_box<obj::persistent_hash_set>().erase();
      }

      /* Build set from elements. */
      auto set = make_box<obj::persistent_hash_set>();
      for(int i = 0; i < argc; ++i)
      {
        set = set->conj(object_ref{ reinterpret_cast<object *>(elements[i]) });
      }
      return set.erase();
    }

#ifdef __EMSCRIPTEN__
    EMSCRIPTEN_KEEPALIVE
#endif
    void *jank_box_double(double value)
    {
      auto boxed = make_box<obj::real>(value);
      return boxed.erase();
    }

#ifdef __EMSCRIPTEN__
    EMSCRIPTEN_KEEPALIVE
#endif
    double jank_unbox_double(void *obj)
    {
      if(!obj)
      {
        return 0.0;
      }
      object_ref ref{ reinterpret_cast<object *>(obj) };

      /* Try real first. */
      auto real_obj = dyn_cast<obj::real>(ref);
      if(!real_obj.is_nil())
      {
        return real_obj->data;
      }

      /* Try integer and convert. */
      auto int_obj = dyn_cast<obj::integer>(ref);
      if(!int_obj.is_nil())
      {
        return static_cast<double>(int_obj->data);
      }

      return 0.0;
    }

#ifdef __EMSCRIPTEN__
    EMSCRIPTEN_KEEPALIVE
#endif
    void *jank_make_string(char const *str)
    {
      return make_box<obj::persistent_string>(str).erase();
    }

#ifdef __EMSCRIPTEN__
    EMSCRIPTEN_KEEPALIVE
#endif
    void *jank_println(int argc, void **args)
    {
      /* Call clojure.core/println. */
      return jank_call_var("clojure.core", "println", argc, args);
    }

    /* ===== Additional helpers for complex expressions ===== */

#ifdef __EMSCRIPTEN__
    EMSCRIPTEN_KEEPALIVE
#endif
    void *jank_nil_value()
    {
      return jank_nil.erase();
    }

#ifdef __EMSCRIPTEN__
    EMSCRIPTEN_KEEPALIVE
#endif
    void *jank_make_symbol(char const *ns, char const *name)
    {
      if(ns && ns[0])
      {
        return make_box<obj::symbol>(ns, name).erase();
      }
      return make_box<obj::symbol>(name).erase();
    }

    /* Create a callable wrapper from a function pointer.
     * This allows anonymous functions from patches to be passed to HOFs like mapv.
     * The fn_ptr should have signature: void* (*)(void*) for arity 1, etc. */
#ifdef __EMSCRIPTEN__
    EMSCRIPTEN_KEEPALIVE
#endif
    void *jank_make_fn_wrapper(void *fn_ptr, int arity)
    {
      using object_ref = runtime::object_ref;
      obj::native_function_wrapper_ref wrapper;

      switch(arity)
      {
        case 0:
          {
            using fn_type = object_ref (*)();
            auto fn = reinterpret_cast<fn_type>(fn_ptr);
            wrapper = make_box<obj::native_function_wrapper>(
              obj::detail::function_type{ std::function<object_ref()>{ fn } });
            break;
          }
        case 1:
          {
            using fn_type = object_ref (*)(object_ref);
            auto fn = reinterpret_cast<fn_type>(fn_ptr);
            wrapper = make_box<obj::native_function_wrapper>(
              obj::detail::function_type{ std::function<object_ref(object_ref)>{ fn } });
            break;
          }
        case 2:
          {
            using fn_type = object_ref (*)(object_ref, object_ref);
            auto fn = reinterpret_cast<fn_type>(fn_ptr);
            wrapper = make_box<obj::native_function_wrapper>(obj::detail::function_type{
              std::function<object_ref(object_ref, object_ref)>{ fn } });
            break;
          }
        case 3:
          {
            using fn_type = object_ref (*)(object_ref, object_ref, object_ref);
            auto fn = reinterpret_cast<fn_type>(fn_ptr);
            wrapper = make_box<obj::native_function_wrapper>(obj::detail::function_type{
              std::function<object_ref(object_ref, object_ref, object_ref)>{ fn } });
            break;
          }
        default:
          printf("[hot-reload] ERROR: jank_make_fn_wrapper only supports arity 0-3, got %d\n",
                 arity);
          return jank_nil.erase();
      }

      return wrapper.erase();
    }
  }
} // namespace jank::runtime
