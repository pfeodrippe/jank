# iOS JIT - Type Mismatch Error: Expected Integer, Got Type 160

## Date
2026-01-02

## Error Summary

After the app runs successfully for several frames with camera auto-rotation working, it crashes with:

```
[EXPECT_OBJECT] type mismatch: got 160 (unknown) expected 2 (integer) ptr=0x13d214cc0
[EXPECT_OBJECT] Memory dump (first 64 bytes at ptr): a0 4c 21 3d 1 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 c0 4c 21 3d 1 0 0 0 8 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
[jank] Error calling -main (std): [EXPECT_OBJECT] type mismatch: got 160 (unknown) expected 2 (integer)
```

## 🚨 CRITICAL CLUE: AOT vs JIT Difference

**User reports:** This works perfectly in iOS AOT mode, but ONLY fails in iOS JIT mode!

This is a SMOKING GUN that points to:
- NOT a bug in the C++ code (it works in AOT)
- NOT a bug in the jank application code (it works in AOT)
- **LIKELY:** A bug in the JIT compile-server's C++ FFI handling
- **LIKELY:** Different code generation for C++ interop between AOT and JIT
- **LIKELY:** Issue with how JIT dynamically loads/wraps C++ function returns

## Symptoms

1. App starts successfully ✅
2. Namespaces load correctly ✅
3. Camera auto-rotation works (angle-y incrementing by 0.01 each frame) ✅
4. Runs for ~60+ frames successfully ✅
5. **Suddenly crashes with type mismatch error** ❌

## Critical Clues

### 1. Type ID Analysis
- **Expected:** Type 2 (integer)
- **Got:** Type 160 (unknown)
- Type 160 is NOT a valid jank object type!

### 2. Memory Dump Analysis
```
a0 4c 21 3d 1 0 0 0 | 0 1 0 0 0 0 0 0 | 0 0 0 0 0 0 0 0 | 0 0 0 0 0 0 0 0
c0 4c 21 3d 1 0 0 0 | 8 0 0 0 0 0 0 0 | 0 0 0 0 0 0 0 0 | 0 0 0 0 0 0 0 0
```

Interpreting as little-endian 64-bit pointers:
- First 8 bytes: `0x13d214ca0` - looks like a pointer!
- Bytes 8-15: `0x0000000000000100` - could be type metadata (256 in decimal)
- At offset 32: `0x13d214cc0` - another pointer (matches the error ptr!)
- Bytes 40-47: `0x0000000000000008` - the number 8

**This looks like an object structure, not a raw integer!**

### 3. Timing Analysis
The error happens AFTER several successful frames of camera updates. The DEBUG output shows:
```
angle-y= 5.870039
angle-y= 5.880040
...
angle-y= 6.060044
[EXPECT_OBJECT] type mismatch
```

This suggests the error happens during normal frame processing, likely in the main draw loop.

## The Smoking Gun 🔫

**File:** `/Users/pfeodrippe/dev/something/src/vybe/sdf/ios.jank` lines 110-124

```clojure
(defn draw [frame-count]
  (poll-events!)
  (sync-edit-mode-from-cpp!)      ; ← Called every frame!
  (sync-camera-from-cpp!)
  (when (ui/auto-rotate?)
    (state/update-camera! update :angle-y + 0.01)
    (sync-camera-to-cpp!)
    (set-dirty!))
  (process-pending-shader-switch!)
  (shader/check-shader-reload!)
  (ui/new-frame!)
  (ui/draw-debug-ui!)
  (update-uniforms! 0.016)
  (ui/render!)
  (draw-frame!))
```

### Suspect #1: sync-edit-mode-from-cpp!

**File:** `ios.jank:45-50`

```clojure
(defn sync-edit-mode-from-cpp! []
  (state/set-edit-mode!
   {:enabled (sdfx/get_edit_mode)
    :selected-object (sdfx/get_selected_object)    ; ← Could return wrong type!
    :hovered-axis (sdfx/get_hovered_axis)          ; ← Could return wrong type!
    :dragging-axis (sdfx/get_dragging_axis)}))     ; ← Could return wrong type!
```

