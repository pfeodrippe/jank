#pragma once

// Minimal subset of the jank runtime so generated C++ can be compiled for WASM.
// This intentionally avoids pulling in the real runtime dependencies and only
// implements enough behaviour for simple programs with println, basic arithmetic.

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <functional>

namespace jank::runtime
{
  struct object
  {
    virtual ~object() = default;

    virtual std::string to_string() const
    {
      return "#<object>";
    }
  };

  struct object_ref
  {
    object *ptr{ nullptr };

    object_ref() = default;

    explicit object_ref(object *p)
      : ptr{ p }
    {
    }

    object *operator->() const
    {
      return ptr;
    }

    operator bool() const
    {
      return ptr != nullptr;
    }
  };

  template <typename T>
  using oref = std::shared_ptr<T>;

  namespace obj
  {
    // Integer numbers
    struct integer : object
    {
      explicit integer(long long v)
        : value{ v }
      {
      }

      long long value;

      std::string to_string() const override
      {
        return std::to_string(value);
      }
    };

    using integer_ref = oref<integer>;

    // Floating point numbers
    struct real : object
    {
      explicit real(double v)
        : value{ v }
      {
      }

      double value;

      std::string to_string() const override
      {
        return std::to_string(value);
      }
    };

    using real_ref = oref<real>;

    struct persistent_string : object
    {
      explicit persistent_string(std::string v)
        : value{ std::move(v) }
      {
      }

      std::string value;

      std::string to_string() const override
      {
        return value;
      }
    };

    using persistent_string_ref = oref<persistent_string>;

    struct nil : object
    {
      std::string to_string() const override
      {
        return "nil";
      }
    };

    using nil_ref = oref<nil>;

    struct jit_function : object
    {
      explicit jit_function(std::string name)
        : debug_name{ std::move(name) }
      {
      }

      virtual object_ref call()
      {
        return {};
      }

      virtual object_ref call(object_ref)
      {
        return {};
      }

      virtual object_ref call(object_ref, object_ref)
      {
        return {};
      }

      virtual object_ref call(object_ref, object_ref, object_ref)
      {
        return {};
      }

      virtual object_ref call(persistent_string_ref)
      {
        return {};
      }

      std::string debug_name;

      std::string to_string() const override
      {
        return debug_name;
      }
    };

    using jit_function_ref = oref<jit_function>;
  }

  template <typename T, typename... Args>
  oref<T> make_box(Args &&...args)
  {
    return std::make_shared<T>(std::forward<Args>(args)...);
  }

  // Helper to create boxed integers from arithmetic results
  inline oref<obj::integer> make_box(long long v)
  {
    return std::make_shared<obj::integer>(v);
  }

  inline oref<obj::real> make_box(double v)
  {
    return std::make_shared<obj::real>(v);
  }

  struct var : object
  {
    explicit var(obj::jit_function_ref fn)
      : fn{ std::move(fn) }
    {
    }

    obj::jit_function_ref deref() const
    {
      return fn;
    }

    obj::jit_function_ref fn;
  };

  using var_ref = oref<var>;

  // Arithmetic operations
  inline long long add(object_ref const &a, object_ref const &b)
  {
    auto *ai = dynamic_cast<obj::integer *>(a.ptr);
    auto *bi = dynamic_cast<obj::integer *>(b.ptr);
    if(ai && bi)
    {
      return ai->value + bi->value;
    }
    throw std::runtime_error("add: unsupported types");
  }

  inline long long sub(object_ref const &a, object_ref const &b)
  {
    auto *ai = dynamic_cast<obj::integer *>(a.ptr);
    auto *bi = dynamic_cast<obj::integer *>(b.ptr);
    if(ai && bi)
    {
      return ai->value - bi->value;
    }
    throw std::runtime_error("sub: unsupported types");
  }

  inline long long mul(object_ref const &a, object_ref const &b)
  {
    auto *ai = dynamic_cast<obj::integer *>(a.ptr);
    auto *bi = dynamic_cast<obj::integer *>(b.ptr);
    if(ai && bi)
    {
      return ai->value * bi->value;
    }
    throw std::runtime_error("mul: unsupported types");
  }

  inline long long div(object_ref const &a, object_ref const &b)
  {
    auto *ai = dynamic_cast<obj::integer *>(a.ptr);
    auto *bi = dynamic_cast<obj::integer *>(b.ptr);
    if(ai && bi)
    {
      if(bi->value == 0)
      {
        throw std::runtime_error("div: division by zero");
      }
      return ai->value / bi->value;
    }
    throw std::runtime_error("div: unsupported types");
  }

  namespace detail
  {
    struct println_function : obj::jit_function
    {
      println_function()
        : obj::jit_function{ "{:name \"println\"}" }
      {
      }

      object_ref call() override
      {
        std::cout << std::endl;
        return {};
      }

      object_ref call(object_ref arg) override
      {
        if(arg.ptr)
        {
          std::cout << arg->to_string() << std::endl;
        }
        return {};
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

    struct plus_function : obj::jit_function
    {
      plus_function()
        : obj::jit_function{ "{:name \"+\"}" }
      {
      }

      object_ref call(object_ref a, object_ref b) override
      {
        return object_ref{ make_box(add(a, b)).get() };
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

    std::string read_string(std::string s) const
    {
      return s;
    }

    var_lookup_result intern_var(std::string const &ns, std::string const &name)
    {
      auto const key(ns + "/" + name);
      auto found(vars.find(key));
      if(found == vars.end())
      {
        obj::jit_function_ref fn;
        if(name == "println")
        {
          fn = make_box<detail::println_function>();
        }
        else if(name == "+")
        {
          fn = make_box<detail::plus_function>();
        }
        else
        {
          // Unknown var - just return an empty result for now
          // This allows user-defined functions to be created
          return var_lookup_result{};
        }
        auto var_obj = make_box<var>(std::move(fn));
        found = vars.emplace(key, std::move(var_obj)).first;
      }
      return var_lookup_result{ found->second };
    }

  private:
    std::unordered_map<std::string, var_ref> vars;
  };

  inline context global_ctx{};
  inline context *__rt_ctx{ &global_ctx };

  inline object_ref dynamic_call(obj::jit_function_ref const &fn)
  {
    if(!fn)
    {
      throw std::runtime_error("dynamic_call received null function");
    }
    return fn->call();
  }

  inline object_ref dynamic_call(obj::jit_function_ref const &fn, object_ref const &arg)
  {
    if(!fn)
    {
      throw std::runtime_error("dynamic_call received null function");
    }
    return fn->call(arg);
  }

  inline object_ref
  dynamic_call(obj::jit_function_ref const &fn, obj::persistent_string_ref const &arg)
  {
    if(!fn)
    {
      throw std::runtime_error("dynamic_call received null function");
    }
    return fn->call(arg);
  }

  inline object_ref
  dynamic_call(obj::jit_function_ref const &fn, object_ref const &a1, object_ref const &a2)
  {
    if(!fn)
    {
      throw std::runtime_error("dynamic_call received null function");
    }
    return fn->call(a1, a2);
  }
}
