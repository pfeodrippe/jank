# Fix: Var definition location now available in nREPL info

## Issue

jank vars were not showing definition location in nREPL. The user reported:
```
my-integrated-demo/draw-imgui-panel!
[world]
  Not documented.

Definition location unavailable.
```

Additionally, after an initial fix, the file path was incorrectly showing `clojure/core.jank` instead of the actual file where `defn` was called. This was because macro-expanded `def` forms have source location pointing to the macro definition, not the call site.

## Root Cause

1. In `analyze_def` (processor.cpp), when a `def` expression is analyzed, the symbol's metadata was copied but file/line/column source location was never added.

2. For macros like `defn`, the expanded `def` form's metadata points to `clojure/core.jank` (where `defn` is defined) rather than the file where `defn` was called.

## Fix

Added source location metadata to vars in `analyze_def` at line ~1374. The fix checks for macro expansions first to get the original source location (from the `defn` call), falling back to the `def` form's metadata for direct `def` calls:

```cpp
/* Add source location (file, line, column) to var metadata.
 * If this def came from a macro expansion (like defn), use the original source location.
 * Otherwise, use the def form's source location. */
auto def_source(read::source::unknown);
auto const expansion(latest_expansion(macro_expansions));
if(expansion != runtime::jank_nil)
{
  def_source = object_source(expansion);
}
if(def_source == read::source::unknown || def_source.file == read::no_source_path)
{
  def_source = meta_source(l->meta);
}
if(def_source != read::source::unknown && def_source.file != read::no_source_path)
{
  auto meta(qualified_sym->meta.unwrap_or(runtime::jank_nil));
  meta = runtime::assoc(
    meta,
    __rt_ctx->intern_keyword("file").expect_ok(),
    runtime::make_box<runtime::obj::persistent_string>(def_source.file));
  meta = runtime::assoc(
    meta,
    __rt_ctx->intern_keyword("line").expect_ok(),
    runtime::make_box<runtime::obj::integer>(static_cast<i64>(def_source.start.line)));
  meta = runtime::assoc(
    meta,
    __rt_ctx->intern_keyword("column").expect_ok(),
    runtime::make_box<runtime::obj::integer>(static_cast<i64>(def_source.start.col)));
  qualified_sym = qualified_sym->with_meta(meta);
}
```

## Key Insight

The `macro_expansions` vector tracks the original forms before macro expansion. `latest_expansion()` returns the original `defn` call form, which has the correct source location (the user's file). This is preferred over `l->meta` which points to the macro definition file.

## Changed Files

- `src/cpp/jank/analyze/processor.cpp` (~line 1374): Added source location to var metadata with macro expansion handling
- `test/cpp/jank/nrepl/engine.cpp`: Added test assertions for line/column in info response

## Test Results

All 190 jank tests pass with 2279 assertions.
