# Fix: context::find_var namespace alias resolution

## Problem

When using `jank.compiler/native-source` with quoted forms containing namespace aliases (e.g., `state/get-camera` where `state` is an alias from `:as`), the analyzer would fail with "Unable to resolve symbol".

This happened because `context::find_var()` only looked up namespace names directly in the global namespace map, without checking if the namespace portion was an alias in the current namespace.

## Root Cause

In `compiler+runtime/src/cpp/jank/runtime/context.cpp`, the `find_var` function:

```cpp
var_ref context::find_var(obj::symbol_ref const &sym)
{
  if(!sym->ns.empty())
  {
    ns_ref ns{};
    {
      auto const locked_namespaces(namespaces.rlock());
      auto const found(locked_namespaces->find(make_box<obj::symbol>("", sym->ns)));
      if(found == locked_namespaces->end())
      {
        return {};  // <-- Returned empty without checking aliases!
      }
      ns = found->second;
    }
    return ns->find_var(make_box<obj::symbol>("", sym->name));
  }
  // ...
}
```

When `sym->ns` was "state" (an alias), the lookup in `namespaces` would fail because "state" is not a real namespace name - it's just an alias defined in the current namespace.

## Solution

Modified `find_var` to check namespace aliases when direct lookup fails:

```cpp
ns_ref ns{};
bool try_alias{ false };
{
  auto const locked_namespaces(namespaces.rlock());
  auto const found(locked_namespaces->find(make_box<obj::symbol>("", sym->ns)));
  if(found == locked_namespaces->end())
  {
    try_alias = true;
  }
  else
  {
    ns = found->second;
  }
}

/* Namespace not found directly, try to resolve as alias in current namespace.
 * This is done outside the lock to avoid potential deadlocks. */
if(try_alias)
{
  auto const current_ns_obj(current_ns_var->deref());
  if(current_ns_obj->type != object_type::ns)
  {
    return {};
  }
  auto const current(expect_object<jank::runtime::ns>(current_ns_obj));
  auto const alias(current->find_alias(make_box<obj::symbol>(sym->ns)));
  if(alias.is_nil())
  {
    return {};
  }
  ns = alias;
}
```

**Important**:
1. The alias resolution must be done OUTSIDE the locked scope to avoid deadlocks when `current_ns_var->deref()` is called.
2. Added defensive type check before calling `expect_object` to handle edge cases gracefully.

This matches the behavior of `intern_keyword` which already resolves aliases correctly.

## Tests Added

- `test/jank/dev/native-source/pass-ns-alias.jank` - Tests `native-source` with aliased namespaces

## Additional Fixes

### Test Framework: Missing jank.compiler-native module
The test main (`test/cpp/main.cpp`) was not loading `jank_load_jank_compiler_native()`, which meant the `jank.compiler` module wasn't available in tests.

### Test: Namespace pollution between tests
`test/jank/form/ns/pass-re-eval-with-shadowed-var.jank` was changing `*ns*` to `test.ns.shadow` but not restoring it, causing subsequent tests to fail. Fixed by adding cleanup in `finally` block.

## Files Changed

- `compiler+runtime/src/cpp/jank/runtime/context.cpp` - Added alias resolution in `find_var`
- `compiler+runtime/test/cpp/main.cpp` - Added `jank_load_jank_compiler_native()` call
- `compiler+runtime/test/jank/form/ns/pass-re-eval-with-shadowed-var.jank` - Added namespace cleanup
- `compiler+runtime/test/jank/dev/native-source/pass-ns-alias.jank` - New test for alias resolution
