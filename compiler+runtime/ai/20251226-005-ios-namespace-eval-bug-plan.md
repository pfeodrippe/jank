# iOS nREPL Namespace Evaluation Bug - Fix Plan

## Problem Statement

When evaluating code on the iOS nREPL (port 5558) with a specific namespace context:
- **Request**: `ns "vybe.sdf.ui"`, code `(defn aaa [] (+ 4 1))`
- **Expected**: `#'vybe.sdf.ui/aaa`
- **Actual**: `#'clojure.core/aaa`

The var is being defined in `clojure.core` instead of `vybe.sdf.ui`.

## Root Cause Analysis

Looking at the compile server output:
```
[compile-server] Namespace .Users.pfeodrippe.Library.Developer.CoreSimulator.Devices.57653CE6-DF09-4724-8B28-7CB6BA90E0E3.data.Containers.Bundle.Application.C2453F1C-C4A8-4ADC-A6DC-7BA2F6C87DA2.SdfViewerMobile-JIT-Only.app.src.jank.vybe.sdf.ui compiled successfully
```

The namespace is being registered with a **path-based name** (`.Users.pfeodrippe....vybe.sdf.ui`) instead of the proper namespace name (`vybe.sdf.ui`).

In `handle_eval` (eval.hpp lines 134-143):
```cpp
auto const requested_ns(msg.get("ns"));
object_ref eval_ns{ session.current_ns };
if(!requested_ns.empty())
{
  auto const ns_sym(make_box<obj::symbol>(make_immutable_string(requested_ns)));
  auto const found_ns(__rt_ctx->find_ns(ns_sym));
  if(!found_ns.is_nil())
  {
    eval_ns = found_ns;
  }
}
```

When `find_ns("vybe.sdf.ui")` is called, it returns nil because the namespace is actually registered under the path-based name. Therefore, `eval_ns` remains as `session.current_ns` which defaults to `user` or `clojure.core`.

## Investigation Steps

1. **Verify namespace registration** - Check how namespaces are registered when loaded via remote compilation
   - File: `src/cpp/jank/runtime/ns.cpp` or similar
   - Look for where `intern_ns` is called after loading remote modules

2. **Check module loader** - How does the iOS module loader register namespaces?
   - File: Look for remote module loading code
   - The `[loader] Loaded remote module: vybe.sdf.ui` message suggests the loader knows the correct name

3. **Check namespace name extraction** - Where does the path-based name come from?
   - Likely in the compile server when it compiles namespaces
   - The `ns` form should extract the proper namespace name

## Potential Fixes

### Option A: Fix namespace registration on iOS module load
When loading a remote module, ensure the namespace is registered under its proper name (from the `(ns ...)` form), not the path-based name.

### Option B: Create namespace alias
When loading a module, create an alias from the proper name to the path-based name.

### Option C: Fix compile server namespace handling
Ensure the compile server extracts and sends the proper namespace name, not the path-based name.

## Files to Investigate

1. `include/cpp/jank/nrepl_server/ops/eval.hpp` - Line 134-143, namespace resolution
2. `include/cpp/jank/compile_server/server.hpp` - How namespaces are compiled
3. iOS module loader code - How modules are loaded and namespaces registered
4. `src/cpp/jank/runtime/context.cpp` or similar - `find_ns` and `intern_ns` implementation

## Test Plan

1. After fix, evaluate `(defn aaa [] (+ 4 1))` in `vybe.sdf.ui` buffer
2. Verify result is `#'vybe.sdf.ui/aaa`
3. Verify `(vybe.sdf.ui/aaa)` returns `5`
4. Test eldoc shows correct namespace for `aaa`

## Next Steps

1. Search for where namespaces are registered after remote module loading
2. Compare namespace name in `(ns ...)` form vs registered name
3. Fix the registration to use proper namespace name
