# Runtime Behavior Extensibility Plan for jank

## Problem Statement

Users should be able to extend existing behavior at runtime, similar to how Clojure's `defprotocol`/`deftype`/`defrecord` and libraries like [potemkin](https://github.com/clj-commons/potemkin) allow creating custom map-like types by implementing just 6 core functions (`get`, `assoc`, `dissoc`, `keys`, `meta`, `with-meta`).

Currently, jank's object system uses a **closed enum-based dispatch** where:
- All types are defined in `object_type` enum (75+ variants)
- `visit_object()` has an exhaustive switch over all types
- Behaviors are C++20 concepts checked at compile-time
- No mechanism exists for users to create types that participate in the behavior system at runtime

## Current Architecture Analysis

### Object System

```cpp
// object.hpp - Fixed enum of all types
enum class object_type : u8 {
  nil, boolean, integer, persistent_hash_map, ... // 75+ types
};

// Every object starts with a base field containing its type
struct object { object_type type; };
```

### Behavior System (20 behaviors)

Behaviors are C++20 concepts (compile-time only):
- `object_like` - universal (equal, to_string, to_hash)
- `callable` - functions (call with 0-10 arities)
- `seqable`, `sequenceable`, `countable`, `conjable` - sequences
- `associatively_readable`, `associatively_writable` - maps
- `map_like` - full map protocol
- `indexable`, `stackable`, `comparable` - collections
- `metadatable`, `derefable`, `nameable` - misc

### Dispatch Mechanism

```cpp
// visit.hpp - 144-line switch statement
auto visit_object(F const &fn, object_ref const erased, Args &&...args) {
  switch(erased->type) {
    case object_type::nil:
      return fn(expect_object<obj::nil>(erased), ...);
    case object_type::persistent_hash_map:
      return fn(expect_object<obj::persistent_hash_map>(erased), ...);
    // ... 70+ more cases
  }
}
```

### What Already Exists

1. **`multi_function`** - Runtime polymorphism via value-based dispatch (defmulti/defmethod)
2. **`opaque_box`** - Generic wrapper for C++ objects with limited behavior
3. **`tagged_literal`** - Custom reader-tagged values

### What's Missing (Commented Out in core.jank)

- `defprotocol` - line 7134
- `extend-protocol` / `extend-type` - lines 7137-7187
- `deftype` - line 7498 (deftype Eduction)
- `reify` - lines 7244, 7363, 7548 (future-call, promise, iteration)
- `defrecord` - not found at all

## Design Options

### Option 1: Hybrid Enum + User Type Slot (Recommended)

Add a special `user_defined` enum value that delegates to a vtable-like structure stored in the object itself.

```cpp
// object.hpp
enum class object_type : u8 {
  // ... existing 75 types ...
  user_defined = 255  // Special marker for user-defined types
};

// New user-defined type structure
struct user_type_vtable {
  jtl::immutable_string type_name;

  // Core behaviors as function pointers
  object_ref (*get)(object_ref self, object_ref key);
  object_ref (*get_with_default)(object_ref self, object_ref key, object_ref dflt);
  bool (*contains)(object_ref self, object_ref key);
  object_ref (*assoc)(object_ref self, object_ref key, object_ref val);
  object_ref (*dissoc)(object_ref self, object_ref key);
  object_ref (*seq)(object_ref self);
  i64 (*count)(object_ref self);
  object_ref (*conj)(object_ref self, object_ref val);
  object_ref (*call)(object_ref self, ...); // variadic arities
  object_ref (*with_meta)(object_ref self, object_ref meta);
  object_ref (*meta)(object_ref self);
  // ... other behaviors as needed

  // Behavior flags (which protocols this type implements)
  u32 behavior_flags;  // bitfield: seqable, countable, map_like, etc.
};

struct user_type {
  object base{ object_type::user_defined };
  user_type_vtable const *vtable;
  object_ref data;      // The actual data (usually a map or vector)
  object_ref metadata;
};
```

**Pros:**
- Minimal changes to existing code (just add one case to visit_object)
- Fast path for built-in types unchanged (~10x faster than vtables)
- User types get vtable dispatch (slower but flexible)
- Compatible with existing multimethod system

**Cons:**
- User-defined types are slower than built-in types
- Requires careful design of vtable interface

