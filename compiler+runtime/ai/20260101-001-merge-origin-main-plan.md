# Merge origin/main Plan - 2026-01-01

## Current Situation
- On branch: `nrepl-4`
- Need to merge with: `origin/main`
- Origin/main has 20+ commits ahead
- Breaking changes expected from origin/main

## Analysis of Changes

### Major Changes in origin/main
Looking at the diff stats, the main changes include:
1. **Build System & CI**: Changes to CI workflows, CMakeLists.txt restructuring
2. **New Directories**: `.claude/`, `.serena/`, new documentation files
3. **Documentation**: CLAUDE.md, AGENTS.md, PROMPTS.md added to root
4. **Code Structure**: Massive refactoring in compiler+runtime/
5. **iOS Support**: New iOS-related files and configurations
6. **WASM Support**: New WASM-related implementations
7. **nREPL**: Significant nREPL server implementation changes
8. **Runtime**: Core runtime refactoring (allocator, math, etc.)

### Potential Conflict Areas
Based on current branch changes:
- `test/bash/clojure-test-suite/clojure-test-suite` (submodule changes)
- `third-party/bdwgc` (submodule changes)
- Potential conflicts in CMakeLists.txt
- Potential conflicts in runtime/context files
- Potential conflicts in nREPL implementations

## Merge Strategy

### Phase 1: Pre-Merge Preparation
1. ✅ Fetch origin/main
2. Create backup of current state
3. Check submodule status
4. Document current test baseline

### Phase 2: Merge Execution
1. Attempt merge with origin/main
2. Identify conflicts
3. Analyze each conflict carefully:
   - Preserve nrepl-4 branch features
   - Integrate origin/main improvements
   - Ensure no functionality loss

### Phase 3: Conflict Resolution
For each conflict:
1. Understand both versions
2. Determine correct resolution strategy:
   - Keep ours (current branch)
   - Keep theirs (origin/main)
   - Manual merge (combine both)
3. Test resolution makes sense

### Phase 4: Post-Merge Validation
1. Run configure: `./bin/configure -GNinja -DCMAKE_BUILD_TYPE=Debug -Djank_test=on -Djank_local_clang=on`
2. Run compile: `./bin/compile`
3. Fix any breaking changes
4. Run tests: `./bin/test` and compare with baseline
5. Verify no new test failures

### Phase 5: Cleanup
1. Update this document with actual conflicts encountered
2. Document breaking changes fixed
3. Commit merge with detailed message

## CRITICAL MERGE STRATEGY (Updated)

**FIRST ATTEMPT FAILED** - Task agent removed nrepl-4 features!

### Correct Strategy for Second Attempt:
For EVERY conflict, must preserve BOTH functionalities:

1. **CMakeLists.txt - MUST HAVE BOTH:**
   - ✅ nrepl-4: `jank_target_wasm`, `jank_target_ios`, `jank_ios_jit` options
   - ✅ nrepl-4: `jank_core_modules` list (including nREPL server modules)
   - ✅ nrepl-4: WASM/iOS specific build configurations
   - ✅ origin/main: `jank_profile_gc`, `jank_force_phase_2` options
   - ✅ origin/main: Phase 2 improvements

2. **Code Files - MERGE NOT REPLACE:**
   - If nrepl-4 adds iOS/WASM code, KEEP IT
   - If origin/main adds new features, ADD THEM
   - NEVER choose one side completely - COMBINE

3. **Headers - ADD BOTH:**
   - Keep all includes from both sides
   - Merge all function signatures

### Resolution Rules:
- ❌ NEVER use `git checkout --theirs` or `--ours` blindly
- ✅ ALWAYS manually merge to include BOTH sets of changes
- ✅ When in doubt, include MORE not less

## Execution Log

### First Attempt (FAILED)
- Reason: Task agent chose sides instead of merging
- Lost: All iOS/WASM/nREPL functionality from nrepl-4
- Action: Aborted merge, restarting

### Second Attempt (SUCCESSFUL!)

**Status**: ALL 45 conflicts RESOLVED by properly merging BOTH sides' functionality!

**Files Resolved**: 45 total
- Build system: CMakeLists.txt (8 conflicts), bin/ar-merge, cmake files (3)
- Workflow files: 3
- Config: .gitignore
- Headers: 18 files (.hpp)
- Source: 20 files (.cpp)
- Clojure: 1 file (core.jank)

**Merge Strategy Applied**:
1. ✅ Kept ALL nrepl-4 features:
   - `jank_target_wasm`, `jank_target_ios`, `jank_ios_jit` options
   - `jank_core_modules` with nREPL server modules
   - All iOS/WASM conditional compilation (#ifdef blocks)
   - GC allocator support
   - WASM AOT compilation logic

2. ✅ Added ALL origin/main features:
   - `jank_profile_gc`, `jank_force_phase_2` options
   - Phase 2 build improvements
   - `-femulated-tls` and `-Wno-c2y-extensions` compiler flags
   - const qualifiers improvements
   - jtl::option<> return types for better error handling

3. ✅ Merge patterns used:
   - const qualifiers → Accepted from origin/main
   - Return types → Used jtl::option<> from origin/main
   - Platform conditionals → Kept from nrepl-4
   - New functions/features → Included from BOTH sides
   - Memory management → Kept nrepl-4's GC allocator

**Verification**:
```bash
grep -r "^<<<<<<< HEAD" . | wc -l  # Returns: 0 (no conflicts!)
```

**Critical Features Preserved**:
- ✅ iOS JIT compilation support
- ✅ WASM target support
- ✅ nREPL server modules (jank.nrepl-server.core, jank.nrepl-server.server)
- ✅ jank.export module
- ✅ All platform-specific build logic
- ✅ GC profiling from origin/main
- ✅ Phase 2 forcing from origin/main

**Next Steps**:
1. User will mark files as resolved with `git add`
2. Run build: `./bin/configure && ./bin/compile`
3. Run tests to verify no regressions
4. Commit merge

## Notes
- CRITICAL: Never delete `build/llvm-install/`
- Use exact build commands from CLAUDE.md
- Save all command outputs with `tee`
