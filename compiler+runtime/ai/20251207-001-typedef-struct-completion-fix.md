# Typedef Struct Completion Fix

## Problem

When using native header completions with an empty scope (for C headers):
```clojure
(ns vybe.flecs (:require ["flecs.h" :as fl :scope ""]))
```

Structs defined using C-style typedef pattern were not appearing in completions:
```c
typedef struct { ... } MyStruct;  // Not found by completions
```

## Root Cause

`Cpp::IsClass()` from CppInterOp returns `false` for TypedefDecl (the typedef name). It only returns `true` for RecordDecl (the actual struct/class). When C code uses `typedef struct {...} Name;`, calling `Cpp::GetScope("Name")` returns the TypedefDecl, not the underlying RecordDecl.

## Solution

In two locations, we now check if the scope is a TypedefDecl and get the underlying type:

### 1. native_header_completion.cpp - `enumerate_native_header_symbols`

```cpp
bool is_class = Cpp::IsClass(child_scope);
bool is_enum = Cpp::IsEnumScope(child_scope);

/* Handle C-style typedef structs: typedef struct {...} Name; */
if(!is_class && !is_enum)
{
  auto *decl = static_cast<clang::Decl *>(child_scope);
  if(auto *typedef_decl = clang::dyn_cast<clang::TypedefNameDecl>(decl))
  {
    auto underlying_type = typedef_decl->getUnderlyingType();
    if(auto *tag_decl = underlying_type->getAsTagDecl())
    {
      if(clang::isa<clang::RecordDecl>(tag_decl))
      {
        is_class = true;
        child_scope = tag_decl;
      }
      else if(clang::isa<clang::EnumDecl>(tag_decl))
      {
        is_enum = true;
        child_scope = tag_decl;
      }
    }
  }
}
```

### 2. engine.hpp - `describe_native_header_type`

Same pattern applied to ensure type descriptions work correctly.

## Required includes

```cpp
#include <clang/AST/Type.h>
```

## Test

Test case `complete returns typedef structs from C header with global scope` in `test/cpp/jank/nrepl/engine.cpp` validates:
- Completion for `rl/Vec` prefix returns Vector2, Vector3
- Completion for `rl/Col` prefix returns Color

Uses `test_c_header.h` which defines typedef structs at global scope.

### 3. Field enumeration for the "info" response

For the info/eldoc response, we also needed to enumerate struct fields for the "Fields:" section.
`Cpp::GetDatamembers()` only works for C++ classes (`CXXRecordDecl`), not plain C structs (`RecordDecl`).

Solution: Use Clang AST directly to iterate fields:

```cpp
/* Enumerate struct/class fields for the Fields: section */
auto *decl = static_cast<clang::Decl *>(type_scope);
if(auto *record_decl = clang::dyn_cast<clang::RecordDecl>(decl))
{
  for(auto *field : record_decl->fields())
  {
    var_documentation::cpp_field field_doc;
    field_doc.name = field->getNameAsString();
    field_doc.type = field->getType().getAsString();
    info.cpp_fields.emplace_back(std::move(field_doc));
  }
}
```

### 4. Qualified name for global scope

When looking up types with empty scope, `Cpp::GetScopeFromCompleteName("::TypeName")` doesn't work.
Fix: Use just the symbol name for global scope (empty alias.scope):

```cpp
std::string qualified_name;
if(alias.scope.empty())
{
  qualified_name = symbol_name;  // Just "Vector2", not "::Vector2"
}
else
{
  qualified_name = to_std_string(alias.scope) + "::" + symbol_name;
}
```

## Tests

1. `complete returns typedef structs from C header with global scope` - Validates completion for typedef structs
2. `info returns fields for typedef structs from C header` - Validates that info response includes cpp-fields

Uses `test_c_header.h` which defines:
- `typedef struct Color { unsigned char r, g, b, a; } Color;`
- `typedef struct { float x, y; } Vector2;`
- `typedef struct { float x, y, z; } Vector3;`

## Impact

This fix also resolved 7 other failing tests that depended on typedef struct handling:
- complete flecs-like world with mixin includes
- complete returns members for classes with #include inside class body
- Various other completion tests

Before: 210 tests, 202 passed, 8 failed
After: 212 tests, 211 passed, 1 failed (unrelated pre-existing issue)
