#pragma once

// Minimal subset of the jank runtime so generated C++ can be compiled for WASM.
// This intentionally avoids pulling in the real runtime dependencies and only
// implements enough behaviour for simple println-driven programs.

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace jank::runtime
{
  struct object
  {
    virtual ~object() = default;
  };

  struct object_ref
  {
    object *ptr{ nullptr };

    object_ref() = default;
    explicit object_ref(object *p) : ptr{ p } {}

    object *operator->() const { return ptr; }
  };

  template <typename T>
  using oref = std::shared_ptr<T>;

  namespace obj
  {
    struct persistent_string : object
    {
      explicit persistent_string(std::string v) : value{ std::move(v) } {}
      std::string value;
    };
    using persistent_string_ref = oref<persistent_string>;

    struct jit_function : object
    {
      explicit jit_function(std::string name) : debug_name{ std::move(name) } {}
      virtual object_ref call() { return {}; }
      virtual object_ref call(persistent_string_ref) { return {}; }
      std::string debug_name;
    };
    using jit_function_ref = oref<jit_function>;
  }

  template <typename T, typename... Args>
  oref<T> make_box(Args &&...args)
  {
    return std::make_shared<T>(std::forward<Args>(args)...);
  }

  struct var : object
  {
    explicit var(obj::jit_function_ref fn) : fn{ std::move(fn) } {}

    obj::jit_function_ref deref() const { return fn; }

    obj::jit_function_ref fn;
  };
  using var_ref = oref<var>;

  namespace detail
  {
    struct println_function : obj::jit_function
    {
      println_function()
        : obj::jit_function{ "{:name \"println\"}" }
      {
      }

      object_ref call(obj::persistent_string_ref arg) override
      {
        if(arg)
        {
          std::cout << arg->value << std::endl;
        }
        return {};
      }
    };
  }

  struct var_lookup_result
  {
    var_ref value;

    var_ref expect_ok() const
    {
      if(!value)
      {
        throw std::runtime_error("missing var in minimal runtime");
      }
      return value;
    }
  };

  struct context
  {
    context() = default;

    std::string read_string(std::string s) const { return s; }

    var_lookup_result intern_var(std::string const &ns, std::string const &name)
    {
      auto const key(ns + "/" + name);
      auto found(vars.find(key));
      if(found == vars.end())
      {
        if(name == "println")
        {
          auto println_var = make_box<var>(make_box<detail::println_function>());
          found = vars.emplace(key, std::move(println_var)).first;
        }
        else
        {
          throw std::runtime_error("minimal runtime only knows println");
        }
      }
      return var_lookup_result{ found->second };
    }

  private:
    std::unordered_map<std::string, var_ref> vars;
  };

  inline context global_ctx{};
  inline context *__rt_ctx{ &global_ctx };

  inline object_ref dynamic_call(obj::jit_function_ref const &fn,
                                 obj::persistent_string_ref const &arg)
  {
    if(!fn)
    {
      throw std::runtime_error("dynamic_call received null function");
    }
    return fn->call(arg);
  }
}
