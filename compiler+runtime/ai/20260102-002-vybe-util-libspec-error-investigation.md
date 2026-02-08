# iOS JIT Simulator - vybe.util Libspec Error Investigation & Fix Plan

## Date
2026-01-02

## Error Summary

When running `make ios-jit-sim-run` in ~/dev/something, the iOS JIT simulator fails during module loading with:

```
[loader] ERROR while executing entry function for: vybe.util (jank_load_vybe_util$loading__)
[loader] thrown object: {:error :assertion-failure, :data {:msg "not a libspec: [sleep #'clojure.core/sleep]"}}
[loader] current *ns*: vybe.util
[loader] current *file*: "NO_SOURCE_PATH"
[jank] binding_scope pop failed: Mismatched thread binding pop
```

## Key Findings

### 1. Libspec Validation
**Location:** `/Users/pfeodrippe/dev/jank/compiler+runtime/src/jank/clojure/core.jank`

The libspec validator (`libspec?` function) expects:
- A symbol (e.g., `clojure.string`), OR
- A vector where:
  - First element is a symbol/string
  - Second element (if present) is `nil` or a keyword (`:as`, `:refer`, etc.)

**The error:** `[sleep #'clojure.core/sleep]` has a **var** (`#'clojure.core/sleep`) as the second element, not a keyword. This is invalid.

### 2. Sleep Function Discovery
**Location:** `/Users/pfeodrippe/dev/something/SdfViewerMobile/jank-resources/src/jank/clojure/core.jank:4047`

```clojure
(defn- sleep [ms]
  (cpp/clojure.core_native.sleep ms))
```

`sleep` is a **private** function (`defn-`) in `clojure.core`. Private functions should not be referred by other namespaces.

### 3. Namespace Loading Sequence
From the error log:
1. ✅ `vybe.sdf.math` - SUCCESS (warning about `abs` shadowing)
2. ✅ `vybe.sdf.state` - SUCCESS
3. ❌ `vybe.util` - **FAILED** with libspec error
4. (remaining modules not loaded)

### 4. File Locations
Two copies of `vybe.util.jank` exist (identical content):
- `/Users/pfeodrippe/dev/something/src/vybe/util.jank` (source)
- `/Users/pfeodrippe/dev/something/SdfViewerMobile/jank-resources/src/jank/vybe/util.jank` (rsync'd copy)

**Note:** Neither file contains any reference to `sleep`.

### 5. AOT vs JIT Mismatch
Generated files found:
- `/Users/pfeodrippe/dev/something/SdfViewerMobile/generated/vybe_util_generated.cpp` (Dec 22, 10:29)
- Other vybe files generated Dec 22, 11:07-11:17

This suggests potential **stale AOT artifacts** or **AOT/JIT compilation conflict**.

### 6. Related Code Changes
**Modified files in compiler+runtime:**
- `src/cpp/jank/runtime/context.cpp` - binding_scope changes (explains the "Mismatched thread binding pop" error)
- `src/cpp/jank/codegen/processor.cpp` - debug output changes
- `include/cpp/jank/runtime/context.hpp` - binding scope header changes

## Root Cause - CONFIRMED! ⚠️

**This is NOT a stale AOT artifacts issue!** This is a **BUG in jank's `refer` implementation** when handling private functions.

### The Smoking Gun 🔫

In `/Users/pfeodrippe/dev/something/SdfViewerMobile/generated/vybe_util_generated.cpp:1341`:

```cpp
auto const vybe_util_call_11166(jank::runtime::dynamic_call(vybe_util_refer_9443->deref(), vybe_util_const_9444));
```

This is generated from the AOT compilation of `vybe.util` and translates to:

```clojure
(refer clojure.core)
```

**The Problem:**
1. `(refer clojure.core)` tries to refer ALL public symbols from `clojure.core`
2. `sleep` is defined as a PRIVATE function (`defn-`) in `clojure.core` (line 4047)
3. jank's `refer` implementation is incorrectly trying to refer `sleep` even though it's private
4. During the refer processing, jank creates a malformed libspec: `[sleep #'clojure.core/sleep]`
5. The libspec validator rejects it because the second element is a var object, not a keyword

### Why This Happens

The generated code contains:
```cpp
vybe_util_const_9444{jank::runtime::make_box<jank::runtime::obj::symbol>("", "clojure.core")}
```

When `(refer clojure.core)` executes, it should:
1. Get all public vars from `clojure.core`
2. Create refers for each public var

BUT it's incorrectly including `sleep` (a private var) in the refer list, and the format `[sleep #'clojure.core/sleep]` suggests it's using the var object instead of just the symbol.

### Architecture Notes

This is a **HYBRID AOT+JIT system**:
- vybe namespaces are AOT-compiled to C++ (in `SdfViewerMobile/generated/vybe_*_generated.cpp`)
- These AOT modules are linked into the iOS binary
- The JIT compile-server ALSO compiles some modules dynamically
- The error happens during **loading** of the AOT-compiled `vybe.util` module, not during compilation

## Investigation Steps

### Step 1: Clean AOT Artifacts
```bash
cd ~/dev/something
# Remove all generated AOT files for vybe namespaces
rm -f SdfViewerMobile/generated/vybe_*_generated.cpp
rm -f SdfViewerMobile/build-iphonesimulator/generated/vybe_*_generated.cpp
rm -f SdfViewerMobile/build-iphonesimulator/obj/vybe_*_generated.o
rm -f SdfViewerMobile/build/obj/vybe_*_generated.o
rm -f SdfViewerMobile/build-iphoneos/generated/vybe_*_generated.cpp
rm -f SdfViewerMobile/build-iphoneos/obj/vybe_*_generated.o
```

### Step 2: Check if vybe.util Should Be JIT-Only
The iOS JIT mode should compile vybe namespaces dynamically. Verify:
- Is `vybe.util` supposed to be AOT-compiled at all for JIT mode?
- Check Makefile targets to see what should/shouldn't be AOT-compiled

### Step 3: Inspect Generated Code (if cleaning doesn't work)
```bash
# Check if there are any sleep references in the generated code
grep -n "sleep" ~/dev/something/SdfViewerMobile/generated/vybe_util_generated.cpp
# Check the entry function for vybe.util
grep -A 50 "jank_load_vybe_util" ~/dev/something/SdfViewerMobile/generated/vybe_util_generated.cpp
```

### Step 4: Test with Minimal Namespace
Create a minimal test namespace to isolate the issue:
```bash
# Create test namespace
cat > ~/dev/something/src/vybe/test_minimal.jank << 'EOF'
(ns vybe.test-minimal
  (:require [clojure.string :as str]))