**The Problem:**
These C++ functions are supposed to return integers but might be:
1. Returning **pointers** (`int*`) instead of **values** (`int`)
2. Returning **objects** instead of primitives
3. Returning **uninitialized memory** in certain states

### Suspect #2: state/update-camera!

**File:** `ios.jank:115`

```clojure
(state/update-camera! update :angle-y + 0.01)
```

This line calls `state/update-camera!` with:
- `update` - the core Clojure function for updating maps
- `:angle-y` - the key to update
- `+` - the function to apply
- `0.01` - the value to add

**Potential issues:**
1. If `+` is not being recognized as an integer function
2. If `0.01` is being interpreted as the wrong type (double vs float)
3. If the return value is wrapped in an unexpected object type

### Suspect #3: C++ Function Return Types

The C++ functions might have return type declarations like:

```cpp
// WRONG (returns pointer):
int* get_selected_object() {
  return has_selection ? &object_id : nullptr;
}

// CORRECT (returns value):
int get_selected_object() {
  return has_selection ? object_id : -1;
}
```

If they return pointers, jank might be trying to wrap them as objects, creating type 160 (garbage).

## Root Cause Hypothesis

### ❌ REJECTED: Return Type Hypothesis
**Initially thought:** C++ functions return `int*` instead of `int`
**Investigation:** Found functions correctly return `int` by value (see Step 1)
**Conclusion:** This is NOT the issue

### 🎯 PRIMARY HYPOTHESIS: JIT Compile-Server C++ Interop Bug

**KEY INSIGHT:** Code works in AOT but fails in JIT → Bug is in JIT-specific code paths!

**Theory:** The JIT compile-server generates incorrect code for C++ FFI calls:

**AOT Code Path (WORKING ✅):**
1. AOT compiler generates C++ code that calls `get_selected_object()`
2. Return value is directly wrapped as `obj::integer` in generated C++
3. Compiled into binary, linked statically
4. Works perfectly!

**JIT Code Path (BROKEN ❌):**
1. JIT compile-server compiles jank code on-demand
2. Generates code to call C++ functions dynamically
3. **BUG HERE:** Return value wrapping fails for C++ interop functions
4. Creates malformed object with type ID 160 instead of 2
5. Type check fails → crash

**Possible JIT-specific issues:**
1. **Symbol resolution:** JIT might not be finding the correct C++ function signature
2. **Return type inference:** JIT might be treating `int` as a different type (pointer, double, etc.)
3. **Object wrapping:** JIT's dynamic object creation might be broken for primitive returns
4. **ABI mismatch:** JIT might be using wrong calling convention for C++ functions
5. **Memory layout:** JIT-created objects might have different memory layout than AOT objects

**Why type 160 (0xa0)?**
- JIT might be reading from wrong memory offset
- JIT might be creating objects with incorrect header
- JIT might be passing uninitialized stack memory as object

**Why it happens after several frames:**
- JIT compilation happens lazily - function compiled on first call
- After several frames, a specific code path gets JIT-compiled
- That JIT-compiled code has the FFI bug

## Investigation Steps

### Step 1: Find C++ Function Declarations ✅ COMPLETED

**FOUND:** `vulkan/sdf_engine.hpp:2301-2314`

```cpp
inline int get_selected_object() {
    auto* e = get_engine();
    return e ? e->selectedObject : -1;
}

inline int get_hovered_axis() {
    auto* e = get_engine();
    return e ? e->hoveredAxis : -1;
}

inline int get_dragging_axis() {
    auto* e = get_engine();
    return e ? e->draggingAxis : -1;
}
```

**Result:** ✅ Functions correctly return `int` by value (not pointer)
**Result:** ✅ Functions correctly use -1 as sentinel value
**Result:** ✅ Member variables are declared as `int` (lines 228-230)

