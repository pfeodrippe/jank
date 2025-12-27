# iOS JIT-Only Device Mode: Compile Server Not Running Fix

## Problem

When running `make ios-jit-only-device-run`, the device app failed with `sdf_sqrt` not found error:
```
[jank-ios] Registered native alias (header pre-compiled by server): <vybe/vybe_sdf_math.h>
input_line_3:1:77: error: use of undeclared identifier 'sdf_sqrt'
```

The simulator worked fine, but device JIT-only mode failed.

## Root Cause

The device compile server on port 5571 was not running. Investigation showed:
- Simulator compile server (port 5570): Running ✓
- Device compile server (port 5571): NOT running ✗

The issue was that unlike the simulator target which uses `xcrun simctl launch --console-pty` (which blocks), the device target's `xcrun devicectl device process launch` returns immediately. This caused make to exit quickly after launching the app, and the background compile server process could get orphaned or killed.

## Solution

Updated `ios-jit-only-device-run` in the Makefile to block after launching the app:

```makefile
xcrun devicectl device process launch --device "$$DEVICE_ID" com.vybe.SdfViewerMobile-JIT-Only-Device; \
echo ""; \
echo "═══════════════════════════════════════════════════════════════════════"; \
echo "  App launched! Compile server running on port 5571."; \
echo "  Connect nREPL to localhost:5559"; \
echo "  Press Ctrl+C to stop compile server and exit."; \
echo "═══════════════════════════════════════════════════════════════════════"; \
echo ""; \
echo "Compile server logs:"; \
tail -f /dev/null
```

The `tail -f /dev/null` keeps the make process alive, which:
1. Keeps the compile server running on port 5571
2. Gives clear instructions to the user
3. Allows clean shutdown with Ctrl+C

## Files Modified

- `/Users/pfeodrippe/dev/something/Makefile` - Added blocking wait to `ios-jit-only-device-run` target

## Ports Summary

| Mode | Compile Server Port | nREPL Port |
|------|---------------------|------------|
| Simulator | 5570 | Direct to sim |
| Device | 5571 | 5559 (via iproxy → device 5558) |
