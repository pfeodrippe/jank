# Simple Pretty Print Implementation for jank

## Summary

Implemented a simple `clojure.pprint` namespace for jank that provides basic pretty printing functionality without relying on protocols or records (which jank doesn't support yet).

## Implementation Approach

Instead of porting fipp (which requires protocols, records, and eduction), we implemented a simpler recursive pretty printer that:

1. Uses type predicate functions (`map?`, `vector?`, `set?`, etc.) for dispatch
2. Tracks indentation level and available width
3. Tries single-line output first, falls back to multi-line if too wide
4. Respects `*print-length*` and `*print-level*` limits

## Key Functions

- `pprint` - Main entry point, prints to `*out*` with newline
- `pprint-str` - Returns pretty-printed string
- `write` - Clojure-compatible write function with keyword options
- `cl-format` - Basic format string support (~A, ~S, ~%, ~~)
- `print-table` - Prints collections of maps as ASCII tables

## Dynamic Vars

- `*print-right-margin*` - Column width for wrapping (default 72)
- `*print-miser-width*` - Threshold for compact printing (default 40)
- `*print-length*` - Max items per collection (nil = unlimited)
- `*print-level*` - Max nesting depth (nil = unlimited)

## Dependencies

- Requires `clojure.string` (for `join`)
- Uses only existing jank features: `defn`, `defn-`, `cond`, `let`, `loop`, `volatile!`, `binding`

## Why Not fipp?

fipp requires features jank doesn't have yet:
- `defprotocol` / `defrecord` - fipp's IVisitor protocol
- `eduction` - core to fipp's streaming pipeline
- `core.rrb-vector` - efficient deque implementation

## File Location

`compiler+runtime/src/jank/clojure/pprint.jank`

## Important jank Limitations Discovered

1. **No docstrings in ns forms** - jank does not support docstrings in namespace declarations. This causes "not nameable" errors.

2. **case with character literals has codegen issues** - Using `case` with character literals (like `\A`, `\a`) causes "Assertion failed! set" codegen errors. Use `cond` with `=` comparisons instead:
   ```clojure
   ;; Bad - causes codegen error
   (case next-c
     (\A \a) (do-something)
     \% (do-other))

   ;; Good - works correctly
   (cond
     (or (= next-c \A) (= next-c \a)) (do-something)
     (= next-c \%) (do-other))
   ```

## Future Improvements

1. Add `*1` support once REPL history is available
2. Support custom writers when jank has writer abstractions
3. More cl-format directives
4. Better width estimation for complex nested structures
