# jank WASM AOT Bug: Module Require with Native Headers

**Date**: 2025-12-21

## Problem

When using `--codegen wasm-aot`, compiling a jank module that requires OTHER jank modules (which use native C++ headers) fails with:

```
Uncaught exception: Failed to find symbol: 'jank_load_vybe_sdf_math'
```

## Example That Fails

```clojure
;; vybe.sdf.ios - requires other jank modules
(ns vybe.sdf.ios
  (:require
   ["vulkan/sdf_engine.hpp" :as sdfx :scope "sdfx"]  ; C++ header - OK
   [vybe.sdf.math :as m]      ; jank module - FAILS!
   [vybe.sdf.state :as state] ; jank module - FAILS!
   [vybe.sdf.shader :as shader])) ; jank module - FAILS!
```

## Example That Works

```clojure
;; vybe.sdf.iosmain4 - only requires C++ headers directly
(ns vybe.sdf.iosmain4
  (:require
   ["vulkan/sdf_engine.hpp" :as sdfx :scope "sdfx"]))  ; C++ header only - OK!

(defn -main []
  (sdfx/init "vulkan_kim")
  ;; render loop
  (sdfx/cleanup))
```

## Root Cause

During WASM AOT codegen, jank tries to JIT-load dependencies at compile time:
1. Compiling `vybe.sdf.ios`
2. Sees `(:require [vybe.sdf.math :as m])`
3. Tries to call `jank_load_vybe_sdf_math` symbol
4. Symbol doesn't exist (not JIT compiled, we're in AOT mode)
5. **Error**: "Failed to find symbol: 'jank_load_vybe_sdf_math'"

## Workaround

Make each iOS AOT module **standalone** - only require C++ native headers, not other jank modules:

```clojure
;; Instead of requiring jank modules, inline the needed code
;; or call C++ functions directly
(ns vybe.sdf.iosmain4
  (:require
   ["vulkan/sdf_engine.hpp" :as sdfx :scope "sdfx"]))
```

## Modules That Work for iOS AOT

- `vybe.util` - no native header requires
- `vybe.sdf.math` - only requires math C++ headers
- `vybe.sdf.state` - no native header requires
- `vybe.sdf.shader` - requires sdf_engine.hpp but standalone
- `vybe.sdf.iosmain4` - requires sdf_engine.hpp, standalone entry point

## Modules That FAIL for iOS AOT

- `vybe.sdf.ios` - requires other jank modules (math, state, shader, ui)
- `vybe.sdf.events` - requires other jank modules
- `vybe.sdf.render` - requires other jank modules

## Future Fix

The jank WASM AOT compiler should:
1. Not require JIT loading of dependencies during AOT codegen
2. Or, generate forward declarations for `jank_load_*` symbols
3. Or, allow pre-specifying which modules are available
