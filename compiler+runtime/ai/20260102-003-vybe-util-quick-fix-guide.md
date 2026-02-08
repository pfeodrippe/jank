# Quick Fix Guide: vybe.util Libspec Error

## ⚠️ UPDATE: ROOT CAUSE IDENTIFIED!

**This is NOT a stale build artifact issue!** This is a **BUG in jank's `ns-publics` function**.

### The Problem

`ns-publics` is supposed to return **only public vars** but it returns **ALL vars including private ones**.

When `(refer clojure.core)` runs:
1. It calls `ns-publics` to get public vars (line 4436 in `refer` function)
2. `ns-publics` incorrectly returns private vars like `sleep`
3. `refer` tries to create bindings for private vars
4. This creates a malformed libspec `[sleep #'clojure.core/sleep]`
5. The validator rejects it → Error!

### Why Making sleep Public Won't Work

There are many other private vars in `clojure.core` and other namespaces that will cause the same issue. We must fix `ns-publics` itself.

## TL;DR - The Fix 🚀

Fix the `ns-publics` function to filter out private vars (see Fix 1 below):

## 🔧 FIX 1: Fix `ns-publics` to Filter Private Vars (PROPER FIX) ⭐⭐⭐⭐⭐

**Location:** `/Users/pfeodrippe/dev/jank/compiler+runtime/src/jank/clojure/core.jank`

The `ns-publics` function returns ALL vars including private ones. It needs to filter them out.

**Find the `ns-publics` function:**
```bash
cd /Users/pfeodrippe/dev/jank/compiler+runtime
grep -n "^(defn ns-publics" src/jank/clojure/core.jank
```

**Add filtering to exclude private vars:**
```clojure
(defn ns-publics
  "Returns a map of public interned vars in the namespace."
  [ns]
  (let [all-vars (... current implementation ...)]
    ;; ADD THIS: Filter out private vars
    (into {}
          (remove (fn [[_ var]]
                    (-> var meta :private))
                  all-vars))))
```

**After editing:**
```bash
# Rebuild jank
./bin/compile

# Rebuild iOS app
cd ~/dev/something
make ios-jit-sim-build 2>&1 | tee /tmp/build-fixed.log
make ios-jit-sim-run 2>&1 | tee /tmp/run-fixed.log
```

**Expected result:** All namespaces load successfully, no more libspec errors ✅

---

## 🔧 FIX 2 (Alternative): Patch `refer` Directly to Skip Private Vars ⭐⭐⭐

This fixes the symptom in `refer` instead of fixing `ns-publics`. Less ideal because other code might also rely on `ns-publics` being correct.

**Location:** Line 4436-4437 in `/Users/pfeodrippe/dev/jank/compiler+runtime/src/jank/clojure/core.jank`

```bash
cd /Users/pfeodrippe/dev/jank/compiler+runtime
grep -n "sym->var (ns-publics ns)" src/jank/clojure/core.jank
```

**Change this code (line 4436-4437):**
```clojure
sym->var (ns-publics ns)
to-refer (if (= :all (get filters :refer))
```

**To this:**
```clojure
all-vars (ns-publics ns)
;; Filter out private vars since ns-publics doesn't
sym->var (into {}
               (remove (fn [[_ var]]
                         (-> var meta :private))
                       all-vars))
to-refer (if (= :all (get filters :refer))
```

**After editing:**
```bash
# Rebuild jank
./bin/compile

# Rebuild iOS app
cd ~/dev/something
make ios-jit-sim-build 2>&1 | tee /tmp/build-fixed.log
make ios-jit-sim-run 2>&1 | tee /tmp/run-fixed.log
```

**Note:** Fix 1 (fixing `ns-publics`) is preferred as it fixes the root cause.

---

## 🔧 DIAGNOSTIC: See What refer Is Actually Doing (5 minutes)

Add debug output to see exactly what's happening:

```bash
cd /Users/pfeodrippe/dev/jank/compiler+runtime

# Find the refer function
grep -n "defn refer" src/jank/clojure/core.jank

# Add debug output before the error:
# (println "DEBUG: referring symbols from" ns-sym ":" (map first publics))
```

---

## 🔍 DEBUGGING: If All Else Fails

### Add Debug Logging to See What's in the Libspec

Edit `/Users/pfeodrippe/dev/jank/compiler+runtime/src/jank/clojure/core.jank`:

Find this line (around line 3900-3950):
```clojure
(throw-if (not (libspec? arg)) (str "not a libspec: " arg))
```