**Conclusion:** The C++ function signatures are CORRECT. The bug is NOT in the return types.

### Step 2: Check the C++ Implementation

Look for how these functions are implemented:

```cpp
// If implementation looks like this - IT'S WRONG:
int* get_selected_object() {
  static int selected = -1;
  if (!has_selection) return nullptr;
  selected = current_selection_id;
  return &selected;  // ← BAD: returning address of local/static variable
}

// Should be:
int get_selected_object() {
  if (!has_selection) return -1;
  return current_selection_id;  // ← GOOD: returning value
}
```

### Step 2A: Compare AOT vs JIT Generated Code 🔍 NEW STEP!

Since the bug only happens in JIT mode, compare how AOT and JIT handle C++ interop:

**For AOT (working):**
```bash
cd ~/dev/something

# Find the AOT-generated code for ios.jank
grep -A 10 "get_selected_object\|get_hovered_axis\|get_dragging_axis" \
  SdfViewerMobile/generated/*ios*generated.cpp | head -50
```

Look for how the return values are wrapped.

**For JIT (broken):**
The JIT compile-server generates code at runtime. We need to:
1. Check if JIT generates different code for C++ interop
2. Look at the jank compiler's FFI code generation
3. Check for any recent changes to FFI handling

**Files to investigate:**
- `/Users/pfeodrippe/dev/jank/compiler+runtime/src/cpp/jank/codegen/processor.cpp` - Code generation
- Search for C++ interop handling in codegen

### Step 2B: Check Recent Changes to C++ Interop 🔍 NEW STEP!

```bash
cd /Users/pfeodrippe/dev/jank/compiler+runtime

# Check for recent changes to FFI or C++ interop
git log --since="2 weeks ago" --grep="ffi\|interop\|cpp" --oneline

# Check for changes to object wrapping
git log --since="2 weeks ago" -S "expect_object" --oneline

# Check for changes to type system
git log --since="2 weeks ago" -S "object_type" --oneline
```

### Step 2C: Test with Simpler C++ Function 🔍 NEW STEP!

Create a minimal test case to isolate the JIT FFI bug:

**File:** Add to `vulkan/sdf_engine.hpp` (or test file)
```cpp
// Minimal test function
inline int test_simple_int_return() {
    return 42;
}

inline int test_negative_int_return() {
    return -1;
}
```

**File:** Add to `vybe/sdf/ios.jank`
```clojure
(defn test-jit-ffi! []
  (println "[TEST] Calling test_simple_int_return...")
  (let [result1 (sdfx/test_simple_int_return)]
    (println "[TEST] Got result1=" result1 "type=" (type result1)))

  (println "[TEST] Calling test_negative_int_return...")
  (let [result2 (sdfx/test_negative_int_return)]
    (println "[TEST] Got result2=" result2 "type=" (type result2))))
```

Call this early in the draw loop to see if even simple int returns fail.

### Step 3: Add Debug Output to Identify Which Function Fails

**File:** `/Users/pfeodrippe/dev/something/src/vybe/sdf/ios.jank`

```clojure
(defn sync-edit-mode-from-cpp! []
  (println "[DEBUG edit-mode] About to call get_edit_mode")
  (let [enabled (sdfx/get_edit_mode)]
    (println "[DEBUG edit-mode] enabled=" enabled "type=" (type enabled))

    (println "[DEBUG edit-mode] About to call get_selected_object")
    (let [selected (sdfx/get_selected_object)]
      (println "[DEBUG edit-mode] selected=" selected "type=" (type selected))

      (println "[DEBUG edit-mode] About to call get_hovered_axis")
      (let [hovered (sdfx/get_hovered_axis)]
        (println "[DEBUG edit-mode] hovered=" hovered "type=" (type hovered))

        (println "[DEBUG edit-mode] About to call get_dragging_axis")
        (let [dragging (sdfx/get_dragging_axis)]
          (println "[DEBUG edit-mode] dragging=" dragging "type=" (type dragging))

          (state/set-edit-mode!
           {:enabled enabled
            :selected-object selected
            :hovered-axis hovered
            :dragging-axis dragging}))))))
```

