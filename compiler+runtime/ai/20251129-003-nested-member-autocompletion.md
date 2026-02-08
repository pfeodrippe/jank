# Nested Member Autocompletion for Native Header Aliases

## Summary

Added support for nested member autocompletion when using native header aliases. Previously, `flecs/world` would autocomplete to show the `world` type, but `flecs/world.` would not show member functions like `defer_begin`, `defer_end`, etc.

## Problem

When using native header aliases like:
```clojure
(require ["flecs.h" :as flecs :scope "flecs"])
```

The autocompletion worked for top-level symbols:
- `flecs/wor` -> `flecs/world` (type)

But did NOT work for nested members:
- `flecs/world.` -> (empty, should show member functions)

## Solution

Modified three files to support nested member completion:

### 1. `src/cpp/jank/nrepl_server/native_header_completion.cpp`

Added `enumerate_type_members()` function that:
- Takes a type path (e.g., "world") and member prefix (e.g., "" or "defer_")
- Builds the full C++ qualified name (e.g., "flecs::world")
- Uses `Cpp::GetScopeFromCompleteName()` to get the type's scope
- Uses `Cpp::GetAllCppNames()` to enumerate all names in the type
- Filters to return member functions and nested types
- Returns names as `type_path.member_name` (e.g., "world.defer_begin")

Modified `enumerate_native_header_symbols()` to:
- Detect when prefix contains a dot (e.g., "world." or "world.defer")
- Split into type path and member prefix
- Call `enumerate_type_members()` for nested member enumeration

### 2. `src/cpp/jank/nrepl_server/native_header_index.cpp`

Modified `list_functions()` to:
- Detect when prefix contains a dot
- Bypass the cache and call `enumerate_native_header_symbols()` directly
- The cache only contains top-level symbols, so nested members must be enumerated dynamically

### 3. `include/cpp/jank/nrepl_server/engine.hpp`

Modified `describe_native_header_function()` to:
- Detect when symbol name contains a dot (e.g., "world.defer_begin")
- Parse type path and function name
- Look up the type's scope and find the function there
- This enables proper documentation/signature display for member functions

Added helper function `dots_to_cpp_scope()` to convert jank-style dot notation to C++ scope notation.

## Test

Added test case `"complete returns nested member functions for types via native header alias"` in `test/cpp/jank/nrepl/engine.cpp` that:
- Creates a test struct with member functions using `cpp/raw`
- Requires it via a native header alias with `:scope`
- Verifies that `alias/Type.` completion returns member functions

## Usage Example

```clojure
(ns my-flecs-static
  (:require
   ["flecs.h" :as flecs :scope "flecs"]))

;; Now these completions work:
;; flecs/world      -> (type) flecs::world
;; flecs/world.     -> defer_begin, defer_end, progress, etc.
;; flecs/world.def  -> defer_begin, defer_end
```

## Crash Fix for Complex Classes (Including Template Bases)

Complex C++ classes like `flecs::world` could crash when trying to enumerate members.
The crash happened in `Cpp::GetAllCppNames()` when iterating over declarations in
classes with complex declaration chains (e.g., classes using `#include` inside the body for mixins).

### Root Cause

The crash was `EXC_BAD_ACCESS` in `clang::Decl::getNextDeclInContext()` with `this=nullptr`.
This happens because CppInterOp's `GetAllCppNames()` iterates over the raw declaration chain,
which can be corrupted in complex classes.

### Solution: Use GetClassMethods Instead

Instead of using `GetAllCppNames()`, we now use `Cpp::GetClassMethods()` which:
- Uses a different code path internally
- Iterates over `CXXRecordDecl::methods()` instead of the raw declaration list
- Avoids the corrupted declaration chain issue

```cpp
bool safe_get_class_members(void *scope, std::set<std::string> &names)
{
  if(!scope)
    return false;

  std::vector<void *> methods;
  Cpp::GetClassMethods(scope, methods);

  for(auto const method : methods)
  {
    if(!method)
      continue;
    auto const name = Cpp::GetFunctionSignature(method);
    // Extract function name and add to names...
  }
  return true;
}
```

### Test Results

Both tests now pass AND return completions:
- **Simple struct test**: 3 member completions
- **Template base class test**: 3 member completions (previously crashed or returned 0!)

### Key Improvement

Classes with template bases (like `flecs::world`) **now get member autocompletion** instead of
returning empty. The `GetClassMethods()` approach works for all classes, including those with
complex declaration structures.

## Key CppInterOp APIs Used

- `Cpp::GetScopeFromCompleteName(name)` - Get scope by fully qualified name
- `Cpp::GetClassMethods(scope, methods)` - Get all class methods (SAFE, doesn't crash)
- `Cpp::GetFunctionSignature(method)` - Get function signature string
- `Cpp::GetFunctionsUsingName(scope, name)` - Get function overloads by name
- `Cpp::IsClass(scope)` / `Cpp::IsEnumScope(scope)` - Check scope type
- `Cpp::IsConstructor(fn)` - Filter out constructors from member lists
- `Cpp::IsComplete(scope)` - Check if type is complete (not forward-declared)