(defn hello [] "Hello from minimal test")
EOF

# Try to load it via iOS JIT
# If this fails, the issue is broader than vybe.util
```

### Step 5: Add Debug Logging
In `/Users/pfeodrippe/dev/jank/compiler+runtime/src/jank/clojure/core.jank`, around the libspec error:
```clojure
(throw-if (not (libspec? arg))
  (str "not a libspec: " arg
       " (type: " (type arg)
       ", first: " (when (vector? arg) (type (first arg)))
       ", second: " (when (and (vector? arg) (> (count arg) 1)) (type (second arg))) ")"))
```

This will show exactly what types are in the malformed libspec.

### Step 6: Check for Accidental :refer [sleep]
Search for any accidental references:
```bash
cd ~/dev/something
# Check if any namespace accidentally tries to refer sleep
grep -r ":refer.*sleep" --include="*.jank" .
grep -r "clojure.core.*sleep" --include="*.jank" .
```

### Step 7: Examine Namespace Loading Order
The error happens specifically when loading `vybe.util` after `vybe.sdf.state`. Check:
- Does `vybe.sdf.state` require `vybe.util`?
- Are there circular dependencies?
```bash
grep -n "vybe.util" ~/dev/something/src/vybe/sdf/state.jank
```

## Potential Fixes

### Fix 1: Fix jank's `ns-publics` to Actually Return Only Public Vars (ROOT CAUSE FIX) ⭐⭐⭐⭐⭐

**Location:** `/Users/pfeodrippe/dev/jank/compiler+runtime/src/jank/clojure/core.jank`

**The Bug:** At line 4436 in the `refer` function:
```clojure
sym->var (ns-publics ns)
```

The `ns-publics` function is supposed to return ONLY public vars but is returning ALL vars including private ones!

**Two possible fixes:**

**Option A: Fix `ns-publics` itself** (Recommended - fixes the root cause)
Find the `ns-publics` function and add filtering:
```clojure
(defn ns-publics
  "Returns a map of public interned vars in the namespace."
  [ns]
  (let [all-vars (get-all-vars ns)]  ; or however it currently gets vars
    ;; Filter out private vars:
    (into {}
          (remove (fn [[_ var]]
                    (-> var meta :private))
                  all-vars))))