This will show EXACTLY which function call fails and what type it returns.

### Step 4: Check state/set-edit-mode! Implementation

The error might also be in how `state/set-edit-mode!` processes the values:

```bash
cd ~/dev/something
grep -rn "defn set-edit-mode!" --include="*.jank" src/
```

Check if it's doing type checking or conversions that might fail.

### Step 5: Memory Dump Analysis

The memory dump shows:
```
Offset 0-7:   a0 4c 21 3d 01 00 00 00  → Pointer: 0x13d214ca0
Offset 8-15:  00 01 00 00 00 00 00 00  → Integer: 256 (0x100)
Offset 32-39: c0 4c 21 3d 01 00 00 00  → Pointer: 0x13d214cc0 (matches error ptr!)
Offset 40-47: 08 00 00 00 00 00 00 00  → Integer: 8
```

This looks like a jank object structure:
- Offset 0: vtable pointer or type info pointer
- Offset 8: type ID (256 = 0x100, which is byte-swapped 0x0001 = type 1?)
- Offset 32: another pointer (the actual error location)
- Offset 40: a count or size field

**The fact that type 160 appears** suggests byte-corruption or misaligned memory access.

## Solutions

### Solution 1: Investigate JIT Code Generation for C++ FFI (PRIMARY TASK) ⭐⭐⭐⭐⭐

**❌ SKIP:** C++ return types are already correct (verified in Step 1)

**Focus on JIT compile-server instead!**

**Location:** `/Users/pfeodrippe/dev/jank/compiler+runtime/src/cpp/jank/codegen/processor.cpp`

**What to look for:**
1. How does JIT generate code for C++ function calls with `:scope` annotations?
2. How does JIT wrap primitive return values (int, float, bool)?
3. Are there any special cases for C++ interop that might fail?
4. Recent changes to FFI handling that could have broken this?

**Key questions:**
- Does JIT correctly infer return types from C++ functions?
- Does JIT properly box primitive C++ returns into jank objects?
- Is there a difference in how JIT vs AOT handles `inline` C++ functions?
- Could there be an ABI mismatch when JIT calls C++ code?

**Debugging approach:**
```bash
cd /Users/pfeodrippe/dev/jank/compiler+runtime

# Search for C++ interop code generation
grep -rn "cpp::" src/cpp/jank/codegen/
grep -rn "scope.*interop\|interop.*scope" src/cpp/jank/codegen/
grep -rn "make_box.*integer" src/cpp/jank/
```

### Solution 2: Fix C++ Pointer Returns (If They Must Return Pointers)

If the functions MUST return pointers for some reason, change the jank code to dereference them:

**File:** `ios.jank:45-50`

```clojure
(defn sync-edit-mode-from-cpp! []
  (state/set-edit-mode!
   {:enabled (sdfx/get_edit_mode)
    :selected-object (let [ptr (sdfx/get_selected_object)]
                       (if (nil? ptr) -1 ptr))  ; ← Handle pointer properly
    :hovered-axis (let [ptr (sdfx/get_hovered_axis)]
                    (if (nil? ptr) -1 ptr))
    :dragging-axis (let [ptr (sdfx/get_dragging_axis)]
                     (if (nil? ptr) -1 ptr))}))
```

**Note:** This assumes jank can dereference C++ pointers, which it might not be able to do.

### Solution 3: Add Type Guards in Jank Code

**File:** `ios.jank:45-50`

Add defensive type checking:

