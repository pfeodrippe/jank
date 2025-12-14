# Namespace Autocompletion in Require Forms

## Summary

Added namespace autocompletion for `require` forms in the nREPL engine. When a user is typing in a `:require` context (e.g., `(:require [something.ab|])`), the completion system now returns available namespace names instead of vars.

## Implementation

### Key Files Modified

1. **`include/cpp/jank/nrepl_server/engine.hpp`**
   - Added `collect_available_namespaces(prefix)` - collects namespace names from:
     - Loaded namespaces via `__rt_ctx->namespaces`
     - Available modules on module path via `__rt_ctx->module_loader.entries`
   - Added `is_require_context(context)` - detects if the completion context contains `:require` or `(require` patterns
   - Added `make_namespace_candidates(prefix)` - creates completion candidates with type "namespace"

2. **`include/cpp/jank/nrepl_server/ops/completions.hpp`**
   - Modified `handle_completions` to check for require context and return namespace completions

3. **`include/cpp/jank/nrepl_server/ops/complete.hpp`**
   - Modified `handle_complete` to check for require context and return namespace completions

### How It Works

The nREPL `complete` and `completions` operations now accept a `context` field in the message. This field contains surrounding code that helps identify what kind of completion is needed.

When `context` contains patterns like `:require`, `(require '`, or `(require\n`, the system:
1. Collects all available namespace names (both loaded and on module path)
2. Filters by the given prefix
3. Returns completions with type "namespace"

### Test Cases Added

- `completions returns namespaces in require context`
- `complete returns namespaces in require context`
- `completions without require context returns vars not namespaces`

## Usage

Editors like CIDER send a `context` field with completion requests. When typing:

```clojure
(ns my-app
  (:require [sample.|]))
```

The editor sends a completion request with:
- `prefix`: "sample."
- `context`: contains ":require ["

The system returns matching namespaces like `sample.server`, `sample.client`.
