# iOS JIT nREPL Module Loading Investigation

## Problem
iOS JIT app fails with "Unable to find module 'jank.nrepl-server.asio'" even though:
- The symbol `_jank_load_jank_nrepl_server_asio` IS in the binary
- `jank_ios_register_native_modules()` is called and calls `jank_module_set_loaded()`

## Investigation

### Flow Analysis

1. **jank_init_with_pch()** creates the runtime context
2. **jank_ios_register_native_modules()** is called which:
   - Calls `jank_load_jank_nrepl_server_asio()`
   - Calls `jank_module_set_loaded("/jank.nrepl-server.asio")`
3. Main function runs, loads clojure.core, then vybe.sdf.ios

### Key Discovery - Symbol Format Mismatch!

**In Clojure's load-lib (core.jank:4508):**
```clojure
loaded? (contains? @*loaded-libs* lib)
```
Where `lib` is a symbol like `jank.nrepl-server.asio` (NO leading slash)

**In C++ set_is_loaded (loader.cpp:836-837):**
```cpp
return runtime::try_object<runtime::obj::persistent_sorted_set>(curr_val)->conj(
  make_box<obj::symbol>(module));
```
Where `module` is `"/jank.nrepl-server.asio"` (WITH leading slash)

**These symbols don't match!**

The Clojure code checks for symbol `jank.nrepl-server.asio` but we're storing `/jank.nrepl-server.asio` with a leading slash.

### Why This Happens

Looking at root-resource (core.jank:4382-4383):
```clojure
(defn- root-resource [lib]
  (str "/" (name lib)))
```

The leading `/` is used for file paths (like "/jank/nrepl_server/asio.jank"), not for module names in *loaded-libs*.

But the C++ `jank_module_set_loaded` is called with the path format instead of the symbol format.

### Solution

Change `jank_module_set_loaded()` call to NOT include the leading slash:
```cpp
// Wrong:
jank_module_set_loaded("/jank.nrepl-server.asio");

// Correct:
jank_module_set_loaded("jank.nrepl-server.asio");
```

Or update `set_is_loaded` to strip the leading slash if present.

### Note on C++ is_loaded check

The C++ `loader::load()` also checks `is_loaded()` which creates a symbol the same way:
```cpp
auto const ret{ truthy(loaded_libs->contains(make_box<obj::symbol>(module))) };
```

If the C++ side always uses "/" prefix and Clojure side doesn't, they'll never match!

Need to verify which format is correct and make both sides consistent.
