# New CLI Flag: --list-modules for compile-module

## Summary

Added a `--list-modules` flag to the `compile-module` command that prints all loaded modules in dependency order. This enables build scripts to automatically detect and compile the required modules for iOS AOT builds without manual configuration.

## Usage

```bash
jank --module-path src compile-module --list-modules vybe.sdf.ios
```

Output:
```
vybe.sdf.math
vybe.sdf.state
clojure.string
vybe.util
vybe.sdf.shader
vybe.sdf.ui
vybe.sdf.ios
```

## Implementation

### Files Changed

1. **`include/cpp/jank/util/cli.hpp`**
   - Added `bool list_modules{}` to the options struct

2. **`src/cpp/jank/util/cli.cpp`**
   - Added CLI flag parsing for `--list-modules`

3. **`src/cpp/main.cpp`**
   - Modified `compile_module()` to print modules when flag is set
   - Uses `__rt_ctx->loaded_modules_in_order` which tracks all loaded modules
   - Filters out core modules (clojure.core, jank.nrepl-server.asio) since those are part of libjank

### How It Works

When jank compiles a module, it loads all dependencies recursively. Each loaded module is tracked in `loaded_modules_in_order`. The `--list-modules` flag simply prints this list after compilation completes.

## Build Script Integration

The iOS AOT build script (`build_ios_jank_aot.sh`) now uses this feature:

```bash
# Entry point module - all other modules are auto-detected from dependencies
ENTRY_MODULE="vybe.sdf.ios"

# Auto-detect modules using --list-modules
ALL_MODULES=$("$JANK_BIN" "${JANK_FLAGS[@]}" compile-module --list-modules "$ENTRY_MODULE" 2>&1 | grep -v "^\[jank\]" | grep -v "^WARNING:")

# Filter to only include user modules (exclude clojure.* which are pre-compiled)
VYBE_MODULES=()
while IFS= read -r module; do
    if [[ -n "$module" && ! "$module" =~ ^clojure\. ]]; then
        VYBE_MODULES+=("$module")
    fi
done <<< "$ALL_MODULES"
```

## Benefits

1. **No manual module list** - Just specify the entry point module
2. **Automatic dependency resolution** - jank knows the correct load order
3. **Build-time validation** - Missing dependencies cause compilation errors, not runtime crashes
4. **Easy to maintain** - Add a new `:require` and it's automatically picked up

## Notes

- The `clojure.*` modules (clojure.string, clojure.set, etc.) are pre-compiled in the jank iOS build and don't need to be AOT compiled by user projects
- Core modules (clojure.core, jank.nrepl-server.asio) are filtered out automatically
- The order in the output is correct for loading - dependencies come before dependents

## Fully Dynamic Build System

The build script now creates `libvybe_aot.a` containing ALL compiled modules:

```bash
# Creates libvybe_aot.a from all .o files
ar rcs "$IOS_BUILD_DIR/libvybe_aot.a" "$IOS_OBJ_DIR"/*.o
```

The `project.yml` only needs:
```yaml
OTHER_LDFLAGS:
  - "-lvybe_aot"  # All AOT modules bundled - no manual updates needed!
```

**Workflow:**
1. Add a new `(:require [new.module])` in your jank code
2. Run `build_ios_jank_aot.sh` - auto-detects and compiles the new module
3. Build with Xcode - no project.yml changes needed!