### Option 2: Protocol Registry with Inline Cache

Create a global protocol registry that maps (type, protocol) -> implementation.

```cpp
struct protocol {
  symbol_ref name;
  persistent_vector_ref method_names;  // [:get :assoc :dissoc ...]
};

// Global registry: {protocol-name -> {type-name -> implementation-map}}
// implementation-map: {method-name -> callable}

// At call sites, use inline caching
struct protocol_call_site {
  object_type cached_type;
  callable_ref cached_impl;
};
```

**Pros:**
- More Clojure-like semantics
- Can extend existing types after the fact
- Inline caching recovers most performance

**Cons:**
- More complex implementation
- Requires modifying many call sites
- Cache invalidation complexity

### Option 3: Metadata-Based Extension (Simplest)

Use metadata to carry behavior implementations, similar to Clojure 1.10's `:extend-via-metadata`.

```clojure
;; User defines a "map-like" value using metadata
(def my-map
  (with-meta {:data {...}}
    {`get (fn [this k] ...)
     `assoc (fn [this k v] ...)
     `dissoc (fn [this k] ...)}))
```

**Pros:**
- Simplest to implement
- Per-value customization
- No changes to type system needed

**Cons:**
- Requires metadata lookup on every operation (slow)
- Not suitable for core protocols (too slow)
- Limited - can't create truly new types

### Option 4: JIT-Generated Types

At runtime, generate actual C++ types using the JIT compiler.

```cpp
// When user calls (deftype MyMap [data] ILookup (...))
// 1. Generate C++ struct definition
// 2. Add new object_type enum value dynamically (not possible with current design)
// 3. JIT compile the type
```

**Pros:**
- Full performance parity with built-in types
- Most powerful approach

**Cons:**
- Cannot extend enum at runtime in C++
- Would require redesigning object_type to not be an enum
- Very complex implementation

## Recommended Approach: Option 1 + Option 3

Implement a **two-tier system**:

### Tier 1: `user_type` for custom types (Option 1)

For `deftype`, `defrecord`, and `reify` - types that need to participate fully in the behavior system.

### Tier 2: Metadata extension (Option 3)

For lightweight, per-value customization using `:extend-via-metadata`.

## Implementation Plan

### Phase 1: Core Infrastructure

1. **Add `user_type` object** (`obj/user_type.hpp`, `obj/user_type.cpp`)
   - Define `user_type_vtable` with function pointers for all behaviors
   - Implement `user_type` struct with vtable pointer, data, metadata
   - Add `object_type::user_defined` to enum
   - Add case to `visit_object()`

2. **Behavior flag system**
   - Define `enum class behavior_flag : u32` bitfield
   - `seqable = 1`, `countable = 2`, `map_like = 4`, `callable = 8`, etc.
   - Runtime check functions: `is_seqable(obj)`, `is_map_like(obj)`, etc.

3. **Update core functions**
   - Modify `get()`, `assoc()`, `seq()`, `count()`, etc. to check for `user_defined` type
   - Dispatch to vtable methods when appropriate

### Phase 2: defprotocol

1. **Protocol representation** (`obj/protocol.hpp`)
   ```cpp
   struct protocol {
     object base{ object_type::protocol };
     symbol_ref name;
     persistent_vector_ref method_sigs;  // [{:name :get :arglists ([this k] [this k default])} ...]
     persistent_hash_map_ref method_vars;  // {:get #'my.ns/get ...}
   };
   ```

2. **Protocol registry**
   - Global atom holding `{protocol-name -> protocol}`
   - `(defprotocol ILookup (get [this k]) (get [this k default]))`

3. **extend-type / extend-protocol**
   - Register implementations in protocol registry
   - Generate vtable entries for user types

### Phase 3: deftype / defrecord

1. **deftype macro**
   ```clojure
   (deftype MyMap [data]
     ILookup
     (get [this k] (get data k))
     (get [this k default] (get data k default))
     IAssociative
     (assoc [this k v] (MyMap. (assoc data k v))))
   ```
   - Generate vtable at compile time
   - Create constructor function `MyMap.`
   - Register type name

2. **defrecord macro**
   - Same as deftype but auto-implements:
     - `ILookup`, `IAssociative` (field access)
     - `IPersistentMap` (map-like behavior)
     - `IObj` (metadata)
     - `equals`, `hashCode`

### Phase 4: reify

1. **reify special form**
   ```clojure
   (reify ILookup
     (get [this k] ...)
     (get [this k default] ...))
   ```
   - Creates anonymous type instance
   - Vtable constructed at creation site
   - Closures capture lexical scope

### Phase 5: Metadata Extension

1. **`:extend-via-metadata` protocol flag**
   ```clojure
   (defprotocol ILookup
     :extend-via-metadata true
     (get [this k]))
   ```

2. **Metadata dispatch**
   - When calling protocol method, check metadata first
   - Fall back to type-based dispatch

## potemkin-style `def-map-type`

With the above infrastructure, implementing `def-map-type` becomes straightforward:

```clojure
(defmacro def-map-type [name fields & body]
  (let [method-impls (parse-body body)]
    `(deftype ~name ~fields
       ILookup
       (get [this# k#] (~(:get method-impls) this# k#))
       (get [this# k# default#] (~(:get method-impls) this# k# default#))

       IAssociative
       (assoc [this# k# v#] (~(:assoc method-impls) this# k# v#))
       (contains [this# k#] (~(:contains method-impls) this# k#))

       IPersistentMap
       (dissoc [this# k#] (~(:dissoc method-impls) this# k#))

       Seqable
       (seq [this#] (seq (~(:keys method-impls) this#)))

       Counted
       (count [this#] (count (~(:keys method-impls) this#)))

       IObj
       (meta [this#] (~(:meta method-impls) this#))
       (with-meta [this# m#] (~(:with-meta method-impls) this# m#)))))
```

Users only implement 6 functions, macro fills in ~20 interface methods.

## Performance Considerations

| Type | Dispatch Cost | Notes |
|------|--------------|-------|
| Built-in (enum) | ~1 ns | Single switch, branch prediction friendly |
| user_type (vtable) | ~5-10 ns | Indirect function call |
| Metadata extension | ~50-100 ns | Hash lookup + indirect call |

The hybrid approach ensures:
- Hot paths (built-in collections) stay fast
- User types have acceptable performance
- Metadata extension for rare cases where per-value behavior is needed

## Behavior Flag Definitions

```cpp
enum class behavior_flag : u32 {
  none           = 0,
  seqable        = 1 << 0,
  sequenceable   = 1 << 1,
  countable      = 1 << 2,
  conjable       = 1 << 3,
  collection     = 1 << 4,  // seqable + conjable + countable
  assoc_readable = 1 << 5,
  assoc_writable = 1 << 6,
  map_like       = 1 << 7,  // assoc_readable + assoc_writable + countable
  indexable      = 1 << 8,
  stackable      = 1 << 9,
  callable       = 1 << 10,
  metadatable    = 1 << 11,
  derefable      = 1 << 12,
  comparable     = 1 << 13,
  nameable       = 1 << 14,
  number_like    = 1 << 15,
  transientable  = 1 << 16,
  sequential     = 1 << 17,
};
```

## Files to Modify

1. `include/cpp/jank/runtime/object.hpp` - Add `user_defined` to enum
2. `include/cpp/jank/runtime/visit.hpp` - Add user_type case
3. `include/cpp/jank/runtime/obj/user_type.hpp` - New file
4. `src/cpp/jank/runtime/obj/user_type.cpp` - New file
5. `include/cpp/jank/runtime/obj/protocol.hpp` - New file
6. `src/cpp/jank/runtime/obj/protocol.cpp` - New file
7. `src/cpp/jank/runtime/core.cpp` - Modify get, assoc, seq, etc.
8. `src/jank/clojure/core.jank` - Uncomment defprotocol, deftype, etc.
9. `src/cpp/jank/analyze/processor.cpp` - Handle deftype, defrecord, reify special forms

## Open Questions

1. **Type identity**: How to handle `(instance? MyType x)` checks for user types?
2. **Hierarchy**: Should user types participate in `derive` / `isa?` hierarchies?
3. **Equality**: Default vs custom equality semantics for deftype vs defrecord
4. **Printing**: How to customize `print-method` for user types?
5. **Type metadata**: Should user types support type hints / reflection?

## References

- [Clojure Protocols](https://clojure.org/reference/protocols)
- [potemkin def-map-type](https://github.com/clj-commons/potemkin)
- [Rust enum dispatch vs trait objects](https://docs.rs/enum_dispatch/latest/enum_dispatch/) - ~10x speedup for enum over vtable
- [Expression Problem](https://eli.thegreenplace.net/2016/the-expression-problem-and-its-solutions/)

---

## Focused Implementation TODO (Option 1 Only)

**Goal**: Implement minimal `user_type` infrastructure. Users can build `def-map-type`, `defrecord`, etc. in pure jank on top.

### Step 1: Define `user_type_vtable` and `user_type`

**File**: `include/cpp/jank/runtime/obj/user_type.hpp`

```cpp
namespace jank::runtime::obj
{
  // Function pointer types for each behavior
  using get_fn = object_ref (*)(object_ref self, object_ref key);
  using get_default_fn = object_ref (*)(object_ref self, object_ref key, object_ref dflt);
  using contains_fn = native_bool (*)(object_ref self, object_ref key);
  using assoc_fn = object_ref (*)(object_ref self, object_ref key, object_ref val);
  using dissoc_fn = object_ref (*)(object_ref self, object_ref key);
  using seq_fn = object_ref (*)(object_ref self);
  using count_fn = i64 (*)(object_ref self);
  using conj_fn = object_ref (*)(object_ref self, object_ref val);
  using call_fn = object_ref (*)(object_ref self, object_ref args); // args is a seq
  using with_meta_fn = object_ref (*)(object_ref self, object_ref meta);
  using get_meta_fn = object_ref (*)(object_ref self);
  using to_string_fn = jtl::immutable_string (*)(object_ref self);
  using to_hash_fn = i64 (*)(object_ref self);
  using equal_fn = native_bool (*)(object_ref self, object_ref other);

  struct user_type_vtable
  {
    jtl::immutable_string type_name;

    // Behavior function pointers (nullptr if not implemented)
    get_fn get{ nullptr };
    get_default_fn get_default{ nullptr };
    contains_fn contains{ nullptr };
    assoc_fn assoc{ nullptr };
    dissoc_fn dissoc{ nullptr };
    seq_fn seq{ nullptr };
    count_fn count{ nullptr };
    conj_fn conj{ nullptr };
    call_fn call{ nullptr };
    with_meta_fn with_meta{ nullptr };
    get_meta_fn get_meta{ nullptr };
    to_string_fn to_string{ nullptr };
    to_hash_fn to_hash{ nullptr };
    equal_fn equal{ nullptr };

    // Behavior flags
    u32 behavior_flags{ 0 };
  };

  struct user_type : gc
  {
    static constexpr object_type obj_type{ object_type::user_type };
    static constexpr bool pointer_free{ false };

    object base{ obj_type };
    user_type_vtable const *vtable;
    object_ref data;      // User's data (typically a map or vector)
    object_ref metadata;

    // object_like
    native_bool equal(object const &other) const;
    jtl::immutable_string to_string() const;
    void to_string(jtl::string_builder &) const;
    jtl::immutable_string to_code_string() const;
    i64 to_hash() const;
  };
}
```

### Step 2: Add to `object_type` enum

**File**: `include/cpp/jank/runtime/object.hpp`

```cpp
enum class object_type : u8
{
  // ... existing types ...
  opaque_box,
  user_type,  // <-- ADD THIS
};
```

### Step 3: Add case to `visit_object`

**File**: `include/cpp/jank/runtime/visit.hpp`

```cpp
case object_type::user_type:
  return fn(expect_object<obj::user_type>(erased), std::forward<Args>(args)...);
```

### Step 4: Update core runtime functions

**File**: `src/cpp/jank/runtime/core.cpp`

For each behavior function (`get`, `assoc`, `seq`, `count`, etc.), add a check:

```cpp
object_ref get(object_ref coll, object_ref key)
{
  // Check for user_type first (fast path for built-in still via visit_object)
  if(coll->type == object_type::user_type)
  {
    auto const ut = expect_object<obj::user_type>(coll);
    if(ut->vtable->get)
    { return ut->vtable->get(coll, key); }
    throw std::runtime_error("user_type does not implement get");
  }

  return visit_object(
    [&](auto const typed_coll) -> object_ref { ... },
    coll
  );
}
```

### Step 5: Create jank API for `user_type`

**File**: `src/jank/clojure/core.jank`

```clojure
;; Create a user type with a vtable
;; vtable is a map of keyword -> function
;; e.g., {:get (fn [self k] ...) :assoc (fn [self k v] ...) ...}
(defn make-user-type
  "Creates a user-defined type instance.
   type-name: string name for the type
   vtable: map of behavior keywords to implementing functions
   data: the underlying data for this instance"
  [type-name vtable data]
  (cpp/jank.runtime.make_user_type type-name vtable data))

;; Example usage by library authors:
(defn my-lazy-map [thunk]
  (make-user-type
    "MyLazyMap"
    {:get (fn [self k] (get (force (:data self)) k))
     :assoc (fn [self k v] (my-lazy-map (delay (assoc (force (:data self)) k v))))
     :dissoc (fn [self k] (my-lazy-map (delay (dissoc (force (:data self)) k))))
     :seq (fn [self] (seq (force (:data self))))
     :count (fn [self] (count (force (:data self))))}
    {:data (delay (thunk))}))
```

### Step 6: C++ helper for creating user_type from jank

**File**: `src/cpp/jank/runtime/obj/user_type.cpp`

```cpp
// Called from jank: (cpp/jank.runtime.make_user_type name vtable data)
object_ref make_user_type(object_ref name, object_ref vtable_map, object_ref data)
{
  auto vt = new (GC) user_type_vtable{};
  vt->type_name = expect_object<obj::persistent_string>(name)->data;

  // Extract function pointers from vtable_map
  // Each function is wrapped to call back into jank
  auto const vm = expect_object<obj::persistent_hash_map>(vtable_map);

  if(auto get_fn = vm->get(make_keyword("get")); get_fn != obj::nil::nil_const())
  {
    vt->get = [](object_ref self, object_ref key) -> object_ref {
      auto ut = expect_object<obj::user_type>(self);
      auto fn = ut->vtable->_get_impl;  // stored callable
      return dynamic_call(fn, self, key);
    };
    vt->_get_impl = get_fn;  // Store the jank function
    vt->behavior_flags |= behavior_flag::assoc_readable;
  }
  // ... similar for other behaviors ...

  return make_box<obj::user_type>(vt, data, obj::nil::nil_const());
}
```

### Minimal Files to Create/Modify

| File | Action |
|------|--------|
| `include/cpp/jank/runtime/object.hpp` | Add `user_type` to enum |
| `include/cpp/jank/runtime/obj/user_type.hpp` | **NEW** - vtable + user_type struct |
| `src/cpp/jank/runtime/obj/user_type.cpp` | **NEW** - implementation |
| `include/cpp/jank/runtime/visit.hpp` | Add user_type case |
| `src/cpp/jank/runtime/core.cpp` | Check for user_type in get/assoc/seq/etc |
| `src/jank/clojure/core.jank` | Add `make-user-type` function |

### What Users Can Build On Top

With just `make-user-type`, library authors can implement:

1. **def-map-type** (potemkin-style) - macro that generates vtable from 6 functions
2. **defrecord** - macro that creates map-like type with named fields
3. **deftype** - macro for arbitrary behavior implementations
4. **reify** - anonymous type instances with closures

Example user-land `def-map-type`:

```clojure
(defmacro def-map-type [name [& fields] & {:keys [get assoc dissoc keys meta with-meta]}]
  `(defn ~(symbol (str name ".")) [~@fields]
     (make-user-type
       ~(str name)
       {:get ~get
        :assoc ~assoc
        :dissoc ~dissoc
        :seq (fn [self#] (seq (~keys self#)))
        :count (fn [self#] (count (~keys self#)))
        :meta ~(or meta `(fn [self#] (:__meta__ (:data self#))))
        :with-meta ~(or with-meta `(fn [self# m#]
                                      (~(symbol (str name "."))
                                        (assoc (:data self#) :__meta__ m#))))}
       {~@(mapcat (fn [f] [(keyword f) f]) fields)})))
```

### Testing Checklist

- [ ] Create user_type with map-like vtable
- [ ] `(get my-user-type :key)` dispatches to vtable
- [ ] `(assoc my-user-type :key val)` returns new user_type
- [ ] `(seq my-user-type)` works
- [ ] `(count my-user-type)` works
- [ ] User type prints with custom type name
- [ ] User type participates in equality
- [ ] Performance: built-in types not affected
