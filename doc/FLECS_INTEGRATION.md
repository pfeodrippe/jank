# Flecs Integration with jank

**Guide to integrating the Flecs ECS library with jank**

---

## Important: Why a Dynamic Library is Required

Flecs is written in pure C code and uses C-specific features (like `and` and `not` as variable names, implicit void* casts) that are not valid in C++. Since jank's `cpp/raw` compiles as C++, you cannot directly include `flecs.c`.

**Solution**: Pre-compile flecs as a dynamic library (`.dylib` on macOS, `.so` on Linux), then link against it at runtime.

---

## Quick Start (Working Example)

### 1. Build flecs as a shared library

```bash
cd vendor/flecs/distr
clang -shared -fPIC -o libflecs.dylib flecs.c  # macOS
# or: gcc -shared -fPIC -o libflecs.so flecs.c  # Linux
```

### 2. Create your jank file

`src/my_flecs.jank`:
```clojure
(ns my-flecs)

;; Include flecs header + minimal helper
(cpp/raw "
#define FLECS_NO_CPP
#include \"flecs.h\"
#include <jank/runtime/obj/opaque_box.hpp>

// Get world pointer from opaque box
inline ecs_world_t* get_world(jank::runtime::object_ref box) {
  auto opaque = jank::runtime::expect_object<jank::runtime::obj::opaque_box>(box);
  return static_cast<ecs_world_t*>(opaque->data.data);
}
")

;; ============================================================
;; Flecs API - thin wrappers calling C directly
;; ============================================================

(defn create-world []
  (cpp/box (cpp/ecs_init)))

(defn destroy-world! [world]
  (cpp/ecs_fini (cpp/get_world world)))

(defn create-entity [world]
  (cpp/ecs_new (cpp/get_world world)))

(defn progress! [world dt]
  (cpp/ecs_progress (cpp/get_world world) dt))

;; ============================================================
;; Main
;; ============================================================

(defn -main [& args]
  (let [world (create-world)]
    (println "Flecs world created!")
    (println "World:" world)

    (let [entity (create-entity world)]
      (println "Entity created:" entity))

    (progress! world 0.016)
    (println "World progressed!")

    (destroy-world! world)
    (println "Flecs world destroyed!")))
```

This approach uses:
- **Minimal cpp/raw**: Only a `get_world` helper to extract the C pointer
- **Thin jank wrappers**: Call flecs C functions directly via `cpp/ecs_*`
- **`cpp/box`**: Automatically wraps C pointers in opaque_box

### 3. Run it

```bash
jank -I./vendor/flecs/distr \
     -l/full/path/to/vendor/flecs/distr/libflecs.dylib \
     --module-path src \
     run-main my-flecs -main
```

Output:
```
Flecs world created!
World: #object [opaque_box 0x...]
Entity created: 552
World progressed!
Flecs world destroyed!
```

---

## Key Concepts

### 1. cpp/box - Automatic Pointer Wrapping

Use `cpp/box` to wrap C pointers returned by functions:

```clojure
(defn create-world []
  (cpp/box (cpp/ecs_init)))  ; Wraps ecs_world_t* in opaque_box
```

This is equivalent to the verbose C++:
```cpp
jank::runtime::make_box<jank::runtime::obj::opaque_box>(ecs_init(), "ecs_world_t *");
```

### 2. The get_world Helper Pattern

For functions that need the raw C pointer, define a minimal helper in cpp/raw:

```cpp
inline ecs_world_t* get_world(jank::runtime::object_ref box) {
  auto opaque = jank::runtime::expect_object<jank::runtime::obj::opaque_box>(box);
  return static_cast<ecs_world_t*>(opaque->data.data);
}
```

Key points:
- `jank::runtime::object_ref` - Universal reference to any jank object
- `jank::runtime::obj::opaque_box` - Wrapper for opaque C/C++ pointers
- `opaque->data.data` - Extract raw `void*` (note: `.data.data` because `data` is a `jtl::ptr<void>`)

### 3. Direct C Function Calls

Once you have the helper, call C functions directly:

```clojure
(defn destroy-world! [world]
  (cpp/ecs_fini (cpp/get_world world)))  ; Call C function directly!
```

### 4. FLECS_NO_CPP

Always define `FLECS_NO_CPP` before including `flecs.h` when using jank. This disables flecs' C++ template features that would conflict with jank's C++ environment.

---

## Project Structure

```
my-project/
├── vendor/
│   └── flecs/
│       └── distr/
│           ├── flecs.h         # Header (included via cpp/raw)
│           ├── flecs.c         # Source (compiled to dylib)
│           └── libflecs.dylib  # Pre-compiled library
└── src/
    └── my_flecs.jank           # Your jank code
```

---

## Adding More Flecs Functions

With the thin wrapper approach, adding new flecs functions is simple:

