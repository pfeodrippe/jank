# iOS JIT - Null Integer Error in C++ Interop

## Date
2026-01-02

## Error Summary

After fixing the `ns-publics` / `refer` private var issue, the iOS app now runs but crashes with:

```
[EXPECT_OBJECT] null ref for type integer
[jank] Error calling -main (std): [EXPECT_OBJECT] null ref when expecting type integer
```

## Symptoms

1. App starts successfully ✅
2. Namespaces load correctly ✅
3. Main loop begins running ✅
4. Camera sync works (can see debug output) ✅
5. **After several frames, crashes with integer null ref error** ❌

## Root Cause Analysis

### The Error Location

**File:** `/Users/pfeodrippe/dev/jank/compiler+runtime/include/cpp/jank/runtime/rtti.hpp`

The `expect_object<T>()` function is called when converting an `object_ref` to a specific type (like integer). It fails when the `object_ref` is nil but an integer is expected:

```cpp
template <typename T>
constexpr oref<T> expect_object(object_ref const o)
{
  if constexpr(T::obj_type != object_type::nil)
  {
    if(!o.is_some())
    {
      std::cerr << "[EXPECT_OBJECT] null ref for type " << object_type_str(T::obj_type) << "\n";
      throw std::runtime_error(
        "[EXPECT_OBJECT] null ref when expecting type " + std::string(object_type_str(T::obj_type)));
    }
  }
  // ...
}
```

### The Smoking Gun 🔫

**File:** `/Users/pfeodrippe/dev/something/src/vybe/sdf/ios.jank` lines 45-50

```clojure
(defn sync-edit-mode-from-cpp! []
  (state/set-edit-mode!
   {:enabled (sdfx/get_edit_mode)
    :selected-object (sdfx/get_selected_object)    ; ← Returns nil when nothing selected
    :hovered-axis (sdfx/get_hovered_axis)          ; ← Returns nil when nothing hovered
    :dragging-axis (sdfx/get_dragging_axis)}))     ; ← Returns nil when nothing dragging
```

**The Problem:**
1. These C++ functions (`get_selected_object`, `get_hovered_axis`, `get_dragging_axis`) are supposed to return integer IDs
2. When nothing is selected/hovered/dragged, the C++ code returns **NULL/nullptr**
3. NULL gets converted to **nil** in jank
4. Later code expects these values to be integers and calls `expect_object<integer>()`
5. The nil value causes the "null ref for type integer" error

### Why It Happens After Several Frames

The app runs fine initially because:
- Initially, there might be default values set
- The error only triggers when user interaction occurs (or lack thereof)
- Once the C++ code returns NULL for one of these fields, jank crashes trying to use it as an integer

## C++ Interop Contract Violation

**Expected behavior:**
- C++ functions should return sentinel values like `-1` or `0` for "no object"
- Never return NULL/nullptr for functions that jank expects to return integers

**Actual behavior:**
- C++ functions return NULL when there's no selection/hover/drag
- NULL becomes nil in jank
- jank code treats it as an integer → crash

## Solutions

### Solution 1: Fix C++ Side to Return Sentinel Values (RECOMMENDED) ⭐⭐⭐⭐⭐

**Location:** The C++ implementation of these functions in `vulkan/sdf_engine.hpp` or wherever they're defined

Change from returning NULL to returning `-1`:

```cpp
// BEFORE (wrong):
int* get_selected_object() {
  if (!has_selection) return nullptr;  // ← BAD
  return &selected_object_id;
}

// AFTER (correct):
int get_selected_object() {
  if (!has_selection) return -1;  // ← GOOD: -1 means "no selection"
  return selected_object_id;
}
```

Do the same for:
- `get_selected_object()` → return -1 when nothing selected
- `get_hovered_axis()` → return -1 when nothing hovered
- `get_dragging_axis()` → return -1 when nothing dragging

### Solution 2: Add Nil-Handling in Jank Code (WORKAROUND) ⭐⭐⭐

**Location:** `/Users/pfeodrippe/dev/something/src/vybe/sdf/ios.jank` line 45-50

Add nil→integer conversion:

```clojure
(defn sync-edit-mode-from-cpp! []
  (state/set-edit-mode!
   {:enabled (sdfx/get_edit_mode)
    :selected-object (or (sdfx/get_selected_object) -1)  ; ← Convert nil to -1
    :hovered-axis (or (sdfx/get_hovered_axis) -1)        ; ← Convert nil to -1
    :dragging-axis (or (sdfx/get_dragging_axis) -1)}))  ; ← Convert nil to -1
```

**Note:** This is a workaround. The C++ code should be fixed to not return NULL.

### Solution 3: Use Optional/Nullable Types (PROPER LONG-TERM FIX) ⭐⭐⭐⭐

If these values can legitimately be absent, use optional types:

**C++ side:**
```cpp
std::optional<int> get_selected_object() {
  if (!has_selection) return std::nullopt;
  return selected_object_id;
}
```

**Jank side:**
```clojure
(defn sync-edit-mode-from-cpp! []
  (state/set-edit-mode!
   {:enabled (sdfx/get_edit_mode)
    :selected-object (sdfx/get_selected_object)  ; nil is OK here
    :hovered-axis (sdfx/get_hovered_axis)         ; nil is OK here
    :dragging-axis (sdfx/get_dragging_axis)}))   ; nil is OK here
```

