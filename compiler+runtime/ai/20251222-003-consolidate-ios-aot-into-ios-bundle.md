# Consolidate iOS AOT Build into ios-bundle

## Problem

Currently there are two scripts involved in iOS AOT builds:
1. `jank/bin/ios-bundle` - Builds jank runtime and core libraries for iOS
2. `SdfViewerMobile/build_ios_jank_aot.sh` - Project-specific script that does most of the work

The project-specific script is ~430 lines and duplicates much of what `ios-bundle` does. We want to move reusable functionality into jank so project scripts become minimal.

## Current Responsibilities

### `build_ios_jank_aot.sh` (project-specific, ~430 lines)

| Step | What it does | Can move to jank? |
|------|--------------|-------------------|
| Step 0 | Call ios-bundle to build jank runtime | Already in jank |
| Step 1 | Use `--list-modules` to discover dependencies | **YES** |
| Step 1 | Generate C++ for each module | **YES** |
| Step 1 | Add missing includes (workaround) | **YES** (fix in codegen) |
| Step 1.5 | Generate `jank_aot_init.cpp` | **YES** |
| Step 2 | Set jank include paths | **YES** |
| Step 2 | Set project-specific include paths | NO (project config) |
| Step 2 | Cross-compile .cpp to .o | **YES** |
| Step 3 | Copy jank libraries | **YES** |
| Step 3 | Copy core .o files | **YES** |
| Step 3 | Bundle into `libvybe_aot.a` | **YES** |

### `ios-bundle` (jank-provided, ~520 lines)

Already handles:
- Building jank runtime for iOS (calls `build-ios`)
- Regenerating core libraries (clojure.core, etc.)
- Compiling a single .jank file
- Cross-compiling C++ for iOS

Missing:
- Module discovery via `--list-modules`
- Multi-module compilation
- `jank_aot_init.cpp` generation
- Static library bundling

## Proposed Design

### Enhanced `ios-bundle` Interface

```bash
# New interface - compile a full iOS app from an entry point module
jank/bin/ios-bundle \
  --entry-module vybe.sdf.ios \
  --module-path src \
  --output-dir build-iphoneos \
  --output-library libapp_aot.a \
  -I vendor/imgui \
  -I vendor/imgui/backends \
  -I vendor/flecs/distr \
  -L /opt/homebrew/lib \
  --jit-lib /opt/homebrew/lib/libvulkan.dylib \
  --jit-lib /opt/homebrew/lib/libSDL3.dylib \
  device  # or simulator
```

### New `ios-bundle` Functionality

1. **Module Discovery** (new)
   ```bash
   # Use --list-modules to get all dependencies
   modules=$(jank compile-module --list-modules "$entry_module")
   ```

2. **Multi-Module Compilation** (new)
   ```bash
   for module in $modules; do
     jank --codegen wasm-aot --save-cpp-path "$cpp_file" compile-module "$module"
   done
   ```

3. **Generate jank_aot_init.cpp** (new)
   - Auto-generate the single entry point that loads all modules
   - Handle C++ reserved words (template -> template__)

4. **Bundle Static Library** (new)
   ```bash
   ar rcs "$output_library" "$obj_dir"/*.o
   ```

### Simplified Project Script

After consolidation, the project script becomes ~20 lines:

```bash
#!/bin/bash
# build_ios_aot.sh - Minimal project-specific iOS AOT build script

set -e

TARGET="${1:-simulator}"
JANK_SRC="${JANK_SRC:-/path/to/jank/compiler+runtime}"

# Ensure project dependencies are built (native, for JIT symbol resolution)
make build-sdf-deps

# Build iOS AOT bundle
"$JANK_SRC/bin/ios-bundle" \
  --entry-module vybe.sdf.ios \
  --module-path src \
  --output-dir "SdfViewerMobile/build-$([ "$TARGET" = "simulator" ] && echo "iphonesimulator" || echo "iphoneos")" \
  --output-library libvybe_aot.a \
  -I . \
  -I vendor/imgui \
  -I vendor/imgui/backends \
  -I vendor/flecs/distr \
  -I vendor/miniaudio \
  -I vendor \
  -L /opt/homebrew/lib \
  --jit-lib /opt/homebrew/lib/libvulkan.dylib \
  --jit-lib /opt/homebrew/lib/libSDL3.dylib \
  --jit-lib /opt/homebrew/lib/libshaderc_shared.dylib \
  --jit-lib "$PWD/vulkan/libsdf_deps.dylib" \
  "$TARGET"
```