```clojure
;; Most flecs functions can be called directly - just add a jank wrapper:

(defn delete-entity! [world entity]
  (cpp/ecs_delete (cpp/get_world world) entity))

(defn is-alive? [world entity]
  (cpp/ecs_is_alive (cpp/get_world world) entity))

(defn set-name! [world entity name]
  (cpp/ecs_set_name (cpp/get_world world) entity name))

(defn get-name [world entity]
  (cpp/ecs_get_name (cpp/get_world world) entity))
```

For complex operations that need C++ logic, add a helper to cpp/raw:

```cpp
// In cpp/raw block - only when you need C++ logic:
inline ecs_entity_t create_named_entity(jank::runtime::object_ref world_box,
                                        char const* name) {
  auto world = get_world(world_box);
  return ecs_entity(world, { .name = name });
}
```

Then call it from jank:
```clojure
(defn create-named-entity [world name]
  (cpp/create_named_entity world name))
```

---

## Troubleshooting

### "Symbols not found: _ecs_init, _ecs_fini"
The library isn't being loaded. Make sure:
- Use `-l/full/absolute/path/to/libflecs.dylib`
- The dylib exists and is readable

### "no matching function for call"
Type mismatch between jank and C++. Check:
- Use `object_ref` for jank objects
- Use `opaque->data.data` (not just `opaque->data`) to get raw `void*`

### "FLECS_NO_CPP failed: CPP is required"
Some flecs addon requires C++. You may need to disable specific addons or restructure your code.

---

## Why Not Static Linking?

jank uses LLVM JIT which requires symbols to be resolvable at runtime via `dlopen()`. Static libraries (`.a` files) cannot be loaded this way. Options:
1. Use dynamic libraries (recommended for JIT)
2. Use AOT compilation for full static linking

---

## Using the Flecs C++ API

For the full power of Flecs, you can use the C++ API instead of the C API. **Don't define `FLECS_NO_CPP`** - we want the templates!

### C++ Components and Systems

```clojure
(ns my-flecs-cpp)

(cpp/raw "
#include \"flecs.h\"
#include <jank/runtime/obj/opaque_box.hpp>

// ============================================================
// Components - C++ structs become ECS components!
// ============================================================
struct Position { float x, y; };
struct Velocity { float x, y; };

// ============================================================
// World wrapper (heap-allocated for persistence)
// ============================================================
inline flecs::world* get_world(jank::runtime::object_ref box) {
  auto opaque = jank::runtime::expect_object<jank::runtime::obj::opaque_box>(box);
  return static_cast<flecs::world*>(opaque->data.data);
}

inline jank::runtime::object_ref flecs_create_world() {
  auto* world = new flecs::world();
  return jank::runtime::make_box<jank::runtime::obj::opaque_box>(
    static_cast<void*>(world), \"flecs::world\");
}

inline void flecs_destroy_world(jank::runtime::object_ref world_box) {
  delete get_world(world_box);
}

// ============================================================
// Entity with components - the C++ way!
// ============================================================
inline uint64_t flecs_create_moving_entity(jank::runtime::object_ref world_box,
                                            float px, float py, float vx, float vy) {
  return get_world(world_box)->entity()
    .set<Position>({px, py})   // Template magic!
    .set<Velocity>({vx, vy})
    .id();
}

// ============================================================
// System with lambda - define behavior in C++!
// ============================================================
inline void flecs_register_move_system(jank::runtime::object_ref world_box) {
  get_world(world_box)->system<Position, const Velocity>(\"Move\")
    .each([](Position& p, const Velocity& v) {
      p.x += v.x;
      p.y += v.y;
    });
}

inline bool flecs_progress(jank::runtime::object_ref world_box, float dt) {
  return get_world(world_box)->progress(dt);
}
")

;; Jank wrappers
(defn create-world [] (cpp/flecs_create_world))
(defn destroy-world! [world] (cpp/flecs_destroy_world world))
(defn create-moving-entity [world px py vx vy]
  (cpp/flecs_create_moving_entity world px py vx vy))
(defn register-move-system! [world] (cpp/flecs_register_move_system world))
(defn progress! [world dt] (cpp/flecs_progress world dt))

(defn -main [& args]
  (let [world (create-world)]
    (let [entity (create-moving-entity world 0.0 0.0 1.0 2.0)]
      (register-move-system! world)
      (dotimes [_ 5]
        (progress! world 1.0)
        (println "Position updates via C++ system!")))
    (destroy-world! world)))
```

### Key Differences from C API

| Feature | C API | C++ API |
|---------|-------|---------|
| World type | `ecs_world_t*` | `flecs::world*` (heap-allocated) |
| Components | Manual registration | Automatic via templates |
| Entity creation | `ecs_new()` | `world->entity().set<T>({...})` |
| Systems | Function pointers | C++ lambdas with type safety |
| Queries | String-based | Template-based |

### Why Heap-Allocate flecs::world?

`flecs::world` is a C++ object that manages its own lifetime via RAII. Since jank needs the world to persist across function calls, we heap-allocate it with `new` and store the pointer in an opaque_box.
