# iOS Bundle Entry Module Implementation

## Summary

Implemented the `--entry-module` workflow in `ios-bundle` that consolidates most of the iOS AOT build logic into jank. This reduces project-specific build scripts from ~430 lines to ~60 lines (86% reduction).

## New CLI Options

```bash
ios-bundle \
  --entry-module MODULE   # Entry point module name (e.g., myapp.ios)
  --module-path DIR       # Module search path for jank (e.g., src)
  --output-library NAME   # Output static library name (default: libapp_aot.a)
  --jit-lib LIB           # Native library for JIT symbol resolution
  -I DIR                  # Include directory (for both jank compile and C++ cross-compile)
  -L DIR                  # Library search directory for JIT
  -v                      # Verbose output
  simulator               # Build for iOS Simulator
  device                  # Build for iOS Device (default)
```

## How It Works

When `--entry-module` is provided, ios-bundle:

1. **Discovers modules** - Uses `jank compile-module --list-modules` to find all dependencies
2. **Filters modules** - Excludes `clojure.*` modules (pre-compiled by jank)
3. **Generates C++** - Compiles each user module to C++ with WASM AOT codegen
4. **Patches includes** - Adds missing includes (workaround until codegen is fixed)
5. **Generates jank_aot_init.cpp** - Single entry point that loads all modules
6. **Cross-compiles** - Compiles all C++ to iOS arm64 object files
7. **Bundles library** - Creates static library from all .o files (core + user)
8. **Copies libraries** - Copies libjank.a, libjankzip.a, libgc.a to output dir

## Example Usage

### New Minimal Project Script (~60 lines)

```bash
#!/bin/bash
set -e
cd "$(dirname "$0")/.."

TARGET="$1"
JANK_SRC="${JANK_SRC:-/path/to/jank/compiler+runtime}"
OUTPUT_DIR="SdfViewerMobile/build-$([ "$TARGET" = "simulator" ] && echo "iphonesimulator" || echo "iphoneos")"

# Build dependencies (for JIT symbol resolution)
make build-sdf-deps JANK_SRC="$JANK_SRC"

# Build iOS AOT bundle
"$JANK_SRC/bin/ios-bundle" \
  --entry-module vybe.sdf.ios \
  --module-path src \
  --output-dir "$OUTPUT_DIR" \
  --output-library libvybe_aot.a \
  -I . \
  -I vendor/imgui \
  -I vendor/flecs/distr \
  -L /opt/homebrew/lib \
  --jit-lib /opt/homebrew/lib/libSDL3.dylib \
  "$TARGET"
```

### Old Project Script (~430 lines)

The old script had to manually:
- Call `--list-modules` and parse output
- Compile each module individually
- Generate `jank_aot_init.cpp` with correct module load order
- Handle C++ reserved words (template -> template__)
- Set all include paths for cross-compilation
- Copy core library .o files
- Create static library with `ar rcs`

## Output Structure

```
output-dir/
├── libjank.a          (copied from jank iOS build)
├── libjankzip.a       (copied from jank iOS build)
├── libgc.a            (copied from jank iOS build)
├── libvybe_aot.a      (bundled from all .o files)
├── generated/
│   ├── vybe_sdf_math_generated.cpp
│   ├── vybe_sdf_ui_generated.cpp
│   └── jank_aot_init.cpp
└── obj/
    ├── clojure_core_generated.o
    ├── vybe_sdf_math_generated.o
    ├── vybe_sdf_ui_generated.o
    └── jank_aot_init.o
```

## Xcode Integration

```yaml
# project.yml
OTHER_LDFLAGS:
  - "-ljank"
  - "-ljankzip"
  - "-lgc"
  - "-lvybe_aot"  # All AOT modules - no manual updates needed!

LIBRARY_SEARCH_PATHS:
  - "$(PROJECT_DIR)/build-$(PLATFORM_NAME)"
```

## Path Handling

The script handles relative paths correctly by:
1. Saving `original_cwd` before `pushd "${repo_root}"`
2. Converting relative paths to absolute using `original_cwd`
3. This applies to: `module_path`, `output_dir`, include paths, library paths

## Files Changed

- `/Users/pfeodrippe/dev/jank/compiler+runtime/bin/ios-bundle` - Main implementation

## Benefits

1. **86% code reduction** - From 427 to 59 lines in project script
2. **Automatic module discovery** - Just specify entry module
3. **Single static library** - No manual .o file management in Xcode
4. **Consistent builds** - Same logic for all projects
5. **Easy maintenance** - Fix in jank benefits all projects
