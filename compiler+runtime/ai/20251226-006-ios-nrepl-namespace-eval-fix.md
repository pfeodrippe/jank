# iOS nREPL Namespace Eval Fix

## Date: 2025-12-26

## Problem

When evaluating code on the iOS nREPL (port 5558) with a specific namespace context:
- **Request**: `ns "vybe.sdf.ui"`, code `(defn aaa [] (+ 4 1))`
- **Expected**: `#'vybe.sdf.ui/aaa`
- **Actual**: `#'clojure.core/aaa`

The var was being defined in `clojure.core` instead of the requested namespace.

## Root Cause

The issue was in the **compile server**.

In `include/cpp/jank/compile_server/server.hpp`, the `compile_code` function received the namespace parameter but didn't set up a binding scope for compilation. The analyzer used `runtime::__rt_ctx->current_ns()` (the server's current namespace) instead of the requested namespace.

## Fix

### Compile Server (server.hpp) - Set namespace binding scope

Added a binding scope to set `*ns*` to the correct namespace before parsing and analyzing:

```cpp
// Set up namespace binding for compilation
// This ensures def/defn create vars in the correct namespace
auto const ns_sym = runtime::make_box<runtime::obj::symbol>(jtl::immutable_string(ns));
auto const eval_ns = runtime::__rt_ctx->intern_ns(ns_sym);
auto const bindings = runtime::obj::persistent_hash_map::create_unique(
  std::make_pair(runtime::__rt_ctx->current_ns_var, eval_ns));
runtime::context::binding_scope const scope{ bindings };
```

## Files Modified

1. `include/cpp/jank/compile_server/server.hpp` - Add namespace binding scope in `compile_code`

## Testing

```bash
# Start compile server
cd /Users/pfeodrippe/dev/something && make sdf-ios-server

# Start iOS app
make ios-jit-only-sim-run

# Test with nREPL
clj-nrepl-eval -p 5558 "(in-ns 'vybe.sdf.ui)"
clj-nrepl-eval -p 5558 "(defn aaa [] (+ 4 1))"
# Result: #'vybe.sdf.ui/aaa  (NOT #'clojure.core/aaa)

clj-nrepl-eval -p 5558 "(aaa)"
# Result: 5
```

## Additional Fix: Flecs Helpers

Also added `vybe_flecs_jank.cpp` to `project-jit-only-sim.yml` to fix missing Flecs helper symbols:

```yaml
# Vybe Flecs helpers (needed for vybe.flecs jank bindings)
- path: ../vendor/vybe/vybe_flecs_jank.cpp
  compilerFlags: ["-O2"]
```
