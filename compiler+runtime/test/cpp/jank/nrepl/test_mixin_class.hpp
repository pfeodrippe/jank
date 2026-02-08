/* Test class that uses #include inside the class body.
 * This pattern is used by flecs::world and other libraries.
 *
 * The #include inside a class body creates a complex declaration chain
 * that causes CppInterOp's GetAllCppNames and GetClassMethods to fail.
 * The fix uses Clang's noload_lookups() to iterate the lookup table instead. */
#pragma once

namespace mixin_test
{

  struct complex_class
  {
    /* Regular methods defined directly */
    void direct_method()
    {
    }

    int direct_get()
    {
      return 0;
    }

    /* Methods included from mixin file */
#include "test_mixin_methods.hpp"
  };

} /* namespace mixin_test */
