# iOS JIT Device Run Implementation Plan

## Goal
Create `make ios-jit-device-run` that builds, installs, and launches the iOS JIT app on a connected device.

## Current State

### Simulator JIT (working)
- `make ios-jit-sim-run` works
- Uses `SdfViewerMobile-JIT.xcodeproj` (generated from `project-jit.yml`)
- Libraries in `build-iphonesimulator-jit/`
- LLVM headers from `/Users/pfeodrippe/dev/ios-llvm-build/ios-llvm-simulator/include`

### Device JIT (needs implementation)
- `make ios-jit-device` builds jank libraries for device → `build-ios-device-jit/`
- LLVM for device needs to be at `/Users/pfeodrippe/dev/ios-llvm-build/ios-llvm-device/include`
- Need a device-specific Xcode project or modify existing one

## Key Differences: Simulator vs Device

| Aspect | Simulator | Device |
|--------|-----------|--------|
| Library path | `build-iphonesimulator-jit/` | `build-iphoneos-jit/` |
| LLVM path | `ios-llvm-simulator/` | `ios-llvm-device/` |
| SDK | `iphonesimulator` | `iphoneos` |
| Install command | `xcrun simctl install` | `xcrun devicectl device install app` |
| Launch command | `xcrun simctl launch` | `xcrun devicectl device process launch` |

## Implementation Steps

### Step 1: Create Device JIT Project
Create `project-jit-device.yml` that uses:
- `build-iphoneos-jit/` for library paths
- `ios-llvm-device/include` for LLVM headers
- Device SDK settings

### Step 2: Generate Xcode Project
```bash
cd SdfViewerMobile
xcodegen generate --spec project-jit-device.yml --project SdfViewerMobile-JIT-Device
```

### Step 3: Copy Device JIT Libraries
The Makefile needs to copy jank device JIT libraries to `build-iphoneos-jit/`:
```bash
cp -r /Users/pfeodrippe/dev/jank/compiler+runtime/build-ios-device-jit/* SdfViewerMobile/build-iphoneos-jit/
```

### Step 4: Build with xcodebuild
```bash
cd SdfViewerMobile && xcodebuild \
    -project SdfViewerMobile-JIT-Device.xcodeproj \
    -scheme SdfViewerMobile-JIT-Device \
    -configuration Debug \
    -sdk iphoneos \
    -allowProvisioningUpdates \
    build
```

### Step 5: Install and Launch
```bash
# Get device ID
DEVICE_ID=$(xcrun devicectl list devices 2>/dev/null | grep -E "connected.*iPad|connected.*iPhone" | awk '{print $3}' | head -1)

# Find built app
APP_PATH=$(find ~/Library/Developer/Xcode/DerivedData -name "SdfViewerMobile-JIT-Device.app" -path "*/Debug-iphoneos/*" 2>/dev/null | head -1)

# Install
xcrun devicectl device install app --device "$DEVICE_ID" "$APP_PATH"

# Launch
xcrun devicectl device process launch --device "$DEVICE_ID" com.vybe.SdfViewerMobile-JIT-Device
```

## Prerequisites Check
- [ ] Device LLVM built: `make ios-jit-llvm-device` (one-time, ~2 hours)
- [ ] Device jank built: `make ios-jit-device`
- [ ] Physical iOS device connected

## Files to Create/Modify

1. **`SdfViewerMobile/project-jit-device.yml`** - New xcodegen spec for device
2. **`Makefile`** - Add `ios-jit-device-run` target

## Alternative: Single Project with Configurations

Instead of two separate projects, could use xcodegen's `configs` to have Debug-Device and Debug-Simulator configurations. But separate projects are simpler and clearer.

---

## Implementation Complete

### Files Created
1. **`SdfViewerMobile/project-jit-device.yml`** - XcodeGen spec for device JIT
   - Uses `build-iphoneos-jit/` for library paths
   - Uses `ios-llvm-device/include` for LLVM headers
   - Target: `SdfViewerMobile-JIT-Device`

### Makefile Targets Added
1. **`ios-jit-device-libs`** - Copies device JIT libraries from jank build
2. **`ios-jit-device-project`** - Generates Xcode project using xcodegen
3. **`ios-jit-device-run`** - Full workflow: sync sources → build → install → launch

### Usage
```bash
# Prerequisites (one-time, ~2 hours each)
make ios-jit-llvm-device   # Build LLVM for device
make ios-jit-device        # Build jank JIT for device

# Run app on device
make ios-jit-device-run
```

### How It Works
1. `ios-jit-sync-sources` - Syncs all `.jank` files from `src/vybe/` to bundle
2. `ios-jit-device-libs` - Copies jank libraries to `build-iphoneos-jit/`
3. `ios-jit-device-project` - Generates Xcode project from yml
4. Builds with xcodebuild using `iphoneos` SDK
5. Detects connected device using `xcrun devicectl`
6. Installs and launches app on device
