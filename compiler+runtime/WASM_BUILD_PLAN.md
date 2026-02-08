# WASM Build System Improvement Plan

## Problem Statement

The current `emscripten-bundle` bash script has grown complex with manual dependency tracking that is:
1. **Error-prone**: Easy to miss dependencies (e.g., `libjank.a` changes not triggering relink)
2. **Hard to maintain**: Lots of `if` statements checking file timestamps
3. **Duplicated logic**: Similar timestamp checks repeated throughout the script
4. **Incomplete**: Some dependencies aren't tracked (e.g., header file changes)

## Current Dependencies (Manual Tracking)

```
Native jank compiler (build/jank)
    │
    ├──► clojure_core_generated.cpp ──► clojure_core_generated.o ─┐
    ├──► clojure_set_generated.cpp  ──► clojure_set_generated.o  ─┤
    └──► eita_generated.cpp         ──► eita_generated.o         ─┤
                                                                   │
                                        eita_entry.cpp ──► eita_entry.o
                                                                   │
libjank.a (from CMake) ────────────────────────────────────────────┤
libgc.a (from CMake) ──────────────────────────────────────────────┤
libjankzip.a (from CMake) ─────────────────────────────────────────┤
                                                                   │
                                    jank_runtime_prelinked.o ◄─────┘
                                              │
                                              ▼
                                    eita.js + eita.wasm
```

## Options Considered

### Option 1: GNU Make

**Pros:**
- Battle-tested, universally available
- Excellent dependency tracking
- Parallel builds (`make -j`)
- Well-understood by most developers

**Cons:**
- Makefile syntax can be awkward
- Variable handling is limited
- Recursive make has pitfalls
- Not as good for complex logic

**Example Makefile structure:**
```makefile
BUILD_DIR := build-wasm
NATIVE_JANK := build/jank

# Core library generation
$(BUILD_DIR)/%_generated.cpp: src/jank/clojure/%.jank $(NATIVE_JANK)
	$(NATIVE_JANK) run --codegen wasm-aot --save-cpp --save-cpp-path $@ $<

# Object compilation  
$(BUILD_DIR)/%.o: $(BUILD_DIR)/%.cpp
	em++ -c $< -o $@ $(EMCC_FLAGS)

# Final linking
$(BUILD_DIR)/$(MODULE).wasm: $(PRELINKED_RUNTIME) $(USER_OBJECTS) $(ENTRY_OBJ)
	em++ -o $@ $^ $(EMCC_LINK_FLAGS)
```

### Option 2: Ninja (via CMake)

**Pros:**
- Already using CMake for native build
- Ninja is extremely fast
- CMake handles complexity, Ninja handles speed
- Better integration with existing build

**Cons:**
- Would need to integrate jank codegen into CMake
- CMake custom commands can be verbose
- Two-stage build (CMake configure + Ninja build)

**Approach:**
- Add custom CMake targets for WASM bundle generation
- Use `add_custom_command` for jank → C++ codegen
- Let CMake track all dependencies automatically

### Option 3: Just (justfile)

**Pros:**
- Modern, readable syntax
- Good variable handling
- Cross-platform
- Easy to learn

**Cons:**
- Another tool to install
- Less powerful dependency tracking than Make
- Not as widely known

### Option 4: Ninja directly (without CMake)

**Pros:**
- Fastest incremental builds
- Simple, explicit dependency specification
- No configuration step

**Cons:**
- Need to generate ninja file manually or via script
- Less flexible than Make for complex logic
- Manual header dependency tracking

### Option 5: Bazel / Buck2

**Pros:**
- Hermetic builds
- Excellent caching (remote cache support)
- Handles complex dependency graphs well
- Content-addressable (rebuilds only what changed)

**Cons:**
- Steep learning curve
- Heavy tooling
- Overkill for this project size
- Would need to rewrite entire build

## Recommendation: Hybrid CMake + Ninja

**Rationale:**
1. Already using CMake for native + WASM `libjank.a` builds
2. CMake can track header dependencies automatically
3. Ninja provides fast incremental builds
4. Can integrate jank codegen as custom commands

### Implementation Plan

#### Phase 1: Add WASM Bundle Target to CMakeLists.txt

