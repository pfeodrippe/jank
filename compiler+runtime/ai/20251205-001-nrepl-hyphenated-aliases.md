# nREPL Hyphenated Alias Investigation

## Summary

Investigation into the info operation for template functions with hyphenated native alias names.

## Key Finding

**Headers with `:scope` must be in the clang include path.**

When using `require` with `:scope`, the header path must be accessible by clang's libTooling. Relative paths like `../test/cpp/jank/nrepl/template_types.hpp` don't work because they're not in the include path.

## Working Pattern

The correct pattern for tests that need `:scope` with custom namespaces is to place the header in the include path:

```clojure
;; The header is located at include/cpp/jank/test/template_types.hpp
(require '["jank/test/template_types.hpp" :as tmpl-test :scope "template_type_test"])

;; Now info works for tmpl-test/entity.get_id
```

The header file `jank/test/template_types.hpp` defines:
- `template_type_test::entity` struct with template methods
- Template functions like `identity<T>`, `convert<T,U>`, `variadic_func<Args...>`

## Why External Headers Don't Work for `:scope`

The `describe_native_header_function` function in `engine.hpp` uses `analyze::cpp_util::resolve_scope(alias.scope)` to resolve the C++ namespace. This relies on:

1. The header being successfully `#include`d via JIT
2. The scope being findable by CppInterOp's `GetScopeFromCompleteName`

When using relative paths like `../test/...`, clang can't find the header from its include paths, so the namespace doesn't get compiled and `resolve_scope` fails.

## Test Header Location

Created `include/cpp/jank/test/template_types.hpp` with template types for testing.

## Tests Updated

The tests in `test/cpp/jank/nrepl/engine.cpp` for "info returns proper types for template functions, not auto" now use headers from the include path:

- `SUBCASE("non-template method works to verify header loading")`
- `SUBCASE("variadic template member function shows Args types")`
- `SUBCASE("simple template function with T parameter")`
- `SUBCASE("template method with mixed parameters")`

All 4 subcases now pass (18 assertions total).

## Hyphenated Aliases Work

Hyphenated alias names like `tmpl-test` work correctly for both completion and info operations when the header is in the include path.