Then handle nil values downstream in state management.

## Investigation Steps

### Step 1: Find the C++ Function Definitions

```bash
cd ~/dev/something
# Find where these functions are defined
grep -rn "get_selected_object\|get_hovered_axis\|get_dragging_axis" --include="*.hpp" --include="*.cpp" .
```

### Step 2: Check Return Types

Look at the function signatures:
- Do they return `int*` (pointer)? → NULL is possible, need to change to `int`
- Do they return `int`? → Should never be NULL, check implementation
- Do they return `std::optional<int>`? → Then jank code needs to handle nil

### Step 3: Test with Debug Output

Add debug logging to see what values are returned:

```clojure
(defn sync-edit-mode-from-cpp! []
  (let [selected (sdfx/get_selected_object)
        hovered (sdfx/get_hovered_axis)
        dragging (sdfx/get_dragging_axis)]
    (println "[DEBUG edit-mode] selected=" selected "hovered=" hovered "dragging=" dragging)
    (state/set-edit-mode!
     {:enabled (sdfx/get_edit_mode)
      :selected-object selected
      :hovered-axis hovered
      :dragging-axis dragging})))
```

Run the app and watch for when it prints nil values.

### Step 4: Check Similar Issues

These functions might have the same problem:

```clojure
;; Line 53-61: read-object-from-cpp
:type (sdfx/get_object_type_id idx)
:selectable (sdfx/get_object_selectable idx)
```

Check if these can also return nil.

## Quick Fix to Test (2 minutes)

Add nil handling to confirm this is the issue:

```bash
cd ~/dev/something

# Edit ios.jank
cat > /tmp/fix.patch << 'EOF'
--- a/src/vybe/sdf/ios.jank
+++ b/src/vybe/sdf/ios.jank
@@ -45,9 +45,9 @@
 (defn sync-edit-mode-from-cpp! []
   (state/set-edit-mode!
    {:enabled (sdfx/get_edit_mode)
-    :selected-object (sdfx/get_selected_object)
-    :hovered-axis (sdfx/get_hovered_axis)
-    :dragging-axis (sdfx/get_dragging_axis)}))
+    :selected-object (or (sdfx/get_selected_object) -1)
+    :hovered-axis (or (sdfx/get_hovered_axis) -1)
+    :dragging-axis (or (sdfx/get_dragging_axis) -1)}))
EOF

# Apply and test
patch -p1 < /tmp/fix.patch
make ios-jit-sim-build && make ios-jit-sim-run
```

**Expected result:** App runs without crashing ✅

## Proper Fix Implementation

### Step 1: Locate C++ Functions

```bash
cd ~/dev/something
grep -rn "int.*get_selected_object" --include="*.hpp" --include="*.cpp" .
grep -rn "int.*get_hovered_axis" --include="*.hpp" --include="*.cpp" .
grep -rn "int.*get_dragging_axis" --include="*.hpp" --include="*.cpp" .
```

### Step 2: Change Return Type from Pointer to Value

If they currently return `int*`:

```cpp
// Change from:
int* get_selected_object() {
  return selection ? &object_id : nullptr;
}

// To:
int get_selected_object() {
  return selection ? object_id : -1;
}
```

### Step 3: Update Any C++ Callers

Search for usages and update any C++ code that expects a pointer:

```bash
grep -rn "get_selected_object()" --include="*.cpp" .
```

### Step 4: Rebuild Everything

```bash
# Clean C++ build
make clean-cpp  # or equivalent

# Rebuild
make ios-jit-sim-build

# Test
make ios-jit-sim-run
```

## Success Criteria

- [ ] App runs continuously without crashing
- [ ] No "EXPECT_OBJECT null ref for type integer" errors
- [ ] Edit mode selection/hover/drag works correctly
- [ ] When nothing is selected, value is `-1` instead of nil
- [ ] Camera controls continue to work smoothly

## Related Issues

### Other Potential nil→integer Issues

Check these functions for similar problems:

**In read-object-from-cpp (line 53-61):**
```clojure
:type (sdfx/get_object_type_id idx)
:selectable (sdfx/get_object_selectable idx)
```

**In sync-camera-from-cpp (line 32-42):**
```clojure
(let [d (sdfx/get_camera_distance)
      ax (sdfx/get_camera_angle_x)
      ay (sdfx/get_camera_angle_y)
      ty (sdfx/get_camera_target_y)]
  ...)
```

These *seem* less likely to return nil (cameras probably always have values), but worth checking.

## Architecture Notes

This is a common C++ ↔ jank interop pitfall:

**C++ mindset:** Use NULL/nullptr to indicate "no value"
**Jank mindset:** Use nil for "no value", but distinguish between optional and required values

**Best practices:**
1. **Never return NULL for required integer values** - use sentinel like -1
2. **For truly optional values** - use `std::optional<T>` on C++ side, nil-check on jank side
3. **Document the contract** - what does -1 mean? What does nil mean?
4. **Be consistent** - all similar functions should follow the same pattern

## Next Steps

1. Try Solution 2 (quick workaround) to confirm diagnosis
2. Locate C++ function definitions (Investigation Step 1)
3. Implement Solution 1 (proper fix)
4. Test thoroughly with user interaction
5. Check for similar issues in other C++ interop functions
