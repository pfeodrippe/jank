# nREPL Print Middleware Implementation

## Summary

Implemented nREPL print middleware support for the `nrepl.middleware.print/print` parameter. When CIDER (or other nREPL clients) sends this parameter in eval requests, jank now uses the specified print function to format results instead of the default `pr-str`.

## Background

CIDER (and other nREPL clients) can request custom printing of eval results by passing the `nrepl.middleware.print/print` parameter in eval requests. The parameter specifies a qualified function name (e.g., `"cider.nrepl.pprint/pprint"`) that should be used to format the result.

## Implementation

Modified `handle_eval` in `eval.hpp` to:

1. Check for the `nrepl.middleware.print/print` parameter
2. If present, resolve the var using `(resolve 'fn-name)`
3. Call the print function with `(fn result nil)` - nil for writer since jank doesn't have writers
4. Capture the output printed to `*out*` using `scoped_output_redirect`
5. Remove trailing newline (pprint adds one)
6. Use captured output as the "value" in response

Key code added at `eval.hpp:255-298`:
```cpp
auto const print_fn_name(msg.get("nrepl.middleware.print/print"));
if(!print_fn_name.empty())
{
  std::string pprint_output;
  {
    runtime::scoped_output_redirect const pprint_redirect{...};
    auto const print_fn(__rt_ctx->eval_string("(resolve '" + print_fn_name + ")"));
    if(!print_fn.is_nil())
    {
      runtime::dynamic_call(print_fn, result, jank_nil);
    }
  }
  // Use pprint_output as value
}
```

## Test Case

Test `"eval uses custom print function from nrepl.middleware.print/print"` in `test/cpp/jank/nrepl/engine.cpp`:

1. Requires `cider.nrepl.pprint` namespace
2. Evaluates a large map with nested structures
3. Passes `nrepl.middleware.print/print` = `"cider.nrepl.pprint/pprint"`
4. Verifies output contains newlines (multi-line pretty-printed format)

## Files Modified

- `include/cpp/jank/nrepl_server/ops/eval.hpp` - Print middleware implementation
- `test/cpp/jank/nrepl/engine.cpp` - Test case

## Example Request from CIDER

```
{
  "op": "eval",
  "code": "(repeat 100 300)",
  "nrepl.middleware.print/print": "cider.nrepl.pprint/pprint",
  "nrepl.middleware.print/quota": 1048576,
  "nrepl.middleware.print/buffer-size": 4096
}
```

## Related Files

- `src/jank/clojure/pprint.jank` - pprint implementation
- `src/jank/cider/nrepl/pprint.jank` - CIDER nREPL wrapper
