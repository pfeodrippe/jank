cmake_minimum_required(VERSION 3.21)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(
    FATAL_ERROR
    "In-source builds are not supported. "
    "Please read README.md before trying to build this project. "
    "You may need to delete 'CMakeCache.txt' and 'CMakeFiles/' first."
  )
endif()

set(CMAKE_MODULE_PATH
  ${CMAKE_MODULE_PATH}
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake"
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules"
)

# LTO is explicitly not used, since it leads to runtime symbol lookup issues with the JIT.
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION FALSE)

project(
    jank
    VERSION 0.1
    DESCRIPTION "The native Clojure dialect on LLVM."
    HOMEPAGE_URL "https://jank-lang.org/"
    # C is needed here, due to a clang issue:
    # https://stackoverflow.com/questions/71740678/cmake-error-in-findterminfo-with-clang-15-on-macos
    LANGUAGES C CXX
)

# BDWGC redefines BUILD_SHARED_LIBS as an option. The old policy behavior here
# is to reset the value to the new default defined, which is ON. However, we
# want it to keep the value we've already specified, to force BDWGC to build
# as a static lib, so we choose the new behavior.
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
cmake_policy(SET CMP0077 NEW)

# Note: emscripten should handle C++ include paths automatically via its toolchain
# We avoid adding extra -isystem flags that could conflict with emscripten's libc++
# The order of system includes is critical: c++/v1 MUST come before sysroot/include
if(jank_target_wasm)
  # Clear any default system include ordering that CMake might add
  # and let emscripten's toolchain handle the C++ includes properly
  set(CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES "")
endif()

# All options are available here.
option(jank_local_clang "Whether or not to use a local Clang/LLVM source build" OFF)
option(jank_install_local_clang "Whether or not to install the local Clang/LLVM alongside jank" ON)
option(jank_coverage "Enable code coverage measurement" OFF)
option(jank_analyze "Enable static analysis" OFF)
option(jank_test "Enable jank's test suite" OFF)
option(jank_unity_build "Optimize translation unit compilation for the number of cores" OFF)
option(jank_target_wasm "Experimental wasm32-unknown-emscripten build" OFF)
option(jank_debug_gc "Enable GC debug assertions" OFF)
set(jank_sanitize "none" CACHE STRING "The type of Clang sanitization to use (or none)")
set(jank_resource_dir
  "../lib/jank/${CMAKE_PROJECT_VERSION}"
  CACHE STRING
  "Where jank's runtime files are installed. Relative paths are based on the runtime jank binary path")
set(jank_core_modules
  clojure.core
  jank.nrepl-server.core
  jank.nrepl-server.server
  jank.export
)

set(jank_core_module_paths "")
set(jank_core_module_sources "")
set(jank_core_library_objects "")
foreach(module ${jank_core_modules})
  string(REPLACE "." "/" module_rel_path ${module})
  string(REPLACE "-" "_" module_rel_path ${module_rel_path})
  list(APPEND jank_core_module_paths ${module_rel_path})
  set(source_path "${CMAKE_SOURCE_DIR}/src/jank/${module_rel_path}.jank")
  if(NOT EXISTS ${source_path})
    message(FATAL_ERROR "Missing bundled module source: ${source_path}")
  endif()
  list(APPEND jank_core_module_sources ${source_path})
  set(object_path "${CMAKE_BINARY_DIR}/core-libs/${module_rel_path}.o")
  list(APPEND jank_core_library_objects ${object_path})
endforeach()

find_package(Git REQUIRED)
execute_process(
  COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  OUTPUT_VARIABLE jank_git_hash
  ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE
)

set(jank_version "jank-${CMAKE_PROJECT_VERSION}-${jank_git_hash}")

if(jank_target_wasm)
  # Don't force C++ standard for wasm - let emscripten handle it
  set(jank_cxx_standard "")
else()
  set(jank_cxx_standard cxx_std_20)
endif()

