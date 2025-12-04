# Eval Respects Explicit NS Field

## Summary

Fixed the nREPL `eval` operation to respect the explicit `ns` field in the message. Previously, eval always used `session.current_ns` regardless of what namespace the editor requested.

## Problem

When CIDER or other editors send an eval request, they include an `ns` field that specifies which namespace the code should be evaluated in (typically the namespace of the current buffer). For example:

```
{:op "eval"
 :code "(rl/GetFPS)"
 :ns "my-integrated-demo"
 :file "/path/to/file.jank"}
```

The code should be evaluated in the `my-integrated-demo` namespace, using its aliases and refers. But jank was ignoring this field and always using the session's current namespace.

## Fix

Modified `include/cpp/jank/nrepl_server/ops/eval.hpp`:

1. Read the `ns` field from the message
2. If provided, look up that namespace using `__rt_ctx->find_ns()`
3. If found, use it for the `*ns*` binding during evaluation
4. Otherwise, fall back to `session.current_ns`

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

## Test Cases Added

- `eval respects explicit ns field` - Verifies that code is evaluated in the requested namespace
- `eval with explicit ns resolves aliases from that namespace` - Verifies that namespace aliases work correctly
