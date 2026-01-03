# iOS AOT Module Loading Order

When loading jank modules for iOS AOT, they must be loaded in dependency order.

## Critical Loading Order

```cpp
// Core jank modules
extern "C" void* jank_load_clojure_core_native();
extern "C" void* jank_load_core();

// Additional clojure.* modules (required by vybe.util)
extern "C" void* jank_load_string();  // clojure.string
extern "C" void* jank_load_set();     // clojure.set
extern "C" void* jank_load_walk();    // clojure.walk

// Application modules in dependency order
extern "C" void* jank_load_vybe_util();
extern "C" void* jank_load_vybe_sdf_math();
extern "C" void* jank_load_vybe_sdf_state();
extern "C" void* jank_load_vybe_sdf_shader();
extern "C" void* jank_load_vybe_sdf_ui();
extern "C" void* jank_load_vybe_sdf_ios();
```

## Why Order Matters

- Each module's generated C++ references vars from its dependencies
- When `jank_load_<module>()` is called, it binds functions to vars
- If a dependency module hasn't been loaded, its vars won't have values
- Calling `intern_var()` creates the var but leaves it unbound
- Calling an unbound var throws an exception

## Symbol Naming

- `clojure.core` → `jank_load_core`, `jank_load_clojure_core_native`
- `clojure.string` → `jank_load_string` (prefix dropped)
- `clojure.set` → `jank_load_set`
- `vybe.util` → `jank_load_vybe_util`
- `vybe.sdf.ios` → `jank_load_vybe_sdf_ios`

## Object Files to Link

All modules need their corresponding `.o` files linked:
- `clojure_core_generated.o`
- `clojure_string_generated.o`
- `clojure_set_generated.o`
- `clojure_walk_generated.o`
- `vybe_util_generated.o`
- `vybe_sdf_math_generated.o`
- `vybe_sdf_state_generated.o`
- `vybe_sdf_shader_generated.o`
- `vybe_sdf_ui_generated.o`
- `vybe_sdf_ios_generated.o`
