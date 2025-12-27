# iOS PCH Include Path Mismatch Fix

Date: 2025-12-26

## Problem

The iOS compile server was failing with "redefinition of 'is_same'" errors because headers were being included from two different locations:
- `/Users/pfeodrippe/dev/something/SdfViewerMobile/jank-resources/include/` (iOS resources)
- `/Users/pfeodrippe/dev/jank/compiler+runtime/include/cpp/` (jank repo)

The PCH was built with headers from the jank repo path, but during cross-compilation the iOS resources include path was added. Since these are different paths with the same headers, the compiler treated them as different files and tried to include them twice.

## Root Cause

The `build-ios-pch.sh` script was using:
```bash
-I"$JANK_SRC/include/cpp"
```

But the compile server's `make_ios_simulator_config()` adds:
```cpp
config.include_paths.push_back(jank_resource_dir + "/include");
```

These are two different paths to the same headers, causing redefinition errors.

## Solution

Updated `build-ios-pch.sh` to:
1. Copy jank headers to the iOS resources include directory (`jank-resources/include`)
2. Build PCH using those copied headers instead of the jank repo headers

Changes made:
```bash
# Always sync jank headers to output directory to ensure consistency
# The PCH must use the same header paths as cross-compilation
INCLUDE_DIR="$OUTPUT_DIR/include"
echo "Syncing jank headers to $INCLUDE_DIR..."
mkdir -p "$INCLUDE_DIR"
rm -rf "$INCLUDE_DIR/jank" "$INCLUDE_DIR/jtl"
cp -r "$JANK_SRC/include/cpp/"* "$INCLUDE_DIR/"

# Then build PCH with:
-I"$INCLUDE_DIR"  # instead of -I"$JANK_SRC/include/cpp"
```

## Key Insight

When using PCH with `-fincremental-extensions`, the paths in the PCH must exactly match the include paths used during compilation. Otherwise, the compiler finds the "same" headers at different paths and treats them as different files, causing redefinition errors.

## Files Changed

- `/Users/pfeodrippe/dev/something/SdfViewerMobile/build-ios-pch.sh`

## Testing

After rebuilding the PCH with corrected paths:
```bash
echo '{"op":"compile","id":1,"code":"(+ 1 2)","ns":"user"}' | nc -w 5 localhost 5570
```

Successfully returned compiled object file without any redefinition errors.