## Implementation Plan

### Phase 1: Add `--entry-module` Support to ios-bundle

1. Add new CLI options:
   - `--entry-module MODULE` - Entry point module name
   - `--output-library NAME` - Output static library name (default: libapp_aot.a)

2. When `--entry-module` is provided:
   - Run `jank compile-module --list-modules $entry_module` to discover all dependencies
   - Filter out clojure.* modules (pre-compiled by jank)
   - Compile each user module to C++

### Phase 2: Add jank_aot_init.cpp Generation

1. After discovering modules, generate `jank_aot_init.cpp`:
   - Extern declarations for all `jank_load_*` functions
   - Single `jank_aot_init()` function that calls them in order
   - Handle C++ reserved words (template -> template__)

2. The core module load functions should be:
   - `jank_load_clojure_core_native()`
   - `jank_load_core()`
   - `jank_load_string()`
   - `jank_load_set()`
   - `jank_load_walk()`
   - `jank_load_template__()`
   - `jank_load_test()`

### Phase 3: Add Static Library Bundling

1. After cross-compiling all .cpp to .o:
   - Copy core library .o files from jank build
   - Bundle all .o files into single static library
   - Use `ar rcs` to create the archive

2. Output structure:
   ```
   output-dir/
   ├── libjank.a          (copied from jank build)
   ├── libjankzip.a       (copied from jank build)
   ├── libgc.a            (copied from jank build)
   ├── libapp_aot.a       (bundled from all .o files)
   └── obj/
       ├── clojure_core_generated.o
       ├── clojure_set_generated.o
       ├── ...
       ├── vybe_util_generated.o
       ├── vybe_sdf_ui_generated.o
       └── jank_aot_init.o
   ```

### Phase 4: Fix Codegen Include Issues

Currently we have this workaround in build_ios_jank_aot.sh:
```bash
sed -i '' 's|#include <jank/runtime/core/meta.hpp>|#include <jank/runtime/core/meta.hpp>
#include <jank/runtime/obj/opaque_box.hpp>
#include <jank/c_api.h>|' "$cppfile"
```

This should be fixed in jank's codegen to emit proper includes.

## Migration Path

1. Implement Phase 1-3 in ios-bundle
2. Test with SdfViewerMobile project
3. Replace build_ios_jank_aot.sh with minimal wrapper
4. Document the new ios-bundle interface
5. Fix codegen includes (Phase 4)

## Benefits

1. **Less duplication** - Project scripts don't need to know about jank internals
2. **Easier maintenance** - Fixes to iOS build process benefit all projects
3. **Better defaults** - jank knows its own include paths
4. **Simpler project setup** - Just specify entry module and project-specific paths
5. **Automatic updates** - New modules are discovered automatically

## Risks

1. **Breaking changes** - Existing ios-bundle users may need to update
2. **Complexity** - ios-bundle becomes more complex
3. **Project-specific needs** - Some projects may need custom behavior

## Alternatives Considered

### Alternative 1: New `jank ios-aot` Command

Add a new jank CLI command instead of extending ios-bundle:
```bash
jank ios-aot --entry-module vybe.sdf.ios --output-dir build-ios simulator
```

Pros: Cleaner separation, doesn't change existing script
Cons: More code duplication, harder to share cross-compilation logic

### Alternative 2: Configuration File

Use a config file (e.g., `jank-ios.edn`):
```clojure
{:entry-module "vybe.sdf.ios"
 :module-path "src"
 :include-paths ["vendor/imgui" "vendor/flecs/distr"]
 :jit-libs ["/opt/homebrew/lib/libvulkan.dylib"]}
```

Pros: More declarative, easier to version control
Cons: Another file to maintain, less flexible

### Recommended: Extend ios-bundle

The recommended approach is to extend `ios-bundle` because:
1. It already handles iOS cross-compilation
2. It's the existing entry point for iOS builds
3. Keeps everything in one script
4. Backward compatible (existing options still work)
