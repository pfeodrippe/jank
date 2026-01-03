# user_type Factory Pattern API

## Summary
Changed `make-user-type` to return a factory function instead of directly creating instances. This eliminates the need for a separate `user_type_data` function and provides a cleaner API.

## API Change

### Old API (3-arg)
```clojure
(make-user-type "MyType" {:get (fn [this k] ...) ...} {:a 1})
```
- Constructor received type-name, vtable-map, and data as separate arguments
- Required a `user_type_data` function to access internal data
- Vtable functions couldn't easily close over user data

### New API (2-arg factory)
```clojure
(def MyType
  (make-user-type
    "MyType"
    (fn [data]
      {:get (fn [this k] (get data k))
       :to-string (fn [this] (str "#MyType" data))})))

(def instance (MyType {:a 1 :b 2}))
```
- `make-user-type` takes type-name and constructor-fn
- Returns a **factory function** that creates instances when called with data
- Constructor function receives data and returns vtable map
- Vtable closures can directly capture data (no `user_type_data` needed)

## Implementation Details

### C++ Changes

**user_type.hpp** (line 170-173):
```cpp
/* Factory function that returns a factory for creating user_type instances. */
object_ref make_user_type(object_ref type_name, object_ref constructor_fn);
```

**user_type.cpp** (lines 425-440):
```cpp
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
```

Key points:
- Uses `native_function_wrapper` to wrap the factory lambda
- Lambda must be wrapped in `std::function` (native_function_wrapper requirement)
- Captures type name and constructor_fn
- When called with data, invokes constructor_fn to get vtable map

### Required Includes
In `user_type.cpp`:
```cpp
#include <jank/runtime/obj/native_function_wrapper.hpp>
```

## Benefits
1. Cleaner API - no need for `user_type_data`
2. Data encapsulation through closures
3. Follows factory pattern common in OOP
4. User has full control over how data is stored and accessed

## Test Examples
See `test/jank/form/user-type/pass-basic.jank` and `test/jank/form/user-type/pass-callable.jank` for usage examples.
