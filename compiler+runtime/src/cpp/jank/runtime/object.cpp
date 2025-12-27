#include <jank/runtime/object.hpp>
#include <jank/runtime/core/equal.hpp>
#include <jank/runtime/visit.hpp>
#include <jank/hash.hpp>

#include <iostream>

namespace jank::runtime
{
  bool very_equal_to::operator()(object_ref const lhs, object_ref const rhs) const noexcept
  {
    if(lhs->type != rhs->type)
    {
      return false;
    }
    return equal(lhs, rhs);
  }

  bool operator==(object const * const lhs, object_ref const rhs)
  {
    return lhs == rhs.data;
  }

  bool operator!=(object const * const lhs, object_ref const rhs)
  {
    return lhs != rhs.data;
  }
}

namespace std
{
  using namespace jank;
  using namespace jank::runtime;

  size_t hash<object_ref>::operator()(object_ref const o) const noexcept
  {
    if(!o.data)
    {
      std::cerr << "[HASH DEBUG] null object!\n";
      std::abort();
    }
    auto type_val = static_cast<int>(o->type);
    if(type_val < 0 || type_val > 100)
    {
      std::cerr << "[HASH DEBUG] suspicious type=" << type_val
                << " ptr=" << static_cast<void const *>(o.data) << "\n";
    }
    return jank::hash::visit(o.data);
  }

  size_t hash<object>::operator()(object const &o) const noexcept
  {
    return jank::hash::visit(const_cast<runtime::object *>(&o));
  }

  bool
  // NOLINTNEXTLINE(bugprone-exception-escape): TODO: Sort this out.
  equal_to<object_ref>::operator()(object_ref const lhs, object_ref const rhs) const noexcept
  {
    return equal(lhs, rhs);
  }
}
