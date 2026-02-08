# Namespace Display Fix for nREPL Info/Eldoc/Complete

## Problem

The nREPL info, eldoc, and complete operations were showing incorrect namespaces:

1. **Jank vars**: `clojure.core/map` was showing as `my-ns/map` (the lookup namespace instead of the actual namespace where the var is defined)

2. **Native header entities**: `flecs/world.get` was showing instead of `cpp/flecs.world.get`

## Solution

### For Jank Vars (`describe_var` in `engine.hpp`)

Changed from using the lookup namespace (`target_ns`) to using the var's actual namespace:

```cpp
// Before: info.ns_name = to_std_string(target_ns->name->to_string());
// After:
info.ns_name = to_std_string(var->n->name->to_string());
```

The `var->n` field is a `ns_ref` pointing to the namespace where the var is actually defined.

### For Native Header Entities

Changed `describe_native_header_function` and `describe_native_header_type` to:

1. Use `"cpp"` as the namespace
2. Include the full C++ path in the name

```cpp
info.ns_name = "cpp";
if(alias.scope.empty())
{
  info.name = symbol_name;
}
else
{
  info.name = to_std_string(alias.scope) + "." + symbol_name;
}
```

For example, if the alias scope is `flecs.world` and symbol is `get`, the result is:
- `ns_name = "cpp"`
- `name = "flecs.world.get"`

This makes the display show `cpp/flecs.world.get` instead of `flecs/world.get`.

## Parameter Cleanup

After these changes, some parameters became unused:

1. **`alias_name`**: Removed from `describe_native_header_entity`, `describe_native_header_function`, `describe_native_header_type`
2. **`target_ns`**: Removed from `describe_var`

All call sites in `complete.hpp`, `info.hpp`, `eldoc.hpp` were updated accordingly.

## Files Modified

- `include/cpp/jank/nrepl_server/engine.hpp`
- `include/cpp/jank/nrepl_server/ops/complete.hpp`
- `include/cpp/jank/nrepl_server/ops/info.hpp`
- `include/cpp/jank/nrepl_server/ops/eldoc.hpp`
- `test/cpp/jank/nrepl/engine.cpp` (updated test expectations)

## Key Insight

The `var` object in jank has an `n` field (`ns_ref`) that points to its actual defining namespace. This is different from the "lookup namespace" which is just where you're looking from. Always use `var->n->name` to get the true namespace of a var.