```clojure
(defn safe-get-int [f default-value]
  "Safely call a C++ function that should return int, handle errors"
  (try
    (let [result (f)]
      (if (integer? result)
        result
        (do
          (println "[WARN] Expected integer from" f "got type" (type result))
          default-value)))
    (catch Object e
      (println "[ERROR] Exception calling" f ":" e)
      default-value)))

(defn sync-edit-mode-from-cpp! []
  (state/set-edit-mode!
   {:enabled (sdfx/get_edit_mode)
    :selected-object (safe-get-int #(sdfx/get_selected_object) -1)
    :hovered-axis (safe-get-int #(sdfx/get_hovered_axis) -1)
    :dragging-axis (safe-get-int #(sdfx/get_dragging_axis) -1)}))
```

### Solution 4: Check for Memory Corruption

If the C++ code is writing to invalid memory, it could corrupt jank objects:

```bash
cd ~/dev/something

# Check for buffer overruns or memory issues in C++ code
# Look for array accesses without bounds checking
grep -rn "selected_object\|hovered_axis\|dragging_axis" \
  --include="*.cpp" vulkan/ | grep -E "\[|\+\+"
```

## Testing Strategy

### Test 1: Add Debug Output (Step 3 above)
Run the app and see which specific function call fails:
```bash
cd ~/dev/something
make ios-jit-sim-build && make ios-jit-sim-run 2>&1 | tee /tmp/debug-output.txt
```

Look for the last successful debug line before the crash.

### Test 2: Quick Workaround Test
Try skipping `sync-edit-mode-from-cpp!` to confirm it's the source:

```clojure
(defn draw [frame-count]
  (poll-events!)
  ; (sync-edit-mode-from-cpp!)  ; ← COMMENT OUT TEMPORARILY
  (sync-camera-from-cpp!)
  ...)
```

If the app runs without crashing, we've confirmed the issue is in `sync-edit-mode-from-cpp!`.

### Test 3: Check C++ Function Return Types
```bash
cd ~/dev/something
# Find function declarations
grep -A 3 "get_selected_object\|get_hovered_axis\|get_dragging_axis" \
  vulkan/*.hpp SdfViewerMobile/*.hpp
```

Look for return types. If you see `int*` instead of `int`, that's the smoking gun.

## Expected C++ Function Signatures

**CORRECT signatures (return by value):**
```cpp
// In vulkan/sdf_engine.hpp or similar
namespace sdfx {
  int get_edit_mode();           // Returns 0 or 1 (bool-like)
  int get_selected_object();     // Returns object ID or -1
  int get_hovered_axis();        // Returns axis ID or -1
  int get_dragging_axis();       // Returns axis ID or -1
}
```

**INCORRECT signatures (return by pointer):**
```cpp
// DON'T USE THESE:
int* get_selected_object();     // ← BAD: returns pointer
int* get_hovered_axis();        // ← BAD: returns pointer
int* get_dragging_axis();       // ← BAD: returns pointer
```

## Success Criteria

- [ ] No "type mismatch" errors
- [ ] App runs continuously for 100+ frames without crashing
- [ ] Edit mode sync works correctly
- [ ] Selected/hovered/dragged values are valid integers (-1 or positive)
- [ ] Camera rotation continues to work smoothly

## Related Files

- `/Users/pfeodrippe/dev/something/src/vybe/sdf/ios.jank` - Main iOS entry point
- `/Users/pfeodrippe/dev/jank/compiler+runtime/include/cpp/jank/runtime/rtti.hpp` - Type checking code
- `~/dev/something/vulkan/sdf_engine.hpp` - C++ header (likely location of function declarations)
- `~/dev/something/vulkan/sdf_engine.cpp` - C++ implementation (likely location)

## Key Differences from Previous Error

**Previous error (004):** Functions returned `nil` when expecting integer
**This error (005):** Functions return object type 160 (pointer?) when expecting integer

The previous error was about NULL/nil. This error is about returning the **wrong object type entirely**.

## Next Steps

1. **IMMEDIATE:** Add debug output (Step 3) to identify which function fails
2. **Then:** Find C++ function declarations (Step 1) to check return types
3. **Fix:** Change C++ functions to return `int` by value, not `int*` by pointer
4. **Test:** Rebuild and verify app runs without crashes
5. **Verify:** All edit mode functionality works correctly

