# iOS Device JIT with Full AOT Library (libvybe_aot.a)

## Overview
For iOS device JIT mode, we use pre-compiled AOT modules (`libvybe_aot.a`) instead of JIT compiling at runtime. This avoids the memory limit issue (~1850 MB) that occurs when JIT compiling large modules.

## Key Configuration

### project-jit-device.yml linker flags:
```yaml
OTHER_LDFLAGS:
  # ... frameworks ...
  - "-Wl,-force_load,$(PROJECT_DIR)/build-iphoneos-jit/libjank.a"  # jank runtime (needs force_load)
  - "-ljankzip"
  - "-lgc"
  - "-lfolly"
  - "-lvybe_aot"      # AOT modules (regular link, NOT force_load)
  - "-lllvm_merged"   # LLVM for REPL JIT
```

### Important: Do NOT use force_load for libvybe_aot.a
- Using `-Wl,-force_load` causes duplicate symbol errors with tinygltf
- Regular `-lvybe_aot` works because `jank_aot_init()` creates references to all modules
- The regular AOT project (project.yml) also uses `-lvybe_aot` without force_load

### libvybe_aot.a contains:
- clojure_core_generated.o
- clojure_set_generated.o
- clojure_string_generated.o
- clojure_template_generated.o
- clojure_test_generated.o
- clojure_walk_generated.o
- jank_aot_init.o
- vybe_sdf_greeting_generated.o
- vybe_sdf_ios_generated.o
- vybe_sdf_math_generated.o
- vybe_sdf_shader_generated.o
- vybe_sdf_state_generated.o
- vybe_sdf_ui_generated.o
- vybe_util_generated.o

## Build Process
1. AOT compile modules using `ios-bundle` (creates .o files)
2. Package into libvybe_aot.a
3. Link with `-lvybe_aot` in Xcode project
4. JIT is still available for nREPL eval (via libjank.a + lllvm_merged)

## Notes on tinygltf duplicate symbols
- sdf_engine_impl.cpp includes TINYGLTF_IMPLEMENTATION
- Some AOT modules also include the same implementation
- This produces linker warnings but not errors
- The linker picks one copy and the app works correctly