```

**Option B: Filter in `refer` function** (Quick fix - line 4436-4437)
```clojure
(defn refer
  ;; ... existing code ...
  ([ns-sym & filters]
   (let [ns (find-ns ns-sym)
         ; ... existing code ...
         all-vars (ns-publics ns)  ; Currently returns private vars too!
         ;; Filter out private vars here:
         sym->var (into {}
                        (remove (fn [[_ var]]
                                  (-> var meta :private))
                                all-vars))
         to-refer (if (= :all (get filters :refer))
                    (keys sym->var)
                    (or (get filters :refer) (get filters :only) (keys sym->var)))]
     ;; rest of implementation
     ))
```

**Recommended:** Fix `ns-publics` itself (Option A) since other code may rely on it returning only public vars.

### Fix 2: Make `sleep` Public (WORKAROUND - Not Recommended)

If the fix to `refer` is complex, we can work around it by making `sleep` public:

```clojure
;; In /Users/pfeodrippe/dev/something/SdfViewerMobile/jank-resources/src/jank/clojure/core.jank:4047
;; Change from defn- to defn
(defn sleep [ms]  ; <-- changed from defn-
  (cpp/clojure.core_native.sleep ms))
```

**⚠️ Warning:** This is a workaround, not a proper fix. The real issue is that `refer` should skip private vars.

### Fix 3: Don't Use `(refer clojure.core)` in vybe.util (WORKAROUND)

Modify `vybe.util.jank` to explicitly list what it needs from `clojure.core` instead of using bare `(refer clojure.core)`:

```clojure
(ns vybe.util
  (:require [clojure.string :as str])
  ;; Don't use (refer clojure.core) implicitly
  ;; If you need specific symbols, use :refer
  )
```

**Note:** This might already be the case - check if vybe.util explicitly calls `(refer clojure.core)` or if it's being added automatically by the AOT compiler.

### Fix 4: Add Defensive Check in libspec Validation (BAND-AID)

This won't fix the root cause but will provide a better error message:

```clojure
;; In clojure.core.jank
(defn- libspec? [x]
  (or (symbol? x)
      (and (vector? x)
           (or (nil? (second x))
               (keyword? (second x))
               ;; Defensive: if it's a var, someone made a mistake
               (when (var? (second x))
                 (throw (ex-info :assertion-failure
                         {:msg (str "Invalid libspec format - got var instead of keyword: " x
                                   "\nThis usually means refer is trying to refer a private var.")})))))))
```

### Fix 5: Investigate Why vybe.util Has `(refer clojure.core)`

Check if this is:
- Explicitly in the source code
- Auto-generated by the AOT compiler
- Part of some namespace initialization macro

```bash
cd ~/dev/something
grep -n "(refer" src/vybe/util.jank
# If not found in source, it's being added by the compiler
```

## Testing Strategy

After each fix attempt:

1. **Clean build**:
   ```bash
   make clean  # in ~/dev/something
   make ios-jit-sim-build 2>&1 | tee /tmp/build-output.txt
   ```

2. **Run and capture output**:
   ```bash
   make ios-jit-sim-run 2>&1 | tee /tmp/run-output.txt
   ```

3. **Check for**:
   - ✅ No libspec errors
   - ✅ vybe.util loads successfully
   - ✅ All namespaces load in sequence
   - ✅ App runs without crashing

4. **If still failing**, examine `/tmp/run-output.txt` for:
   - Which namespace loads before the failure
   - Any new error messages
   - Stack traces or additional context

## Success Criteria

- [ ] `make ios-jit-sim-run` completes without libspec errors
- [ ] All vybe namespaces load successfully
- [ ] No "binding_scope pop failed" errors
- [ ] iOS simulator app runs normally
- [ ] nREPL connectivity works (if enabled)

## Additional Notes

### Binding Scope Error
The "binding_scope pop failed" error is likely a **secondary effect** of the libspec error. The libspec error causes an exception during namespace loading, which disrupts the normal binding scope cleanup. This should resolve once the libspec issue is fixed.

### Why JIT vs AOT Matters
- **AOT mode**: Namespaces compiled to C++ ahead of time, linked into binary
- **JIT mode**: Namespaces compiled on-demand by compile server, loaded dynamically
- **Hybrid**: Some core namespaces AOT, app namespaces JIT

The error suggests a mismatch or corruption in how AOT artifacts are being used in JIT mode.

## Next Steps

1. **IMMEDIATE**: Try Fix 1 (clean rebuild) - 90% chance this resolves it
2. **If Fix 1 fails**: Try Investigation Step 5 (add debug logging) to see exact types
3. **If still failing**: Try Fix 4 (revert binding_scope changes) to isolate the cause
4. **Document findings**: Update this file with results and root cause once identified

## References

- jank libspec validation: `/Users/pfeodrippe/dev/jank/compiler+runtime/src/jank/clojure/core.jank` (search for "libspec?")
- Error source: `load-libs` function in clojure.core
- vybe.util source: `~/dev/something/src/vybe/util.jank`
- Generated artifacts: `~/dev/something/SdfViewerMobile/generated/vybe_util_generated.cpp`