# We need different flags for linking whole libraries on different OSes.
# We need to be mindful to wrap each library with ${jank_link_whole_start} and ${jank_link_whole_end}.
# For example:
#   ${jank_link_whole_start} jank_lib ${jank_link_whole_end}
#   ${jank_link_whole_start} nanobench_lib ${jank_link_whole_end}
#
if(${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
  set(jank_link_whole_start "-Wl,--whole-archive")
  set(jank_link_whole_end "-Wl,--no-whole-archive")
elseif(${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
  set(jank_link_whole_start "-Wl,-force_load")
  set(jank_link_whole_end "")
elseif(${CMAKE_SYSTEM_NAME} STREQUAL "Emscripten")
  set(jank_link_whole_start "")
  set(jank_link_whole_end "")
else()
  message(FATAL_ERROR "Unsupported OS: " ${CMAKE_SYSTEM_NAME})
endif()

if(jank_target_wasm AND NOT ${CMAKE_SYSTEM_NAME} STREQUAL "Emscripten")
  message(WARNING "jank_target_wasm is ON but this build is not using the Emscripten toolchain. The configuration may fail.")
endif()

if(jank_target_wasm)
  # Simpler flags for wasm to avoid conflicts with emscripten's toolchain
  # -fPIC is required for MAIN_MODULE dynamic linking support
  set(
    jank_aot_compiler_flags
    -Wall
    -Wno-deprecated-declarations
    -fno-common
    -frtti
    -fexceptions
    -fPIC
  )
else()
  set(
    jank_aot_compiler_flags
    -Wall -Wextra -Wpedantic
    -Wfloat-equal -Wuninitialized -Wswitch-enum -Wnon-virtual-dtor
    -Wold-style-cast -Wno-gnu-case-range -Wno-c99-designator
    -Wno-gnu-conditional-omitted-operand
    -Wno-implicit-fallthrough
    -Wno-covered-switch-default
    -Wno-invalid-offsetof
    -Wno-deprecated-declarations
    -Wno-c++23-extensions
    -Wno-macro-redefined
    -Wno-c23-extensions
    -fno-common
    -frtti
    -fexceptions
    #-ftime-trace
  )
endif()

if(jank_target_wasm)
  # Emscripten handles its own C++ standard - just set minimal defines
  # Disable format header completely due to math.h macro conflicts in emscripten 4.0.20
  # Also undefine math.h macros to allow std:: versions to work
  set(
    jank_common_compiler_flags
    -D_LIBCPP_HAS_NO_INCOMPLETE_FORMAT
    -D_LIBCPP_HAS_NO_LOCALIZATION
    -DIMMER_HAS_LIBGC=1 -DIMMER_TAGGED_NODE=0 -DHAVE_CXX14=1
    -DFOLLY_HAVE_JEMALLOC=0 -DFOLLY_HAVE_TCMALLOC=0 -DFOLLY_ASSUME_NO_JEMALLOC=1 -DFOLLY_ASSUME_NO_TCMALLOC=1
    -DJANK_TARGET_EMSCRIPTEN
  )
  # Set C++20 via CMake property instead of flag
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  # Tell CMake to use only regular includes, not system includes for third-party libs
  set(CMAKE_NO_SYSTEM_FROM_IMPORTED ON)
else()
  set(
    jank_common_compiler_flags
    -std=gnu++20
    -DIMMER_HAS_LIBGC=1 -DIMMER_TAGGED_NODE=0 -DHAVE_CXX14=1
    -DCPPINTEROP_USE_REPL
    -DFOLLY_HAVE_JEMALLOC=0 -DFOLLY_HAVE_TCMALLOC=0 -DFOLLY_ASSUME_NO_JEMALLOC=1 -DFOLLY_ASSUME_NO_TCMALLOC=1
    #-DLIBASSERT_STATIC_DEFINE=1
    #-stdlib=libc++
  )
endif()

set(
  jank_linker_flags
  #-stdlib=libc++ -lc++abi
  )

if (NOT jank_local_clang)
  list(APPEND jank_linker_flags -L/opt/homebrew/opt/llvm/lib -L/opt/homebrew/opt/llvm/lib/c++ -L/opt/homebrew/opt/llvm/lib/unwind)
endif ()

# If the user opts into a specific build type, we will provide sane defaults. However, for
# packaging, we can expect that CMAKE_BUILD_TYPE=None will be used and packagers will provide
# the exact flags they desire.
#
# https://wiki.archlinux.org/title/CMake_package_guidelines
#
# Aside from setting these defaults, we don't rely on any CMAKE_BUILD_TYPE being set.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  list(APPEND jank_common_compiler_flags -Werror -O0 -g)
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
  list(APPEND jank_common_compiler_flags -O2 -DNDEBUG)
elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
  list(APPEND jank_common_compiler_flags -Og -g -DNDEBUG)
endif()

if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(jank_enable_phase_2 ON)
endif()

if(jank_target_wasm)
  set(jank_enable_phase_2 OFF)
endif()

if(jank_test)
  list(APPEND jank_common_compiler_flags -DJANK_TEST)
endif()

include(cmake/coverage.cmake)
include(cmake/analyze.cmake)
include(cmake/sanitization.cmake)

# Enable easier tooling during development.
set(CMAKE_EXPORT_COMPILE_COMMANDS on)
# Also include the system includes in the compile_commands.json so that weird
# systems like NixOS can have tooling work properly.
set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES ${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES})

set(jank_jit_compiler_flags "")

if(NOT jank_target_wasm)
  # ---- Clang/LLVM ----
  include(cmake/llvm.cmake)

if(
  NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
  OR
  NOT CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL CLANG_PACKAGE_VERSION
)
  message(
    FATAL_ERROR
    "Found Clang ${CLANG_PACKAGE_VERSION} to embed in jank, "
    "but trying to use ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} to compile it. "
    "Please ensure you're using the same Clang to compile jank as you are embedding in jank. "
    "Follow the build.md documentation."
  )
endif()

function(jank_hook_llvm target)
  set(tgt "${target}")
  target_include_directories("${tgt}" SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
  target_compile_definitions("${tgt}" PRIVATE ${LLVM_DEFINITIONS})
  target_link_directories("${tgt}" PRIVATE ${llvm_dir}/lib)
endfunction()
# ---- Clang/LLVM ----

# ---- Clang JIT environment ----
# We need to know some info about Clang, at runtime, which the JIT itself
# has a hard time finding. Since we're expecting jank to go hand in hand with
# the Clang which was used to build it, we bake this info into jank.
#
# This should greatly simplify distribution, since package maintainers
# can configure jank correctly during the build phase and then it'll just
# continue to work with that setup once it's installed.
execute_process(
  COMMAND bash -c "${CMAKE_CXX_COMPILER} -xc++ -E -v /dev/null 2>&1 | sed -n -e '/^.include/,\${' -e '/^ \\/.*/p' -e '}' | sed 's/^[[:space:]]\\{1,\\}/-I/' | sed 's/ (framework directory)//'"
  RESULT_VARIABLE clang_system_includes_result
  OUTPUT_VARIABLE clang_system_includes_unescaped
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT clang_system_includes_result EQUAL 0)
  message(FATAL_ERROR
    "Failed to extract include paths from ${CMAKE_CXX_COMPILER}:\n"
    "  exit code: ${clang_system_includes_result}"
  )
endif()
string(REPLACE "\n" " " clang_system_include_dirs "${clang_system_includes_unescaped}")
separate_arguments(clang_system_include_dirs)

set(clang_system_include_flags "")
foreach(dir ${clang_system_include_dirs})
  list(APPEND clang_system_include_flags ${dir})
endforeach()

execute_process(
  COMMAND ${CMAKE_CXX_COMPILER} -print-resource-dir
  RESULT_VARIABLE clang_resource_dir_result
  OUTPUT_VARIABLE clang_resource_dir
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT clang_resource_dir_result EQUAL 0)
  message(FATAL_ERROR
    "Failed to extract resource dir ${CMAKE_CXX_COMPILER}:\n"
    "  exit code: ${clang_resource_dir_result}"
  )
endif()
list(APPEND jank_common_compiler_flags
  -DJANK_RESOURCE_DIR="${jank_resource_dir}"
  -DJANK_CLANG_PATH="${CMAKE_CXX_COMPILER}"
  -DJANK_CLANG_MAJOR_VERSION="${CLANG_VERSION_MAJOR}"
  -DJANK_CLANG_RESOURCE_DIR="${clang_resource_dir}"
)

set(
  jank_jit_compiler_flags
  # We don't actually want to see any warnings for JIT compiled code.
  -w
  -fwrapv
  -fno-stack-protector
  -fPIC
  -Xclang -fno-validate-pch
  -Xclang -fno-pch-timestamp
  ${LLVM_DEFINITIONS_LIST}
  ${clang_system_include_flags}
)
  # ---- Clang JIT environment ----
else()
  function(jank_hook_llvm target)
  endfunction()
endif()

if(NOT jank_target_wasm)
  # ---- libnanobench.a ----
  # nanobench uses a single header for both .hpp and .cpp inclusion, based on
  # whether a define has been set. This doesn't work with jank's pre-compiled
  # headers, so I've manually split the two up and included them here.

  set(
    nanobench_sources
    third-party/nanobench/src/nanobench.cpp
  )

  add_library(nanobench_lib STATIC ${nanobench_sources})

  # Not our code. Don't try to lint it.
  set_source_files_properties(${nanobench_sources} PROPERTIES SKIP_LINTING ON)

  target_include_directories(
      nanobench_lib
      SYSTEM
      PUBLIC
      "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third-party/nanobench/include>"
  )

  set_property(TARGET nanobench_lib PROPERTY OUTPUT_NAME nanobench)

  if(jank_cxx_standard)
    target_compile_features(nanobench_lib PUBLIC ${jank_cxx_standard})
  endif()
  if(jank_target_wasm)
    # For wasm, use minimal flags to avoid conflicts with emscripten
    target_compile_options(nanobench_lib PUBLIC -DIMMER_HAS_LIBGC=1 -DIMMER_TAGGED_NODE=0 -DHAVE_CXX14=1)
  else()
    target_compile_options(nanobench_lib PUBLIC ${jank_common_compiler_flags} ${jank_aot_compiler_flags})
  endif()
  target_link_options(nanobench_lib PRIVATE ${jank_linker_flags})

  #set_target_properties(nanobench_lib PROPERTIES LINK_FLAGS_RELEASE "-s")

  # Symbol exporting for JIT.
  set_target_properties(nanobench_lib PROPERTIES ENABLE_EXPORTS 1)
  # ---- libnanobench.a ----
endif()

# ---- libjankzip.a ----

set(
  jankzip_sources
  third-party/zip/src/zip.c
)

add_library(jankzip_lib STATIC ${jankzip_sources})

# Not our code. Don't try to lint it.
set_source_files_properties(${jankzip_sources} PROPERTIES SKIP_LINTING ON)

target_include_directories(
    jankzip_lib
    SYSTEM
    PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third-party/zip/src>"
)

set_property(TARGET jankzip_lib PROPERTY OUTPUT_NAME jankzip)

target_compile_options(jankzip_lib PUBLIC -w)

#set_target_properties(jankzip_lib PROPERTIES LINK_FLAGS_RELEASE "-s")

# Symbol exporting for JIT.
set_target_properties(jankzip_lib PROPERTIES ENABLE_EXPORTS 1)
# ---- libjankzip.a ----

# The sources for jank are used for both the compiler and its tests, so
# they're just pulled into a static library. It's important that this
# remains a static lib, since these symbols need to be accessible in the
# compiler's runtime by the JIT compiler.
#
# This is libjank.a, but the final libjank-standalone.a will be merged
# by ar-merge to contain all of the other static libs we build, too. We
# merge them so that consumption of jank as a lib is much easier. Rather
# than bringing in libjank and 10 other libs, users just need libjank-standalone.
#
# However, merging static libs isn't a well supported operation. Generally,
# it involves unpacking each archive and then buliding a new archive from
# all of the object files unpacked from all archives. The main problem is
# that we don't know how many archives there will be and what exactly they
# will be called at configuration time. CMake does not present this information
# until build time. So, we need to be crafty.
#
# We tackle this by providing our own AR (archive) program, which wraps whatever
# CMake was going to use (probably llvm-ar). Our custom script keeps track of
# each archive created and all of the object files within. It also supports
# a couple of other custom commands, including a command to merge all of the
# object files seen into the final library.

configure_file(bin/ar-merge ar-merge USE_SOURCE_PERMISSIONS @ONLY)
set(CMAKE_AR "${CMAKE_BINARY_DIR}/ar-merge")
set(jank_lib_standalone_deps
  jank_lib
  jankzip_lib
  bdwgc::gc
)
if(NOT jank_target_wasm)
  list(APPEND jank_lib_standalone_deps Boost::multiprecision folly_lib bdwgc::gctba)
endif()
if(NOT jank_target_wasm)
  list(APPEND jank_lib_standalone_deps nanobench_lib clangCppInterOp cpptrace::cpptrace ftxui::screen ftxui::dom)
endif()

if(NOT jank_target_wasm)
  add_custom_command(
    DEPENDS ${jank_lib_standalone_deps}
    OUTPUT ${CMAKE_BINARY_DIR}/libjank-standalone-phase-1.a
    COMMAND_EXPAND_LISTS
    VERBATIM
    COMMAND "${CMAKE_BINARY_DIR}/ar-merge" merge
  )
  add_custom_target(
    jank_lib_standalone_phase_1
    ALL
    DEPENDS ${CMAKE_BINARY_DIR}/libjank-standalone-phase-1.a
  )

  add_custom_command(
    DEPENDS ${jank_lib_standalone_deps}
            jank_core_libraries
    OUTPUT ${CMAKE_BINARY_DIR}/libjank-standalone.a
    COMMAND_EXPAND_LISTS
    VERBATIM
    COMMAND "${CMAKE_BINARY_DIR}/ar-merge" merge-phase-2
  )
  add_custom_target(
    jank_lib_standalone_phase_2
    ALL
    DEPENDS ${CMAKE_BINARY_DIR}/libjank-standalone.a
  )
endif()

# ---- Other dependencies ----
# We want all deps built as static libs.
set(BUILD_SHARED_LIBS OFF)

include(FetchContent)
include(cmake/dependency/bdwgc.cmake)
if(NOT jank_target_wasm)
  include(cmake/dependency/ftxui.cmake)
  include(cmake/dependency/cpptrace.cmake)
  include(cmake/dependency/cppinterop.cmake)
  include(cmake/dependency/boost-multiprecision.cmake)
else()
  message(STATUS "Skipping ftxui/cpptrace/boost-multiprecision dependencies for wasm build")

  # ---- CppInterOp WASM build ----
  # Use pre-built CppInterOp and LLVM WASM libraries (minimal build)
  set(WASM_LLVM_BUILD_DIR "$ENV{HOME}/dev/jank/wasm-llvm-build/llvm-build-minimal")
  set(WASM_CPPINTEROP_BUILD_DIR "$ENV{HOME}/dev/jank/wasm-llvm-build/cppinterop-build")

  if(EXISTS "${WASM_CPPINTEROP_BUILD_DIR}/lib/libclangCppInterOp.a")
    message(STATUS "Found CppInterOp WASM build at ${WASM_CPPINTEROP_BUILD_DIR}")
    set(JANK_WASM_HAS_CPPINTEROP TRUE)

    # Import CppInterOp as a static library
    add_library(clangCppInterOp_wasm STATIC IMPORTED)
    set_target_properties(clangCppInterOp_wasm PROPERTIES
      IMPORTED_LOCATION "${WASM_CPPINTEROP_BUILD_DIR}/lib/libclangCppInterOp.a"
    )

    # LLVM/Clang include directories for WASM
    set(WASM_LLVM_INCLUDE_DIRS
      "${WASM_LLVM_BUILD_DIR}/include"
      "$ENV{HOME}/dev/llvm-project/llvm/include"
      "$ENV{HOME}/dev/llvm-project/clang/include"
      "${WASM_LLVM_BUILD_DIR}/tools/clang/include"
    )
  else()
    message(STATUS "CppInterOp WASM build not found - eval will not be available")
    set(JANK_WASM_HAS_CPPINTEROP FALSE)
  endif()
  # ---- CppInterOp WASM build ----
endif()

if(NOT jank_target_wasm)
  find_package(OpenSSL REQUIRED COMPONENTS Crypto)

  find_path(
    jank_boost_include_dir
    NAMES boost/asio.hpp
    HINTS ENV BOOST_ROOT
    PATH_SUFFIXES include
    PATHS /opt/homebrew/include /usr/local/include /usr/include
  )
  if(NOT jank_boost_include_dir)
    message(FATAL_ERROR "Unable to find boost/asio.hpp. Install Boost headers or set BOOST_ROOT.")
  endif()
else()
  set(OPENSSL_INCLUDE_DIR "")
  set(jank_boost_include_dir "")
endif()
# ---- Other dependencies ----

# ---- libjank.a ----
# For WASM: include full runtime sources needed for clojure.core and clojure.set
# For native: include everything including AOT/codegen/nREPL

# Common runtime sources needed for both WASM and native builds
set(jank_runtime_common_sources
  src/cpp/jank/hash.cpp
  src/cpp/jank/util/scope_exit.cpp
  src/cpp/jank/util/escape.cpp
  src/cpp/jtl/string_builder.cpp
  src/cpp/jank/util/string.cpp
  src/cpp/jank/util/fmt.cpp
  src/cpp/jank/util/fmt/print.cpp
  src/cpp/jank/util/path.cpp
  src/cpp/jank/util/try.cpp
  src/cpp/jank/profile/time.cpp
  src/cpp/jank/error.cpp
  src/cpp/jank/error/lex.cpp
  src/cpp/jank/error/parse.cpp
  src/cpp/jank/error/analyze.cpp
  src/cpp/jank/error/runtime.cpp
  src/cpp/jank/error/system.cpp
  src/cpp/jank/read/source.cpp
  src/cpp/jank/read/lex.cpp
  src/cpp/jank/read/parse.cpp
  src/cpp/jank/read/reparse.cpp
  src/cpp/jank/runtime/detail/type.cpp
  src/cpp/jank/runtime/core.cpp
  src/cpp/jank/runtime/core/equal.cpp
  src/cpp/jank/runtime/core/to_string.cpp
  src/cpp/jank/runtime/core/seq.cpp
  src/cpp/jank/runtime/core/truthy.cpp
  src/cpp/jank/runtime/core/munge.cpp
  src/cpp/jank/runtime/core/integer_cache.cpp
  src/cpp/jank/runtime/core/real_cache.cpp
  src/cpp/jank/runtime/core/arena.cpp
  src/cpp/jank/runtime/core/debug_allocator.cpp
  src/cpp/jank/runtime/core/optimization.cpp
  src/cpp/jank/runtime/core/math.cpp
  src/cpp/jank/runtime/core/meta.cpp
  src/cpp/jank/runtime/module/loader.cpp
  src/cpp/jank/runtime/object.cpp
  src/cpp/jank/runtime/detail/native_array_map.cpp
  src/cpp/jank/runtime/context.cpp
  src/cpp/jank/runtime/ns.cpp
  src/cpp/jank/runtime/var.cpp
  src/cpp/jank/runtime/hot_reload.cpp
  src/cpp/jank/runtime/obj/nil.cpp
  src/cpp/jank/runtime/obj/number.cpp
  src/cpp/jank/runtime/obj/native_function_wrapper.cpp
  src/cpp/jank/runtime/obj/jit_function.cpp
  src/cpp/jank/runtime/obj/jit_closure.cpp
  src/cpp/jank/runtime/obj/multi_function.cpp
  src/cpp/jank/runtime/obj/native_pointer_wrapper.cpp
  src/cpp/jank/runtime/obj/symbol.cpp
  src/cpp/jank/runtime/obj/keyword.cpp
  src/cpp/jank/runtime/obj/tagged_literal.cpp
  src/cpp/jank/runtime/obj/re_pattern.cpp
  src/cpp/jank/runtime/obj/re_matcher.cpp
  src/cpp/jank/runtime/obj/uuid.cpp
  src/cpp/jank/runtime/obj/inst.cpp
  src/cpp/jank/runtime/obj/opaque_box.cpp
  src/cpp/jank/runtime/obj/user_type.cpp
  src/cpp/jank/runtime/obj/character.cpp
  src/cpp/jank/runtime/obj/big_integer.cpp
  src/cpp/jank/runtime/obj/big_decimal.cpp
  src/cpp/jank/runtime/obj/persistent_list.cpp
  src/cpp/jank/runtime/obj/persistent_vector.cpp
  src/cpp/jank/runtime/obj/persistent_vector_sequence.cpp
  src/cpp/jank/runtime/obj/persistent_array_map.cpp
  src/cpp/jank/runtime/obj/transient_array_map.cpp
  src/cpp/jank/runtime/obj/persistent_hash_map.cpp
  src/cpp/jank/runtime/obj/transient_hash_map.cpp
  src/cpp/jank/runtime/obj/persistent_sorted_map.cpp
  src/cpp/jank/runtime/obj/transient_sorted_map.cpp
  src/cpp/jank/runtime/obj/detail/base_persistent_map.cpp
  src/cpp/jank/runtime/obj/detail/base_persistent_map_sequence.cpp
  src/cpp/jank/runtime/obj/transient_vector.cpp
  src/cpp/jank/runtime/obj/persistent_hash_set.cpp
  src/cpp/jank/runtime/obj/transient_hash_set.cpp
  src/cpp/jank/runtime/obj/persistent_sorted_set.cpp
  src/cpp/jank/runtime/obj/transient_sorted_set.cpp
  src/cpp/jank/runtime/obj/persistent_string.cpp
  src/cpp/jank/runtime/obj/persistent_string_sequence.cpp
  src/cpp/jank/runtime/obj/cons.cpp
  src/cpp/jank/runtime/obj/range.cpp
  src/cpp/jank/runtime/obj/integer_range.cpp
  src/cpp/jank/runtime/obj/repeat.cpp
  src/cpp/jank/runtime/obj/ratio.cpp
  src/cpp/jank/runtime/obj/iterator.cpp
  src/cpp/jank/runtime/obj/lazy_sequence.cpp
  src/cpp/jank/runtime/obj/chunk_buffer.cpp
  src/cpp/jank/runtime/obj/array_chunk.cpp
  src/cpp/jank/runtime/obj/chunked_cons.cpp
  src/cpp/jank/runtime/obj/detail/iterator_sequence.cpp
  src/cpp/jank/runtime/obj/native_array_sequence.cpp
  src/cpp/jank/runtime/obj/native_vector_sequence.cpp
  src/cpp/jank/runtime/obj/atom.cpp
  src/cpp/jank/runtime/obj/arena.cpp
  src/cpp/jank/runtime/obj/volatile.cpp
  src/cpp/jank/runtime/obj/delay.cpp
  src/cpp/jank/runtime/obj/reduced.cpp
  src/cpp/jank/runtime/behavior/callable.cpp
  src/cpp/jank/runtime/behavior/metadatable.cpp
)

set(jank_lib_sources)

# Add runtime sources for both WASM and native
if(jank_target_wasm)
  # WASM build: full runtime
  list(APPEND jank_lib_sources ${jank_runtime_common_sources})
  list(APPEND jank_lib_sources
    src/cpp/jank/util/environment_wasm.cpp
    src/cpp/jank/wasm_stub.cpp
    src/cpp/jank/runtime/perf_wasm.cpp
    # WASM-specific C API (no LLVM deps)
    src/cpp/jank/c_api_wasm.cpp
    # Core native functions (original file with WASM guards)
    src/cpp/clojure/core_native.cpp
    # String native functions for clojure.string
    src/cpp/clojure/string_native.cpp
    # GC stub for missing gc_badalc.cc
    src/cpp/jank/gc_wasm_stub.cpp
    # Stubs for native-only functionality
    src/cpp/jank/wasm_native_stubs.cpp
  )

  # If CppInterOp WASM build is available, enable analyzer, codegen, and eval
  if(JANK_WASM_HAS_CPPINTEROP)
    message(STATUS "Enabling analyzer and eval for WASM build (CppInterOp available)")
    list(APPEND jank_lib_sources
      # Analyze files
      src/cpp/jank/analyze/processor.cpp
      src/cpp/jank/analyze/expression.cpp
      src/cpp/jank/analyze/expression_hash.cpp
      src/cpp/jank/analyze/expr/vector.cpp
      src/cpp/jank/analyze/expr/recursion_reference.cpp
      src/cpp/jank/analyze/expr/map.cpp
      src/cpp/jank/analyze/expr/do.cpp
      src/cpp/jank/analyze/expr/var_deref.cpp
      src/cpp/jank/analyze/expr/recur.cpp
      src/cpp/jank/analyze/expr/named_recursion.cpp
      src/cpp/jank/analyze/expr/local_reference.cpp
      src/cpp/jank/analyze/expr/def.cpp
      src/cpp/jank/analyze/expr/let.cpp
      src/cpp/jank/analyze/expr/letfn.cpp
      src/cpp/jank/analyze/expr/throw.cpp
      src/cpp/jank/analyze/expr/if.cpp
      src/cpp/jank/analyze/expr/list.cpp
      src/cpp/jank/analyze/expr/primitive_literal.cpp
      src/cpp/jank/analyze/expr/call.cpp
      src/cpp/jank/analyze/expr/set.cpp
      src/cpp/jank/analyze/expr/var_ref.cpp
      src/cpp/jank/analyze/expr/function.cpp
      src/cpp/jank/analyze/expr/try.cpp
      src/cpp/jank/analyze/expr/case.cpp
      src/cpp/jank/analyze/expr/cpp_raw.cpp
      src/cpp/jank/analyze/expr/cpp_type.cpp
      src/cpp/jank/analyze/expr/cpp_value.cpp
      src/cpp/jank/analyze/expr/cpp_cast.cpp
      src/cpp/jank/analyze/expr/cpp_call.cpp
      src/cpp/jank/analyze/expr/cpp_constructor_call.cpp
      src/cpp/jank/analyze/expr/cpp_member_call.cpp
      src/cpp/jank/analyze/expr/cpp_member_access.cpp
      src/cpp/jank/analyze/expr/cpp_builtin_operator_call.cpp
      src/cpp/jank/analyze/expr/cpp_box.cpp
      src/cpp/jank/analyze/expr/cpp_unbox.cpp
      src/cpp/jank/analyze/expr/cpp_new.cpp
      src/cpp/jank/analyze/expr/cpp_delete.cpp
      src/cpp/jank/analyze/local_frame.cpp
      src/cpp/jank/analyze/step/force_boxed.cpp
      src/cpp/jank/analyze/cpp_util.cpp
      src/cpp/jank/analyze/pass/walk.cpp
      src/cpp/jank/analyze/pass/optimize.cpp
      src/cpp/jank/analyze/pass/strip_source_meta.cpp
      # Codegen files
      src/cpp/jank/codegen/processor.cpp
      src/cpp/jank/codegen/wasm_patch_processor.cpp
      # Evaluate
      src/cpp/jank/evaluate.cpp
      # Error handling for codegen
      src/cpp/jank/error/codegen.cpp
      # Clang format utility (needed by evaluate.cpp)
      src/cpp/jank/util/clang_format.cpp
    )
  else()
    message(STATUS "WASM build without CppInterOp - eval not available")
  endif()
else()
  # Native build: full runtime + AOT/codegen/nREPL
  list(APPEND jank_lib_sources ${jank_runtime_common_sources})
  list(APPEND jank_lib_sources
    src/cpp/jank/util/environment.cpp
    src/cpp/jank/util/sha256.cpp
    src/cpp/jank/util/cli.cpp
    src/cpp/jank/util/clang_format.cpp
    src/cpp/jank/util/clang.cpp
    src/cpp/jank/error/aot.cpp
    src/cpp/jank/error/codegen.cpp
    src/cpp/jank/codegen/processor.cpp
    src/cpp/jank/codegen/wasm_patch_processor.cpp
    src/cpp/jank/codegen/llvm_processor.cpp
    src/cpp/jank/nrepl_server/asio.cpp
    src/cpp/jank/nrepl_server/native_header_completion.cpp
    src/cpp/jank/nrepl_server/native_header_index.cpp
    src/cpp/jank/aot/processor.cpp
    src/cpp/jank/aot/resource.cpp
    src/cpp/jank/compiler_native.cpp
    src/cpp/jank/perf_native.cpp
    src/cpp/jank/ui/highlight.cpp
    src/cpp/jank/error/report.cpp
    src/cpp/jank/runtime/perf.cpp
    # Analyze files (require LLVM, not available in WASM)
    src/cpp/jank/analyze/processor.cpp
    src/cpp/jank/analyze/expression.cpp
    src/cpp/jank/analyze/expression_hash.cpp
    src/cpp/jank/analyze/expr/vector.cpp
    src/cpp/jank/analyze/expr/recursion_reference.cpp
    src/cpp/jank/analyze/expr/map.cpp
    src/cpp/jank/analyze/expr/do.cpp
    src/cpp/jank/analyze/expr/var_deref.cpp
    src/cpp/jank/analyze/expr/recur.cpp
    src/cpp/jank/analyze/expr/named_recursion.cpp
    src/cpp/jank/analyze/expr/local_reference.cpp
    src/cpp/jank/analyze/expr/def.cpp
    src/cpp/jank/analyze/expr/let.cpp
    src/cpp/jank/analyze/expr/letfn.cpp
    src/cpp/jank/analyze/expr/throw.cpp
    src/cpp/jank/analyze/expr/if.cpp
    src/cpp/jank/analyze/expr/list.cpp
    src/cpp/jank/analyze/expr/primitive_literal.cpp
    src/cpp/jank/analyze/expr/call.cpp
    src/cpp/jank/analyze/expr/set.cpp
    src/cpp/jank/analyze/expr/var_ref.cpp
    src/cpp/jank/analyze/expr/function.cpp
    src/cpp/jank/analyze/expr/try.cpp
    src/cpp/jank/analyze/expr/case.cpp
    src/cpp/jank/analyze/expr/cpp_raw.cpp
    src/cpp/jank/analyze/expr/cpp_type.cpp
    src/cpp/jank/analyze/expr/cpp_value.cpp
    src/cpp/jank/analyze/expr/cpp_cast.cpp
    src/cpp/jank/analyze/expr/cpp_call.cpp
    src/cpp/jank/analyze/expr/cpp_constructor_call.cpp
    src/cpp/jank/analyze/expr/cpp_member_call.cpp
    src/cpp/jank/analyze/expr/cpp_member_access.cpp
    src/cpp/jank/analyze/expr/cpp_builtin_operator_call.cpp
    src/cpp/jank/analyze/expr/cpp_box.cpp
    src/cpp/jank/analyze/expr/cpp_unbox.cpp
    src/cpp/jank/analyze/expr/cpp_new.cpp
    src/cpp/jank/analyze/expr/cpp_delete.cpp
    src/cpp/jank/analyze/local_frame.cpp
    src/cpp/jank/analyze/step/force_boxed.cpp
    src/cpp/jank/analyze/cpp_util.cpp
    src/cpp/jank/analyze/pass/walk.cpp
    src/cpp/jank/analyze/pass/optimize.cpp
    src/cpp/jank/analyze/pass/strip_source_meta.cpp
    # Common utility files
    src/cpp/jank/evaluate.cpp
    src/cpp/jank/c_api.cpp
    src/cpp/clojure/core_native.cpp
    src/cpp/clojure/string_native.cpp
    src/cpp/jank/arena_native.cpp
    src/cpp/jank/debug_allocator_native.cpp
  )
endif()

if(jank_target_wasm)
  # WASM always uses the stub JIT processor - no native execution possible
  # CppInterOp in WASM is used for type resolution and analysis, not JIT
  list(APPEND jank_lib_sources src/cpp/jank/jit/processor_stub_wasm.cpp)
  if(JANK_WASM_HAS_CPPINTEROP)
    # Add interpreter accessor for WASM with CppInterOp
    list(APPEND jank_lib_sources src/cpp/jank/jit/interpreter.cpp)
  endif()
else()
  list(APPEND jank_lib_sources src/cpp/jank/jit/processor.cpp)
  list(APPEND jank_lib_sources src/cpp/jank/jit/interpreter.cpp)
endif()

add_library(
  jank_lib STATIC
  ${jank_lib_sources}
)
set_target_properties(jank_lib PROPERTIES UNITY_BUILD ${jank_unity_build})

set_property(TARGET jank_lib PROPERTY OUTPUT_NAME jank)

if(jank_cxx_standard)
  target_compile_features(jank_lib PUBLIC ${jank_cxx_standard})
endif()
target_compile_options(jank_lib PUBLIC ${jank_common_compiler_flags} ${jank_aot_compiler_flags})

if(jank_target_wasm)
  # Use wasm-specific implementations without cpptrace
  target_sources(jank_lib PRIVATE 
    src/cpp/jtl/panic_wasm.cpp
    src/cpp/jtl/assert_wasm.cpp
  )
  
  # Define JANK_TARGET_WASM for conditional compilation
  target_compile_definitions(jank_lib PUBLIC JANK_TARGET_WASM=1)

  # If CppInterOp is available, define JANK_HAS_CPPINTEROP to include real headers
  if(JANK_WASM_HAS_CPPINTEROP)
    target_compile_definitions(jank_lib PUBLIC JANK_HAS_CPPINTEROP=1)
  endif()
  
  # For WASM, only our project include is non-system
  target_include_directories(
    jank_lib
    PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/cpp>"
  )
  # Third-party includes should be SYSTEM to not interfere with emscripten's libc++
  target_include_directories(
    jank_lib
    SYSTEM
    PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third-party/immer>"
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third-party/bdwgc/include>"
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third-party/cli11/include>"
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third-party/zip/src>"
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third-party/bpptree/include>"
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third-party/boost-preprocessor/include>"
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third-party/stduuid/include/>"
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third-party/cppinterop/include>"
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third-party/cppinterop/lib>"
  )
  # Add LLVM/Clang includes when CppInterOp is available
  if(JANK_WASM_HAS_CPPINTEROP)
    target_include_directories(
      jank_lib
      SYSTEM
      PUBLIC
      ${WASM_LLVM_INCLUDE_DIRS}
    )
  endif()
else()
  # Use normal implementations with cpptrace for non-wasm
  target_sources(jank_lib PRIVATE 
    src/cpp/jtl/panic.cpp
    src/cpp/jtl/assert.cpp
  )
  
  target_include_directories(
    jank_lib
    PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/cpp>"
  )
  target_include_directories(
    jank_lib
    SYSTEM
    PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third-party/nanobench/include>"
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third-party/folly>"
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third-party/bpptree/include>"
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third-party/bdwgc/include>"
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third-party/immer>"
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third-party/cli11/include>"
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third-party/ftxui/include>"
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third-party/zip/src>"
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third-party/cpptrace/include>"
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third-party/cppinterop/include>"
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third-party/cppinterop/lib>"
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third-party/boost-preprocessor/include>"
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third-party/boost-multiprecision/include>"
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third-party/stduuid/include/>"
    ${jank_boost_include_dir}
    ${OPENSSL_INCLUDE_DIR}
  )
endif()

target_link_libraries(
  jank_lib PRIVATE
  bdwgc::gc
)
if(NOT jank_target_wasm)
  target_link_libraries(jank_lib PRIVATE Boost::multiprecision bdwgc::gctba)
endif()

if(NOT jank_target_wasm)
  target_link_libraries(
    jank_lib PRIVATE
    nanobench_lib
    clangCppInterOp
    cpptrace::cpptrace
    ftxui::screen ftxui::dom
  )
else()
  # WASM builds with CppInterOp support
  if(JANK_WASM_HAS_CPPINTEROP)
    target_link_libraries(
      jank_lib PRIVATE
      clangCppInterOp_wasm
    )
  endif()
endif()

if(APPLE)
  target_link_libraries(jank_lib PUBLIC libzstd_static)
endif()

jank_hook_llvm(jank_lib)

# Build a string of all include flags for jank. This will be used for building the PCH at build
# time as well as building the PCH on first run, after installation.
get_target_property(jank_lib_includes_prop jank_lib INCLUDE_DIRECTORIES)
set(jank_lib_includes "")

# For wasm builds, don't embed include paths in JANK_JIT_FLAGS
# This prevents our -I flags from overriding emscripten's system includes
if(NOT jank_target_wasm)
  foreach(dir ${jank_lib_includes_prop})
    # System includes are just the path, but normal includes have this format:
    # $<BUILD_INTERFACE:/home/jeaye/projects/jank/compiler+runtime/include/cpp>
    # We need to strip all of that out.
    if(${dir} MATCHES "BUILD_INTERFACE:")
      string(REGEX REPLACE "\\$<BUILD_INTERFACE:(.*)>" "\\1" dir_path ${dir})
      list(APPEND jank_lib_includes -I${dir_path})
    else()
      # We use -I instead of -isystem here, since the latter causes Clang's JIT to
      # fail to find important headers, like stdlib.h, even if we're only adding new
      # system includes.
      #
      # Fortunately, this doesn't really matter. The main reason for the system includes
      # is for static analysis and such to ignore those headers.
      list(APPEND jank_lib_includes -I${dir})
    endif()
  endforeach()
endif()

set(jank_lib_linker_flags_list "")
get_target_property(jank_lib_link_dirs_prop jank_lib LINK_DIRECTORIES)
foreach(lib ${jank_lib_link_dirs_prop})
  list(APPEND jank_lib_linker_flags_list -L${lib})
endforeach()
list(APPEND jank_lib_linker_flags_list ${jank_linker_flags})
list(APPEND jank_lib_linker_flags_list ${CMAKE_CXX_FLAGS})
list(JOIN jank_lib_linker_flags_list " " jank_lib_linker_flags_unescaped)
string(REPLACE "\"" "\\\"" jank_lib_linker_flags_str "${jank_lib_linker_flags_unescaped}")

# Build a string of all of our JIT compilation flags so we can load them up in the runtime.
set(jank_jit_compile_flags_list ${jank_common_compiler_flags} ${jank_jit_compiler_flags} ${jank_lib_includes})
list(JOIN jank_jit_compile_flags_list " " jank_jit_compile_flags_str_unescaped)
string(REPLACE "\"" "\\\"" jank_jit_compile_flags_str "${jank_jit_compile_flags_str_unescaped}")

target_compile_options(
  jank_lib
  PUBLIC
  -DJANK_VERSION="${jank_version}"
  -DJANK_CLANG_PREFIX="${CLANG_INSTALL_PREFIX}"
  -DJANK_JIT_FLAGS="${jank_jit_compile_flags_str}"
  -DJANK_AOT_FLAGS="${jank_lib_linker_flags_str}"
)
target_link_options(jank_lib PRIVATE ${jank_linker_flags})

#set_target_properties(jank_lib PROPERTIES LINK_FLAGS_RELEASE "-s")

# Symbol exporting for JIT.
set_target_properties(jank_lib PROPERTIES ENABLE_EXPORTS 1)
# ---- libjank.a ----

# ---- libfolly.a ----

if(NOT jank_target_wasm)
  # Folly is well and truly a pain in the ass to build through its
  # own build system. It regularly fails to build for me
  # on all sorts of standard systems (both Linux and macOS) and jank
  # has been running with custom patches for several months now. After
  # running into compilation issues with it yet again, I've decided to
  # just rip the good bits out and only compile what we need.
  #
  # The Folly fork we have makes things even easier to build by commenting
  # out some bits related to jemalloc and tcmalloc. Folly has a whole
  # complex preprocessor system for this stuff, but it still ends up
  # linking to them even when you say you don't want them.

  set(
    folly_sources
    third-party/folly/folly/ScopeGuard.cpp
    third-party/folly/folly/SharedMutex.cpp
    third-party/folly/folly/concurrency/CacheLocality.cpp
    third-party/folly/folly/synchronization/ParkingLot.cpp
    third-party/folly/folly/synchronization/SanitizeThread.cpp
    third-party/folly/folly/lang/SafeAssert.cpp
    third-party/folly/folly/lang/ToAscii.cpp
    third-party/folly/folly/system/ThreadId.cpp
    third-party/folly/folly/system/AtFork.cpp
    third-party/folly/folly/detail/Futex.cpp
    third-party/folly/folly/memory/folly_stubs.cpp
  )

  add_library(folly_lib STATIC ${folly_sources})

  # Not our code. Don't try to lint it.
  set_source_files_properties(${folly_sources} PROPERTIES SKIP_LINTING ON)

  target_include_directories(
    folly_lib
    SYSTEM
    PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third-party/folly>"
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third-party/boost-preprocessor/include>"
  )

  set_property(TARGET folly_lib PROPERTY OUTPUT_NAME folly)

  if(jank_cxx_standard)
    target_compile_features(folly_lib PUBLIC ${jank_cxx_standard})
  endif()
  target_compile_options(folly_lib PUBLIC ${jank_common_compiler_flags} ${jank_aot_compiler_flags} -w)
  target_link_options(folly_lib PRIVATE ${jank_linker_flags})

  #set_target_properties(folly_lib PROPERTIES LINK_FLAGS_RELEASE "-s")

  # Symbol exporting for JIT.
  set_target_properties(folly_lib PROPERTIES ENABLE_EXPORTS 1)

  target_link_libraries(
    folly_lib PUBLIC
  )
endif()
# ---- libfolly.a ----

if(NOT jank_target_wasm)
  # ---- jank phase 1 executable ----
  add_executable(
  jank_exe_phase_1
  src/cpp/main.cpp
  src/cpp/jank/environment/check_health.cpp
)
add_executable(jank::exe ALIAS jank_exe_phase_1)

set_property(TARGET jank_exe_phase_1 PROPERTY OUTPUT_NAME jank-phase-1)

# Symbol exporting for JIT.
set_target_properties(jank_exe_phase_1 PROPERTIES ENABLE_EXPORTS 1)

target_compile_features(jank_exe_phase_1 PRIVATE ${jank_cxx_standard})
target_compile_options(jank_exe_phase_1
  PUBLIC
  ${jank_common_compiler_flags} ${jank_aot_compiler_flags}
  -DJANK_VERSION="${jank_version}"
)
target_include_directories(jank_exe_phase_1 SYSTEM PRIVATE "$<TARGET_PROPERTY:jank_lib,INCLUDE_DIRECTORIES>")
target_link_directories(jank_exe_phase_1 PRIVATE "$<TARGET_PROPERTY:jank_lib,LINK_DIRECTORIES>")
target_link_options(jank_exe_phase_1 PRIVATE ${jank_linker_flags} -L ${CMAKE_BINARY_DIR})

target_link_libraries(
  jank_exe_phase_1 PUBLIC
  ${jank_link_whole_start} ${CMAKE_BINARY_DIR}/libjank-standalone-phase-1.a ${jank_link_whole_end}
  z
  LLVM clang-cpp
  OpenSSL::Crypto
)

jank_hook_llvm(jank_exe_phase_1)

add_dependencies(jank_exe_phase_1 jank_lib_standalone_phase_1)

#set_target_properties(jank_exe_phase_1 PROPERTIES LINK_FLAGS_RELEASE "-s")
# ---- jank phase 1 executable ----

# ---- jank phase 2 executable ----
if(jank_enable_phase_2)
  add_executable(
    jank_exe_phase_2
    "$<TARGET_PROPERTY:jank_exe_phase_1,SOURCES>"
  )
  add_executable(jank::exe ALIAS jank_exe_phase_2)

  set_property(TARGET jank_exe_phase_2 PROPERTY OUTPUT_NAME jank)

  # Symbol exporting for JIT.
  set_target_properties(jank_exe_phase_2 PROPERTIES ENABLE_EXPORTS 1)

  target_compile_features(jank_exe_phase_2 PRIVATE ${jank_cxx_standard})
  target_compile_options(jank_exe_phase_2 PUBLIC "$<TARGET_PROPERTY:jank_exe_phase_1,COMPILE_OPTIONS>" -DJANK_PHASE_2)
  target_include_directories(jank_exe_phase_2 SYSTEM PRIVATE "$<TARGET_PROPERTY:jank_exe_phase_1,INCLUDE_DIRECTORIES>")
  target_link_directories(jank_exe_phase_2 PRIVATE "$<TARGET_PROPERTY:jank_exe_phase_1,LINK_DIRECTORIES>")
  target_link_options(jank_exe_phase_2 PRIVATE "$<TARGET_PROPERTY:jank_exe_phase_1,LINK_OPTIONS>")

  target_link_libraries(
    jank_exe_phase_2 PUBLIC
    "$<TARGET_PROPERTY:jank_exe_phase_1,LINK_LIBRARIES>"
    ${jank_core_library_objects}
  )

  jank_hook_llvm(jank_exe_phase_2)

  add_dependencies(jank_exe_phase_2 jank_lib_standalone_phase_2 jank_core_libraries)

  #set_target_properties(jank_exe_phase_2 PROPERTIES LINK_FLAGS_RELEASE "-s")
else()
  add_custom_command(
    DEPENDS ${CMAKE_BINARY_DIR}/jank-phase-1 ${jank_core_library_objects} ${jank_incremental_pch_flag}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    OUTPUT ${CMAKE_BINARY_DIR}/jank
    COMMAND ln -sf jank-phase-1 jank
  )
  add_custom_target(
    jank_exe_phase_2
    ALL
    DEPENDS ${CMAKE_BINARY_DIR}/jank
  )
endif()
# ---- jank phase 2 executable ----

# ---- Tests ----
if(jank_test)
  add_executable(
    jank_test_exe
    test/cpp/main.cpp
    test/cpp/jtl/immutable_string.cpp
    test/cpp/jtl/string_builder.cpp
    test/cpp/jank/util/fmt.cpp
    test/cpp/jank/util/path.cpp
    test/cpp/jank/read/lex.cpp
    test/cpp/jank/read/parse.cpp
    test/cpp/jank/analyze/box.cpp
    test/cpp/jank/runtime/behavior/callable.cpp
    test/cpp/jank/runtime/core/seq.cpp
    test/cpp/jank/runtime/core/integer_cache.cpp
    test/cpp/jank/runtime/core/integer_cache_benchmark.cpp
    test/cpp/jank/runtime/core/real_cache.cpp
    test/cpp/jank/runtime/core/arena.cpp
    test/cpp/jank/runtime/core/custom_allocator.cpp
    test/cpp/jank/runtime/core/optimization.cpp
    test/cpp/jank/runtime/detail/native_persistent_list.cpp
    test/cpp/jank/runtime/obj/big_integer.cpp
    test/cpp/jank/runtime/obj/big_decimal.cpp
    test/cpp/jank/runtime/obj/persistent_string.cpp
    test/cpp/jank/runtime/obj/ratio.cpp
    test/cpp/jank/runtime/obj/persistent_list.cpp
    test/cpp/jank/runtime/obj/persistent_string.cpp
    test/cpp/jank/runtime/obj/persistent_vector.cpp
    test/cpp/jank/runtime/obj/range.cpp
    test/cpp/jank/runtime/obj/integer_range.cpp
    test/cpp/jank/runtime/obj/repeat.cpp
    test/cpp/jank/nrepl/engine.cpp
    test/cpp/jank/nrepl/eval.cpp
    test/cpp/jank/nrepl/complete.cpp
    test/cpp/jank/nrepl/completions.cpp
    test/cpp/jank/nrepl/info.cpp
    test/cpp/jank/nrepl/eldoc.cpp
    test/cpp/jank/nrepl/test.cpp
    test/cpp/jank/nrepl/native.cpp
    test/cpp/jank/nrepl/native_header_index.cpp
    test/cpp/jank/jit/processor.cpp
    test/cpp/jank/perf/eval_benchmark.cpp
  )
  add_executable(jank::test_exe ALIAS jank_test_exe)
  add_dependencies(jank_test_exe jank_exe_phase_1 jank_core_libraries)

  set_property(TARGET jank_test_exe PROPERTY OUTPUT_NAME jank-test)

  target_compile_features(jank_test_exe PRIVATE ${jank_cxx_standard})
  # A doctest issue causes some warnings: https://github.com/doctest/doctest/issues/900
  target_compile_options(jank_test_exe PUBLIC ${jank_common_compiler_flags} ${jank_aot_compiler_flags} "-Wno-#warnings")
  target_compile_options(jank_test_exe PRIVATE -DDOCTEST_CONFIG_SUPER_FAST_ASSERTS)
  target_include_directories(jank_test_exe SYSTEM PRIVATE "$<TARGET_PROPERTY:jank_lib,INCLUDE_DIRECTORIES>")
  target_link_directories(jank_test_exe PRIVATE "$<TARGET_PROPERTY:jank_lib,LINK_DIRECTORIES>")
  target_link_options(jank_test_exe PRIVATE ${jank_linker_flags} -L ${CMAKE_BINARY_DIR})

  find_package(doctest REQUIRED)
  target_link_libraries(
    jank_test_exe PUBLIC
    ${jank_link_whole_start} ${CMAKE_BINARY_DIR}/libjank-standalone-phase-1.a ${jank_link_whole_end}
    z
    LLVM clang-cpp
    OpenSSL::Crypto
    doctest::doctest
  )

  jank_hook_llvm(jank_test_exe)

  # Symbol exporting for JIT.
  set_target_properties(jank_test_exe PROPERTIES ENABLE_EXPORTS 1)

  add_dependencies(jank_test_exe jank_exe_phase_2)

  add_test(NAME "Test" COMMAND jank_test_exe)
endif()
# ---- Tests ----
else()
  message(STATUS "Skipping native executables/tests for wasm build")
endif()

# ---- Incremental PCH ----
# Once we boot up jank, the first thing we do is load a PCH so that the JIT environment
# can know all of the types and functions within the jank runtime. This PCH is our
# "incremental" PCH, named that way since it uses Clang's -fincremental-extensions flag.

if(NOT jank_target_wasm)
  # Build a string of all of our AOT compilation flags so we can share them with our incremental PCH.
  set(jank_incremental_pch_flag ${CMAKE_BINARY_DIR}/incremental.pch)

  separate_arguments(jank_incremental_pch_cxx_flags UNIX_COMMAND ${CMAKE_CXX_FLAGS})
  list(REMOVE_ITEM jank_incremental_pch_cxx_flags -fPIC)
  add_custom_command(
    DEPENDS ${CMAKE_BINARY_DIR}/jank-phase-1
    OUTPUT ${jank_incremental_pch_flag}
    COMMAND_EXPAND_LISTS
    VERBATIM
    COMMAND ${CMAKE_CXX_COMPILER}
            #${jank_incremental_pch_cxx_flags}
            ${jank_common_compiler_flags} ${jank_jit_compiler_flags} ${jank_lib_includes}
            -Xclang -fincremental-extensions
            -Xclang -emit-pch
            -Xclang -fmodules-embed-all-files
            -fno-modules-validate-system-headers
            -fpch-instantiate-templates
            -x c++-header
            -w
            -c ${PROJECT_SOURCE_DIR}/include/cpp/jank/prelude.hpp
            -o ${jank_incremental_pch_flag}
  )
  add_custom_target(
    jank_incremental_pch
    ALL
    DEPENDS ${jank_incremental_pch_flag}
  )
endif()
# ---- Incremental PCH ----

# ---- Compiled Clojure libraries ----
# We do a bit of a dance here, to have a custom command generate a file
# which is a then a dependency of a custom target. This is because custom
# targets *always* run, but we only want to compile our libs when they change,
# or when we haven't yet done so.
#
# With this setup, we'll compile when the flag file doesn't exist (i.e. on
# first build or after a clean), when any of the jank sources for these libs
# change, or whenever the jank binary changes.
if(NOT jank_target_wasm)
  set(jank_core_libraries_flag "${CMAKE_BINARY_DIR}/classes/core-libraries")
  set(jank_core_library_commands "")
  list(LENGTH jank_core_modules jank_core_module_count)
  if(jank_core_module_count GREATER 0)
    math(EXPR jank_core_module_last_index "${jank_core_module_count} - 1")
    foreach(module_index RANGE 0 ${jank_core_module_last_index})
      list(GET jank_core_modules ${module_index} jank_core_module_name)
      list(GET jank_core_module_paths ${module_index} jank_core_module_path)
      list(GET jank_core_library_objects ${module_index} jank_core_module_object)
      get_filename_component(jank_core_module_dir ${jank_core_module_object} DIRECTORY)
      list(APPEND jank_core_library_commands
        COMMAND ${CMAKE_COMMAND} -E make_directory ${jank_core_module_dir}
        COMMAND ${CMAKE_BINARY_DIR}/jank-phase-1 compile-module -o ${jank_core_module_object} ${jank_core_module_name}
      )
    endforeach()
  endif()
  add_custom_command(
    DEPENDS ${CMAKE_BINARY_DIR}/jank-phase-1 ${jank_core_module_sources} ${jank_incremental_pch_flag}
    OUTPUT ${jank_core_libraries_flag}
    BYPRODUCTS ${jank_core_library_objects}
    ${jank_core_library_commands}
    COMMAND touch ${jank_core_libraries_flag}
  )
  add_custom_target(
    jank_core_libraries
    ALL
    DEPENDS ${jank_core_libraries_flag}
  )
endif()
# ---- Compiled Clojure libraries ----

# ---- Clang format YAML ----
# We copy this into our binary directory so that jank can use it to format code. It's also
# installed alongside jank, for the same reason.
file(COPY ${CMAKE_SOURCE_DIR}/../.clang-format DESTINATION ${CMAKE_BINARY_DIR})
# ---- Clang format YAML ----

# ---- Install rules ----
if(NOT CMAKE_SKIP_INSTALL_RULES AND jank_enable_phase_2)
  include(cmake/install.cmake)
endif()
# ---- Install rules ----

# Finally, output a helpful summary of the most important bits to the user.
include(cmake/summary.cmake)
