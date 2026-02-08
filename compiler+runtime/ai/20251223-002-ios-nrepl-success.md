# iOS JIT nREPL Module Loading - SUCCESS

## Summary
Successfully got nREPL server working in iOS JIT mode. The app now starts with live REPL support on port 5558.

## Root Cause
The module loading failure had two issues:

### Issue 1: Leading Slash Mismatch
- `jank_module_set_loaded("/jank.nrepl-server.asio")` was called WITH leading slash
- But `context::load_module()` (context.cpp:560-562) strips leading `/` before calling `loader::load()`
- So `is_loaded()` checks for `"jank.nrepl-server.asio"` WITHOUT slash
- These symbols didn't match!

### Issue 2: Timing with *loaded-libs*
- The iOS app has its own init flow in `sdf_viewer_ios.mm` (doesn't use `jank_init_with_pch()`)
- clojure.core's initialization resets `*loaded-libs*`
- If we registered the module BEFORE clojure.core loaded, the registration would be wiped

## Solution
Modified `/Users/pfeodrippe/dev/something/SdfViewerMobile/sdf_viewer_ios.mm`:

```cpp
extern "C" void* jank_load_jank_nrepl_server_asio();
extern "C" void jank_module_set_loaded(const char* module);

#if defined(JANK_IOS_JIT)
static bool load_jank_modules_jit() {
    // 1. Load clojure.core-native first
    jank_load_clojure_core_native();

    // 2. Load clojure.core from source
    jank::runtime::__rt_ctx->load_module("/clojure.core", ...);

    // 3. Register nREPL native module AFTER clojure.core loads
    //    (because clojure.core resets *loaded-libs*)
    jank_load_jank_nrepl_server_asio();
    jank_module_set_loaded("jank.nrepl-server.asio");  // NO leading slash!

    // 4. Now load the application module
    jank::runtime::__rt_ctx->load_module("/vybe.sdf.ios", ...);
}
#endif
```

## Key Insights

1. **Module name format**: Use `"jank.nrepl-server.asio"` (no leading `/`) when calling `jank_module_set_loaded()`

2. **Timing matters**: Native modules that need to be in `*loaded-libs*` must be registered AFTER clojure.core loads

3. **iOS has custom init**: The iOS app doesn't use `jank_init_with_pch()` - it has its own init in the .mm file

## Verification Output
```
[jank-jit] Registering nREPL server native module...
[jank-jit] nREPL native module registered!
[jank-jit] vybe.sdf.ios loaded successfully!
[jank] Runtime initialized successfully!
[iOS] Starting nREPL server on port 5558...
Starting embedded nREPL server on 127.0.0.1:5558
[iOS] nREPL server started!
```

## Files Modified
- `/Users/pfeodrippe/dev/something/SdfViewerMobile/sdf_viewer_ios.mm` - Added nREPL registration after clojure.core loads
- `/Users/pfeodrippe/dev/jank/compiler+runtime/src/cpp/jank/c_api.cpp` - Fixed leading slash (though this isn't used by iOS app's custom init)
- `/Users/pfeodrippe/dev/something/SdfViewerMobile/jank-resources/src/jank/vybe/sdf/ios.jank` - Added debug prints for troubleshooting

## Build Commands

```bash
# Build iOS JIT app
cd /Users/pfeodrippe/dev/something/SdfViewerMobile
xcodebuild -project SdfViewerMobile-JIT.xcodeproj -scheme SdfViewerMobile-JIT \
  -destination 'platform=iOS Simulator,id=57653CE6-DF09-4724-8B28-7CB6BA90E0E3' \
  -configuration Debug build

# Install and launch
xcrun simctl install 57653CE6-DF09-4724-8B28-7CB6BA90E0E3 \
  /Users/pfeodrippe/Library/Developer/Xcode/DerivedData/SdfViewerMobile-JIT-alktydjgxlubrrgzpaausrhpvdss/Build/Products/Debug-iphonesimulator/SdfViewerMobile-JIT.app
xcrun simctl launch --console-pty 57653CE6-DF09-4724-8B28-7CB6BA90E0E3 com.vybe.SdfViewerMobile-JIT
```

## Final Result
- iOS JIT app running with nREPL on port 5558
- 3D hand with cigarette rendering at ~158 FPS
- ImGui debug panel working
- Touch controls functional
