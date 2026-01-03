# iOS UIKit Main Thread Requirement

## Problem
After fixing the module loading order and moving `-main` to a background pthread for stack size, the app crashed with:
```
Main Thread Checker: UI API called on a background thread: -[UIWindow initWithFrame:]
NSInternalInconsistencyException: Call must be made on main thread
```

## Root Cause
UIKit APIs (used by SDL for window creation) **must** be called from the main thread on iOS. Our pthread wrapper was running the entire `-main` function on a background thread.

## Analysis
The large-stack pthread was originally needed because:
1. iOS main thread has ~1MB stack
2. jank's JIT compilation uses deep recursion (visitor pattern)
3. Module loading triggers JIT compilation

However, once modules are loaded:
- `-main` only runs AOT-compiled code
- The render loop (`poll_events`, `draw_frame`) doesn't need deep stacks
- SDL/UIKit window creation **requires** main thread

## Solution
Split the execution:
1. **Module loading**: Keep on 8MB pthread (in `init_jank_runtime_on_large_stack()`)
2. **-main execution**: Run on main thread (for UIKit compatibility)

```cpp
// Before: ran -main on pthread
static void call_jank_main() {
    // ... pthread creation with 8MB stack ...
    pthread_create(&thread, &attr, jank_main_thread_func, &result);
}

// After: run -main on main thread
static void call_jank_main() {
    std::cout << "[jank] Running -main on main thread (required for UIKit)..." << std::endl;
    call_jank_main_impl();
}
```

## Key Insight
The deep stack is needed for **JIT compilation** (module loading), not for **running AOT code**. Since all modules are now AOT-compiled and properly marked as loaded via `jank_module_set_loaded()`, there's no JIT compilation happening during `-main`.

## Files Changed
- `/Users/pfeodrippe/dev/something/SdfViewerMobile/sdf_viewer_ios.mm`: Simplified `call_jank_main()` to run on main thread
