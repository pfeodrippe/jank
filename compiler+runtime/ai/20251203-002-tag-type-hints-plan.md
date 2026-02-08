# Plan: Implement `:tag` Type Hints in jank

## Problem Statement

Currently, jank has metadata infrastructure but doesn't use `:tag` metadata for compile-time type inference. Users want:

```clojure
(defonce ^{:tag :bool} is-paused (v->p false))
(p->v is-paused)  ; Should know type is :bool at compile time
```

Or with shorthand:
```clojure
(defonce ^:bool is-paused (v->p false))
```

## Current State in jank

### What EXISTS:
1. **Metadata on vars** - `var.hpp:74` has `jtl::option<object_ref> meta`
2. **Metadata on symbols** - Symbols support metadata via `with_meta()`
3. **Local binding type tracking** - `local_frame.hpp:61` has `jtl::ptr<void> type`
4. **Type inference from expressions** - `cpp_util::expression_type()` infers C++ types
5. **`:rettag` in defn** - `core.jank:666` passes `:tag` to function as `:rettag`

### What's MISSING:
1. **No `:tag` extraction** - Analyzer doesn't read `:tag` from var/symbol metadata
2. **No `:tag` to type conversion** - No mapping from `:tag` values to C++ types
3. **No propagation to usages** - When referencing a var, `:tag` isn't consulted
4. **`:rettag` unused** - Functions store it but never apply it

## Implementation Plan

### Phase 1: Tag-to-Type Mapping Function

**File:** `src/cpp/jank/analyze/cpp_util.cpp` (new function)

```cpp
// Convert :tag metadata to C++ type pointer
jtl::ptr<void> tag_to_cpp_type(runtime::object_ref tag) {
  if(tag == runtime::jank_nil) return untyped_object_ptr_type();

  // Handle keyword tags like :bool, :i32, :f64
  if(auto kw = runtime::dyn_cast<runtime::obj::keyword>(tag)) {
    auto const name = kw->name;
    if(name == "bool" || name == "boolean") return /* bool* type */;
    if(name == "i32" || name == "int") return /* int* type */;
    if(name == "i64" || name == "long") return /* long* type */;
    if(name == "f32" || name == "float") return /* float* type */;
    if(name == "f64" || name == "double") return /* double* type */;
    // ... more types
  }

  // Handle string tags for C++ types
  if(auto str = runtime::dyn_cast<runtime::obj::persistent_string>(tag)) {
    // Use CppInterOp to look up the type
  }

  return untyped_object_ptr_type();
}
```

### Phase 2: Extract `:tag` from Var Metadata

**File:** `src/cpp/jank/analyze/processor.cpp`

**Location:** `analyze_symbol()` function (~line 1564)

When analyzing a symbol that resolves to a var, check for `:tag` metadata:

```cpp
// In analyze_symbol, after resolving to a var:
if(var->meta.is_some()) {
  auto const tag = runtime::get(var->meta.unwrap(),
                                __rt_ctx->intern_keyword("tag").expect_ok());
  if(tag != runtime::jank_nil) {
    // Store tag info for later use
    // This affects the expression's inferred type
  }
}
```

### Phase 3: Apply `:tag` to `var_deref` Expression Type

**File:** `include/cpp/jank/analyze/expr/var_deref.hpp`

Add a type field to `var_deref`:

```cpp
struct var_deref : expression {
  // existing fields...
  jtl::ptr<void> tag_type{ nullptr };  // NEW: type from :tag metadata
};
```

**File:** `src/cpp/jank/analyze/processor.cpp`

When creating `var_deref`, populate `tag_type`:

```cpp
auto const deref = make_box<expr::var_deref>(
  expression_position{current_frame},
  qualified_sym,
  found_var,
  current_frame
);

// NEW: Check for :tag metadata
if(found_var->meta.is_some()) {
  auto const tag = runtime::get(found_var->meta.unwrap(), tag_kw);
  if(tag != runtime::jank_nil) {
    deref->tag_type = cpp_util::tag_to_cpp_type(tag);
  }
}
```

### Phase 4: Propagate to Local Bindings

**File:** `src/cpp/jank/analyze/processor.cpp`

**Location:** `analyze_let()` (~line 2320)

When a local binding is initialized from a var with `:tag`, use the tag type:

```cpp
// When creating local_binding, check if value_expr has tag_type
auto expr_type = cpp_util::non_void_expression_type(it.second);

// If it's a var_deref with tag_type, prefer that
if(auto vd = expression_cast<expr::var_deref>(it.second)) {
  if(vd->tag_type) {
    expr_type = vd->tag_type;
  }
}
```

### Phase 5: Support Symbol-Level `:tag`

For direct symbol metadata like `^:bool is-paused`:

**File:** `src/cpp/jank/analyze/processor.cpp`

When analyzing a symbol with metadata, check for `:tag`:

```cpp
// In analyze_symbol
if(sym->meta.is_some()) {
  auto const tag = runtime::get(sym->meta.unwrap(), tag_kw);
  // Use this tag for type inference
}
```

### Phase 6: Macro Access to `:tag`

For macros like `p->v` to access `:tag` at expansion time:

**Option A:** Use `resolve` + `meta` (current approach, works):
```clojure
(defmacro p->v [p]
  (let [tag (:tag (meta (resolve p)))]
    ...))
```

**Option B:** Analyzer provides type info to macros (more complex):
- Would require new special form or compiler hook

## Files to Modify

1. `include/cpp/jank/analyze/cpp_util.hpp` - Add `tag_to_cpp_type()` declaration
2. `src/cpp/jank/analyze/cpp_util.cpp` - Implement `tag_to_cpp_type()`
3. `include/cpp/jank/analyze/expr/var_deref.hpp` - Add `tag_type` field
4. `src/cpp/jank/analyze/processor.cpp` - Multiple changes:
   - `analyze_def()` - Ensure `:tag` is preserved in var metadata
   - `analyze_symbol()` - Extract `:tag` when resolving vars
   - `analyze_let()` - Propagate tag types to local bindings

## Testing Strategy

1. **Unit test:** `:tag` metadata is preserved through `def`
2. **Unit test:** `tag_to_cpp_type()` correctly maps keywords to types
3. **Unit test:** `var_deref` expression has correct `tag_type`
4. **Integration test:** Macro can access `:tag` via `(meta (resolve sym))`

## Example Usage After Implementation

```clojure
;; Define with type hint
(defonce ^:bool is-paused (v->p false))

;; Macro can infer type at expansion time
(p->v is-paused)  ; Expands with correct bool* type

;; Also works with explicit schema
(defonce ^{:tag [:* :bool]} is-paused-ptr (v->p false))
```

## Priority

**Phase 1-2:** Essential for basic `:tag` support
**Phase 3-4:** Needed for type propagation
**Phase 5:** Nice-to-have for symbol-level hints
**Phase 6:** Depends on use case (macro vs compiler support)

## Alternative: Pure Macro Solution (Simpler)

If compiler changes are too invasive, the workaround is:

```clojure
;; User annotates var with :tag
(defonce ^{:tag :bool} is-paused (v->p false))

;; p->v macro looks up var metadata
(defmacro p->v [p]
  (let [tag (:tag (meta (resolve p)))]
    `(cpp/unbox (cpp/type ~(->cpp-type tag)) ~p)))
```

This works TODAY without compiler changes, but requires:
1. User to add `^{:tag :type}` on the def
2. The var to exist before `p->v` is expanded
