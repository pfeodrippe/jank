# Auto-Search All Namespaces in native-header-functions

## Problem

After the previous fix (005), users still had to pass the namespace explicitly when calling `native-header-functions` from contexts where `*ns*` wasn't the namespace with the alias:

```jank
;; Required explicit namespace
(native-header-functions (find-ns 'my-flecs-static) 'flecs "")
```

The user requested that we make it simpler by searching all namespaces automatically.

## Root Cause

The jank wrapper tried to iterate namespaces using `(all-ns)` or `cpp/clojure.core_native.all_ns`, but both approaches failed due to bootstrap issues:

1. `(all-ns)` was defined as throwing `"TODO: port all-ns"`
2. Calling `cpp/clojure.core_native.all_ns` failed because phase-1 compiler didn't have that symbol

## Solution

Move the namespace iteration into C++ by modifying `native_header_functions` to search all namespaces when `current_ns` is nil:

```cpp
object_ref native_header_functions(object_ref const current_ns,
                                   object_ref const alias,
                                   object_ref const prefix)
{
  auto const alias_sym(try_object<obj::symbol>(alias));
  jtl::option<ns::native_alias> alias_data;

  /* If current_ns is nil, search all namespaces for the alias. */
  if(runtime::is_nil(current_ns))
  {
    __rt_ctx->namespaces.withRLock([&](auto const &ns_map) {
      for(auto const &pair : ns_map)
      {
        auto const ns_ptr = try_object<ns>(pair.second);
        alias_data = ns_ptr->find_native_alias(alias_sym);
        if(alias_data)
        {
          break;
        }
      }
    });
    if(!alias_data)
    {
      throw std::runtime_error{
        util::format("Native alias '{}' not found in any namespace", alias_sym->to_string())
      };
    }
  }
  else
  {
    // ... original code for specific namespace
  }
  // ... rest of function
}
```

The jank wrapper now simply passes nil:

```jank
(defn native-header-functions
  ([alias]
   (native-header-functions alias ""))
  ([alias prefix]
   (throw-if (not (symbol? alias)) (str "Native alias must be a symbol: " alias))
   (throw-if (not (string? prefix)) (str "Prefix must be a string: " prefix))
   #?(:wasm (throw "Native C++ headers are not supported in WASM")
      :jank (cpp/clojure.core_native.native_header_functions nil alias prefix))))
```

## Usage

Now users can simply call:

```jank
(native-header-functions 'flecs)  ;; Searches all namespaces
```

## Files Modified

- `src/cpp/clojure/core_native.cpp`: Added nil namespace handling to search all namespaces
- `src/jank/clojure/core.jank`: Simplified wrapper to pass nil

## Test Results

```
:ALIAS_COMPLETIONS [entity entity_view iter table table_range type untyped_component untyped_ref world world_async_stage]
Flecs world created (via static object file)!
```

All completion tests pass (11 assertions).
