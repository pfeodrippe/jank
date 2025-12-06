#pragma once

namespace template_type_test
{
  struct entity
  {
    int get_id() const
    {
      return 42;
    }

    template<typename T>
    T get() const
    {
      return T{};
    }

    template<typename... Args>
    entity child(Args &&...args) const
    {
      return entity{};
    }

    template<typename T>
    entity set(const char *name, T &&value)
    {
      return *this;
    }
  };

  template<typename T>
  T identity(T value)
  {
    return value;
  }

  template<typename T, typename U>
  T convert(U value)
  {
    return static_cast<T>(value);
  }

  template<typename... Args>
  int variadic_func(Args &&...args)
  {
    return sizeof...(args);
  }
}
