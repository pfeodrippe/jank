#pragma once

namespace template_type_test
{
  /* Simple template function with a single type parameter */
  template <typename T>
  T identity(T value)
  {
    return value;
  }

  /* Template function with multiple type parameters */
  template <typename T, typename U>
  T convert(U value)
  {
    return static_cast<T>(value);
  }

  /* Variadic template function - like flecs::entity::child */
  template <typename... Args>
  int variadic_func(Args &&...args)
  {
    return sizeof...(args);
  }

  /* Class with template member functions */
  struct entity
  {
    /* Non-template method for comparison */
    int get_id() const
    {
      return 42;
    }

    /* Template method with single parameter */
    template <typename T>
    T get() const
    {
      return T{};
    }

    /* Template method with variadic parameters - like flecs::entity::child */
    template <typename... Args>
    entity child(Args &&...args) const
    {
      return entity{};
    }

    /* Template method with mixed parameters */
    template <typename T>
    entity set(char const *name, T &&value)
    {
      return *this;
    }
  };
}