```cmake
# In CMakeLists.txt, when jank_target_wasm is ON

# Custom command to generate C++ from jank source
function(jank_wasm_codegen target_name jank_source output_cpp)
  add_custom_command(
    OUTPUT ${output_cpp}
    COMMAND ${NATIVE_JANK} run 
            --codegen wasm-aot 
            --module-path ${CMAKE_SOURCE_DIR}/src/jank
            --save-cpp --save-cpp-path ${output_cpp}
            ${jank_source}
    DEPENDS ${jank_source} ${NATIVE_JANK}
    COMMENT "Generating WASM C++ from ${jank_source}"
  )
endfunction()

# Generate core libraries
jank_wasm_codegen(clojure_core 
  ${CMAKE_SOURCE_DIR}/src/jank/clojure/core.jank
  ${CMAKE_BINARY_DIR}/clojure_core_generated.cpp)

jank_wasm_codegen(clojure_set
  ${CMAKE_SOURCE_DIR}/src/jank/clojure/set.jank  
  ${CMAKE_BINARY_DIR}/clojure_set_generated.cpp)

# User module target (parameterized)
function(add_jank_wasm_module module_name jank_source)
  jank_wasm_codegen(${module_name} ${jank_source}
    ${CMAKE_BINARY_DIR}/${module_name}_generated.cpp)
  
  # Generate entrypoint
  configure_file(
    ${CMAKE_SOURCE_DIR}/cmake/wasm_entry.cpp.in
    ${CMAKE_BINARY_DIR}/${module_name}_entry.cpp
    @ONLY)
  
  # Create executable
  add_executable(${module_name}_wasm
    ${CMAKE_BINARY_DIR}/${module_name}_generated.cpp
    ${CMAKE_BINARY_DIR}/${module_name}_entry.cpp
    ${CMAKE_BINARY_DIR}/clojure_core_generated.cpp
    ${CMAKE_BINARY_DIR}/clojure_set_generated.cpp)
  
  target_link_libraries(${module_name}_wasm jank_lib jankzip_lib gc)
  
  set_target_properties(${module_name}_wasm PROPERTIES
    SUFFIX ".js"
    LINK_FLAGS "${EMCC_LINK_FLAGS}")
endfunction()
```

#### Phase 2: Simplify emscripten-bundle

Reduce `emscripten-bundle` to a thin wrapper:
```bash
#!/bin/bash
# Just invokes CMake with the right options

jank_source="$1"
module_name=$(basename "$jank_source" .jank)

cmake -B build-wasm \
  -Djank_target_wasm=on \
  -DWASM_MODULE_NAME="$module_name" \
  -DWASM_MODULE_SOURCE="$jank_source"

cmake --build build-wasm --target "${module_name}_wasm"
```

#### Phase 3: Handle Native Jank Dependency

The tricky part is that `NATIVE_JANK` must be built first (native build), then used for WASM codegen.

**Solution: ExternalProject or superbuild pattern**
```cmake
if(jank_target_wasm)
  # Find or build native jank first
  find_program(NATIVE_JANK jank PATHS ${CMAKE_SOURCE_DIR}/build)
  if(NOT NATIVE_JANK)
    message(FATAL_ERROR "Native jank not found. Build native first: ./bin/compile")
  endif()
endif()
```

### Migration Path

1. **Week 1**: Add basic CMake functions for jank codegen
2. **Week 2**: Create `add_jank_wasm_module()` CMake function
3. **Week 3**: Test with sample modules, verify dependency tracking
4. **Week 4**: Deprecate complex logic in `emscripten-bundle`, make it a thin wrapper

### Alternative: Keep Script, Use Make for Dependencies

If CMake integration is too complex, a simpler approach:

Create `build-wasm/Makefile` that `emscripten-bundle` generates:

```makefile
# Auto-generated by emscripten-bundle

NATIVE_JANK := ../build/jank
MODULE := eita

include wasm.mk  # Common rules

$(MODULE).wasm: $(MODULE)_entry.o $(MODULE)_generated.o \
                clojure_core_generated.o clojure_set_generated.o \
                libjank.a libgc.a libjankzip.a
	$(EMCC_LINK)
```

Then `emscripten-bundle` just:
1. Generates the Makefile
2. Runs `make -C build-wasm`

## Decision Matrix

| Criteria | Make | CMake+Ninja | Just | Bazel |
|----------|------|-------------|------|-------|
| Dependency tracking | ★★★★☆ | ★★★★★ | ★★★☆☆ | ★★★★★ |
| Ease of implementation | ★★★★☆ | ★★★☆☆ | ★★★★☆ | ★★☆☆☆ |
| Integration with existing | ★★★☆☆ | ★★★★★ | ★★★☆☆ | ★☆☆☆☆ |
| Maintenance burden | ★★★☆☆ | ★★★★☆ | ★★★★☆ | ★★☆☆☆ |
| Build speed | ★★★☆☆ | ★★★★★ | ★★★☆☆ | ★★★★★ |
| **Total** | **15** | **21** | **16** | **15** |

## Conclusion

**Recommended approach: CMake + Ninja integration (Option 2)**

This leverages the existing CMake infrastructure, provides automatic header dependency tracking, and integrates naturally with the current build system. The `emscripten-bundle` script becomes a simple wrapper that invokes CMake.

For a quicker win, **Option 1 (Make)** could be implemented as an intermediate step - generate a Makefile from `emscripten-bundle` and use `make` for the actual dependency tracking.
