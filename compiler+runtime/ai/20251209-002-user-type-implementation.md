# User-Defined Types (user_type) Implementation

## Summary

Implemented runtime behavior extensibility for jank, similar to Clojure's potemkin `def-map-type`. Users can now create custom types with map-like behaviors by providing function implementations in a vtable.

## Files Modified

### New Files
- `include/cpp/jank/runtime/obj/user_type.hpp` - Header with vtable struct and user_type class
- `src/cpp/jank/runtime/obj/user_type.cpp` - Implementation (~400 lines)

### Modified Files
- `include/cpp/jank/runtime/object.hpp` - Added `user_type` to `object_type` enum
- `include/cpp/jank/runtime/visit.hpp` - Added user_type case to `visit_object`
- `include/cpp/jank/runtime/core.hpp` - Added `make_user_type` declaration
- `src/cpp/jank/runtime/core.cpp` - Added `make_user_type` wrapper implementation
- `src/jank/clojure/core.jank` - Added `make-user-type` function
- `CMakeLists.txt` - Added user_type.cpp to build

## Design

### Key Components

1. **behavior_flag enum** - Bitfield for tracking implemented behaviors:
   - seqable, sequenceable, countable, conjable
   - assoc_readable, assoc_writable, map_like
   - callable, metadatable

2. **user_type_vtable struct** - Stores jank function pointers:
   - get_fn, get_entry_fn, contains_fn
   - assoc_fn, dissoc_fn, seq_fn
   - count_fn, conj_fn, call_fn
   - with_meta_fn, meta_fn
   - to_string_fn, to_hash_fn, equal_fn

3. **user_type class** - The runtime type with:
   - Pointer to vtable
   - User data (object_ref)
   - Optional metadata

### Important Technical Decisions

1. **user_type does NOT satisfy C++ seqable concept** - The `seq()` function would return `object_ref` (type-erased), but jank's seqable concept expects typed returns. Code in seq.cpp calls `ret->first()` which fails on type-erased pointers. Solution: Removed seq()/fresh_seq() from user_type entirely.

2. **Callable support up to 10 args** - Uses `dynamic_call` for 0-9 args, `apply_to` with a vector for 10 args (since dynamic_call only supports 11 params including the function).

3. **vtable is GC-managed** - The vtable struct inherits from `gc` to allow GC of stored jank functions.

## Usage Example

```clojure
(make-user-type "MyMap"
                {:get (fn [this k] (get (:data this) k))
                 :assoc (fn [this k v]
                          (make-user-type "MyMap" {}
                                         (assoc (:data this) k v)))
                 :count (fn [this] (count (:data this)))}
                {:data {}})
```

## Supported Behaviors

| Keyword | Signature | Description |
|---------|-----------|-------------|
| :get | (fn [this key]) | Lookup by key |
| :get-entry | (fn [this key]) | Returns [key value] or nil |
| :contains | (fn [this key]) | Returns true if key exists |
| :assoc | (fn [this key val]) | Returns new instance with key=val |
| :dissoc | (fn [this key]) | Returns new instance without key |
| :seq | (fn [this]) | Returns seq of entries |
| :count | (fn [this]) | Returns element count |
| :conj | (fn [this val]) | Returns new instance with val added |
| :call | (fn [this & args]) | Makes type callable as function |
| :with-meta | (fn [this meta]) | Returns new instance with metadata |
| :meta | (fn [this]) | Returns metadata |
| :to-string | (fn [this]) | Returns string representation |
| :to-hash | (fn [this]) | Returns hash code |
| :equal | (fn [this other]) | Returns true if equal |

## Errors Fixed During Implementation

1. `native_bool` unknown - Use `bool` instead (pattern from nil.hpp)
2. `fmt::ptr(&base)` - Use `&base` directly (pattern from atom.cpp)
3. `dynamic_call` 12 args - Use `apply_to` for 10-arg call case
4. visit_seqable issues - Removed user_type from visit_seqable
5. `runtime::is_nil` not found - Use `tag_val.is_nil()` member method
