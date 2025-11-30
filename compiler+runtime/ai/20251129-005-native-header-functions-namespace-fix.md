# Fix native-header-functions Namespace Lookup

## Problem

When calling `(native-header-functions 'flecs)` from a `-main` function, it threw:

```
Native alias 'flecs' is not registered in namespace 'clojure.core'
```

The issue was that `*ns*` (current namespace) was bound to `clojure.core` at runtime,
not the namespace where the alias was defined (`my-flecs-static`).

## Root Cause

The `native-header-functions` wrapper in `clojure.core` passed `*ns*` to the C++ function:

```jank
(defn native-header-functions
  ([alias prefix]
   (cpp/clojure.core_native.native_header_functions *ns* alias prefix)))
```

At runtime (when `-main` is executed), `*ns*` is not necessarily bound to the
namespace where the native header alias was registered.

## Solution

Added a 3-arity version that accepts the namespace explicitly:

```jank
(defn native-header-functions
  ([alias]
   (native-header-functions alias ""))
  ([alias prefix]
   (cpp/clojure.core_native.native_header_functions *ns* alias prefix))
  ([ns-to-use alias prefix]
   (cpp/clojure.core_native.native_header_functions ns-to-use alias prefix)))
```

Users can now pass the namespace explicitly when calling from contexts where
`*ns*` may not be correct (like `-main` functions):

```jank
(native-header-functions (find-ns 'my-flecs-static) 'flecs "")
```

## Files Modified

- `src/jank/clojure/core.jank`: Updated `native-header-functions` with 3-arity version
- `/Users/pfeodrippe/dev/something/src/my_flecs_static.jank`: Updated to pass namespace explicitly

## Test Results

Real flecs usage now works:
```
:ALIAS_COMPLETIONS [entity entity_view iter table table_range type untyped_component untyped_ref world world_async_stage]
Flecs world created (via static object file)!
```

All 14 completion tests pass with 131 assertions.
