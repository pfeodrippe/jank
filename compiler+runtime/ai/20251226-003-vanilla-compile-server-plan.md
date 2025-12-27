# Vanilla Compile Server Plan

## Date: 2025-12-26

## Problem Analysis

The current `make sdf-ios-server` starts the SDF desktop application which:
1. Loads all its dependencies (`vybe.sdf.math`, `vybe.sdf.state`, `vybe.sdf.shader`, etc.)
2. Starts the iOS compile server as a side feature

When iOS requests a namespace, the "newly loaded modules" approach doesn't work because all dependencies are already loaded by the desktop app. This creates a fundamental conflict between:
- What the desktop app needs (all its namespaces)
- What iOS needs (only the requested namespace and its transitive deps)

## Options Considered

### Option A: Vanilla Compile Server (RECOMMENDED)
Start a bare compile server without any app-specific namespace loading.

**How it works:**
- Create a new entry point that only starts `clojure.core` and the compile server
- No app namespaces are preloaded
- When iOS requests `vybe.sdf.ios`, the server JIT-loads all dependencies on-demand
- `loaded_modules_in_order` accurately tracks what was loaded for this request
- The original "newly loaded modules" approach works correctly

**Pros:**
- Clean, minimal footprint
- Only compiles exactly what iOS needs
- No false positives (no modules from desktop app)
- Simpler logic - the original approach works

**Cons:**
- Can't reuse desktop app's JIT state (headers compiled, etc.)
- Slightly slower first-time compilation for each iOS session
- Need separate process if also running desktop app

### Option B: Parse NS Forms for Dependencies
Keep desktop app running, but parse `:require` forms to find dependencies.

**How it works:**
- Parse the ns form to extract `:require` dependencies
- Recursively parse each dependency's ns form
- Build a complete transitive dependency graph
- Compile only those modules

**Pros:**
- Can run alongside desktop app
- Precise: only compiles what the ns actually requires

**Cons:**
- Complex: need full ns form parser
- Edge cases: `require` at runtime, conditional requires, etc.
- May miss dynamically required modules

### Option C: Track All Modules, Filter by iOS Need
Current approach: iterate all loaded modules, send ones not yet sent to iOS.

**Pros:**
- Simple implementation (current fix)
- Works without changes to runtime

**Cons:**
- Compiles modules iOS doesn't need
- Waste of compilation time and bandwidth
- Desktop app's modules pollute the list

## Recommendation: Option A

**Implement a vanilla compile server mode** that starts without loading any app namespaces.

### Implementation Steps

1. **Create new compile server entry point**
   - Add a `--compile-server-only` flag to jank CLI
   - Or create a separate `jank-compile-server` binary
   - Initializes runtime with only `clojure.core`
   - Starts compile server immediately

2. **Update Makefile**
   - Add `make ios-compile-server` target that runs vanilla server
   - Keep `make sdf-ios-server` for desktop app with embedded server
   - User chooses based on use case

3. **Revert current fix**
   - Go back to tracking "newly loaded modules" approach
   - Since server starts fresh, all deps will be "newly loaded"

4. **iOS workflow**
   - Start vanilla compile server: `make ios-compile-server`
   - Start iOS app which connects to server
   - Server compiles namespaces and their deps on-demand

### Why This Is Better

The fundamental insight is:
> A compile server should be stateless with respect to app namespaces.

When the compile server has no preloaded app state:
- Each iOS request is self-contained
- Module tracking works correctly
- No confusion between desktop and iOS needs

The current approach of embedding the compile server in the desktop app creates an inherent conflict that no amount of clever tracking can fully resolve.

## Action Items

1. [ ] Create `--compile-server-only` mode in jank CLI
2. [ ] Add `ios-compile-server` Makefile target
3. [ ] Revert the "all modules" fix back to "newly loaded modules"
4. [ ] Test with vanilla server
5. [ ] Update documentation
