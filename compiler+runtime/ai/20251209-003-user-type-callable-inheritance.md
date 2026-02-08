# User Type Callable Inheritance

## Summary
Made `user_type` inherit from `behavior::callable` so it integrates properly with jank's call dispatch mechanism without needing special-case code in `evaluate.cpp`.

## Changes Made

### 1. user_type.hpp
- Added include for `<jank/runtime/behavior/callable.hpp>`
- Made `user_type` inherit from `behavior::callable`:
  ```cpp
  struct user_type : gc, behavior::callable
  ```
- Added `override` specifiers to all call methods
- Fixed signature mismatches:
  - `this_object_ref()` - removed `const` to match base class (non-const in callable)
  - `get_arity_flags()` - changed return type from `usize` to `arity_flag_t` (u8)

### 2. user_type.cpp
- Updated `this_object_ref()` implementation (no longer needs `const_cast`)
- Updated `get_arity_flags()` return type to `behavior::callable::arity_flag_t`
- Changed arity flag value from `0x7FF` to `0xFF` (fits in 8 bits)

### 3. evaluate.cpp
- Removed the special case for `obj::user_type` in call dispatch
- The existing `if constexpr(std::is_base_of_v<behavior::callable, T>)` now handles user_type automatically

## Key Insight
By inheriting from `behavior::callable`, user_type objects can be called using the same code path as regular functions, without needing special-case handling. This is cleaner and follows the existing patterns in jank.

## Virtual Method Requirements
When inheriting from `behavior::callable`, the following methods must be overridden:
- `call()` with 0-10 args (all must have `override` specifier)
- `this_object_ref()` - must be non-const
- `get_arity_flags()` - must return `arity_flag_t` (u8), not `usize`