## Architecture Notes

### jank Object Type IDs
```cpp
enum class object_type : uint8_t {
  nil = 0,
  boolean = 1,
  integer = 2,        // ← Expected type
  real = 3,
  // ... other types ...
  unknown = 160       // ← Got this! NOT A VALID TYPE!
};
```

Type 160 is NOT in the valid enum range, suggesting:
1. Memory corruption
2. Uninitialized variable
3. Pointer being interpreted as object
4. C++ ABI mismatch

### C++ ↔ jank Interop Contract

**For integer returns:**
- C++ should return `int` (by value)
- jank wraps it in `obj::integer`
- Type ID should be 2

**What's happening:**
- C++ returns `int*` (pointer) or garbage
- jank tries to wrap the pointer as an object
- Creates invalid object with type ID 160
- Type check fails with "expected 2, got 160"

## Additional Investigation

### Check for Stack Corruption
The pointers in the memory dump (0x13d214ca0, 0x13d214cc0) are very close together (32 bytes apart). This could indicate:
1. Stack allocated objects
2. Objects in an array or vector
3. Heap allocated objects

Check if the C++ code is:
```cpp
// BAD: Returning stack addresses
int* get_selected_object() {
  int value = selected_id;
  return &value;  // ← DANGER: returning address of local variable!
}

// GOOD: Returning heap or global
static int selected_id = -1;
int* get_selected_object() {
  return &selected_id;  // OK if truly needed, but better to return by value
}
```

### Type 160 = 0xA0 Analysis
In hexadecimal, 160 = 0xA0. Looking at the memory dump, the first byte is `a0`. This suggests the type ID might be reading from the wrong memory location - possibly reading the first byte of a pointer as the type ID!

If jank's object layout is:
```
Offset 0: type_id (1 byte)
Offset 1-7: padding or flags
Offset 8-15: data or pointer
```

And the C++ function returns a raw pointer like `0x13d214ca0`, jank might be wrapping it as:
```
Offset 0: 0xa0 (first byte of pointer) → interpreted as type 160!
Offset 1-7: 0x4c 0x21 0x3d 0x01 0x00 0x00 0x00
```

This confirms the hypothesis that a C++ pointer is being misinterpreted as a jank integer object.

---

## 📊 Summary of Investigation

### ✅ What We Know
1. **Error:** Type mismatch - expected integer (type 2), got type 160 (unknown)
2. **Location:** Happens in `sync-edit-mode-from-cpp!` when calling C++ functions
3. **C++ Functions:** `get_selected_object()`, `get_hovered_axis()`, `get_dragging_axis()`
4. **C++ Code:** ✅ VERIFIED CORRECT - returns `int` by value, uses -1 sentinel
5. **AOT Mode:** ✅ WORKS PERFECTLY - no errors
6. **JIT Mode:** ❌ FAILS - type mismatch error

### 🎯 Conclusion
**This is a JIT compile-server bug in C++ FFI handling, NOT a C++ code bug.**

The JIT compiler is generating incorrect code for wrapping C++ `int` returns into jank `obj::integer` objects.

### 🔍 Next Investigation Steps (Priority Order)
1. **Add debug output** (Step 3) to confirm which function call fails
2. **Compare AOT vs JIT code** (Step 2A) to see difference in generated code
3. **Check recent changes** (Step 2B) to FFI/interop code in jank compiler
4. **Create minimal test** (Step 2C) to isolate the JIT FFI bug
5. **Investigate JIT codegen** (Solution 1) to find the root cause

### 💡 Recommended Immediate Action
**Run Step 3** (add debug output) to identify EXACTLY which function call fails first:
- Is it `get_edit_mode()`? (returns bool)
- Is it `get_selected_object()`? (returns int)
- Is it `get_hovered_axis()`? (returns int)
- Is it `get_dragging_axis()`? (returns int)

This will narrow down whether the bug affects all primitive returns or just certain types.