Replace with:
```clojure
(throw-if (not (libspec? arg))
  (str "not a libspec: " arg
       "\n  type: " (type arg)
       "\n  vector?: " (vector? arg)
       (when (vector? arg)
         (str "\n  count: " (count arg)
              "\n  first: " (first arg) " (type: " (type (first arg)) ")"
              "\n  second: " (when (> (count arg) 1)
                             (str (second arg) " (type: " (type (second arg)) ")"))))
       "\n  libspec? returned: " (libspec? arg)))
```

Then rebuild jank:
```bash
cd /Users/pfeodrippe/dev/jank/compiler+runtime
./bin/compile
```

Run the iOS app again and you'll get detailed type information about the malformed libspec.

### Check What Requires vybe.util

```bash
cd ~/dev/something
echo "🔍 Finding what requires vybe.util..."
grep -r "vybe.util" --include="*.jank" SdfViewerMobile/jank-resources/src/jank/
grep -r "vybe.util" --include="*.jank" src/
```

### Look for Accidental sleep References

```bash
cd ~/dev/something
echo "🔍 Looking for sleep references..."
grep -r "sleep" --include="*.jank" src/vybe/
grep -r ":refer.*sleep\|:exclude.*sleep" --include="*.jank" .
```

---

## ✅ Success Checklist

After trying a solution, verify:

- [ ] No error: `"not a libspec: [sleep #'clojure.core/sleep]"`
- [ ] Namespace loads: `[loader] Loaded remote module: vybe.util`
- [ ] No binding_scope errors
- [ ] App continues to load remaining modules
- [ ] iOS simulator shows the app UI

---

## 📊 Solution Comparison

| Solution | Type | Complexity | Time | Correctness |
|----------|------|------------|------|-------------|
| Fix 1: Fix `ns-publics` | Root cause fix | Medium | 10-15 min | ⭐⭐⭐⭐⭐ Best |
| Fix 2: Patch `refer` | Symptom fix | Low | 5-10 min | ⭐⭐⭐⭐ Good |
| Diagnostic | Investigation | Low | 5 min | N/A |

---

## 🎯 Recommended Approach

**RECOMMENDED: Fix `ns-publics` (Fix 1)**
- Fixes the root cause
- Other code may rely on `ns-publics` returning only public vars
- 10-15 minutes to implement

**ALTERNATIVE: Patch `refer` directly (Fix 2)**
- Faster to implement (5-10 minutes)
- Only fixes the symptom in this one place
- Good if you need a quick fix and can't find/modify `ns-publics` easily

**For investigation:**
- Use the diagnostic approach to confirm the behavior and understand what vars are being incorrectly referred

---

## 💡 Why This Happens - Technical Deep Dive

**The Root Cause:**
1. The AOT-compiled `vybe_util_generated.cpp` contains a call to `(refer clojure.core)` at line 1341
2. `refer` calls `(ns-publics ns)` at line 4436 to get all public vars from the namespace
3. **THE BUG:** `ns-publics` returns ALL vars, including private ones like `sleep`
4. `sleep` is defined as **private** (`defn-`) in `clojure.core` (line 4047)
5. When `refer` tries to process `sleep`, it creates a malformed libspec: `[sleep #'clojure.core/sleep]`
6. The libspec validator expects `[symbol :keyword]` but gets `[symbol var-object]`, causing the error

**The call chain:**
```
(refer clojure.core)               # Line 1341 in generated code
  → (ns-publics 'clojure.core)     # Line 4436 in refer function
    → Returns ALL vars including private ones ← BUG HERE
  → Tries to refer private vars like 'sleep'
    → Creates malformed libspec [sleep #'clojure.core/sleep]
      → Validation error!
```

**Why this affects ALL namespaces:**
Any namespace with private vars will have this problem when another namespace calls `(refer that-namespace)`. It just happens that `clojure.core` has many private vars (like `sleep`, and likely others), so it fails immediately.

**Why cleaning doesn't work:**
This is NOT a stale build issue. The generated code is correct. The bug is in the runtime `ns-publics` function that doesn't filter out private vars as it should.

---

## 📝 After You Fix It

Once you've resolved the issue, please:

1. **Document which solution worked** in:
   `/Users/pfeodrippe/dev/jank/compiler+runtime/ai/20260102-002-vybe-util-libspec-error-investigation.md`

2. **If it was the binding_scope changes**, consider:
   - Whether the changes need refinement
   - Whether there's a bug in how exceptions interact with binding_scope cleanup
   - Filing an issue or adding a comment in the code

3. **If it was stale build artifacts**, consider:
   - Adding a Makefile target like `make clean-vybe` for future use
   - Documenting which directories need cleaning for JIT mode

Good luck! 🚀
