# iOS JIT: load-file ns form evaluation fix

## Problem

When using `load-file` on iOS JIT-only mode, files with an `ns` form containing `:require` would fail:

```
[compile-client] Compile error: analyze/unresolved-symbol: Unable to resolve symbol 'u/v->p'.
```

The file `vybe.sdf.ui` had:
```clojure
(ns vybe.sdf.ui
  (:require
   [vybe.util :as u]  ; <-- defines u/v->p
   ...))

(defonce *export-colors (u/v->p true))  ; <-- fails: 'u' not resolved
```

## Root Cause

The `compile_code` function in `server.hpp` was analyzing all forms without first **evaluating** the `ns` form. This meant:

1. The `:require` clauses were analyzed but not executed
2. The alias `u` for `vybe.util` was not registered
3. When analyzing `(u/v->p true)`, the symbol `u/v->p` couldn't be resolved

In contrast, `compile_namespace_source` (used for `require`) correctly evaluated the ns form first before analyzing the rest.

## Solution

Modified `compile_code` to:

1. Parse all forms first
2. Check if the first form is an `ns` form
3. If so, **evaluate** it (not just analyze) to load requires and register aliases
4. Then analyze all forms with the proper namespace bindings

### Key changes in `server.hpp`:

```cpp
// Helper to check if a form is an ns form
static bool is_ns_form(runtime::object_ref form)
{
  auto const list = runtime::try_object<runtime::obj::persistent_list>(form);
  if(list.is_nil() || list->data.empty())
  {
    return false;
  }
  auto const first = list->data.first();
  if(first.is_none())
  {
    return false;
  }
  auto const sym = runtime::try_object<runtime::obj::symbol>(first.unwrap());
  if(sym.is_nil())
  {
    return false;
  }
  return sym->name == "ns";
}

// In compile_code():
// Step 2: If first form is an ns form, evaluate it to load requires
if(is_ns_form(all_forms.front()))
{
  std::cout << "[compile-server] Evaluating ns form to load requires..." << std::endl;
  // ... evaluate ns form and update eval_ns to the target namespace
}

// Step 3: Analyze all forms with proper namespace binding
```

## Testing

After the fix, `load-file` works correctly:

```clojure
;; load-file now works
(load-file "/path/to/vybe/sdf/ui.jank")
;; => Successfully loads, u/v->p resolves correctly
```

## Files Modified

- `include/cpp/jank/compile_server/server.hpp` - Added `is_ns_form` helper and ns form evaluation in `compile_code`
