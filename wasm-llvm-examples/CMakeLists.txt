cmake_minimum_required(VERSION 3.20)
project(wasm_clang_repl_test)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# LLVM build directory
set(LLVM_BUILD_DIR "/Users/pfeodrippe/dev/jank/wasm-llvm-build/llvm-build")
set(LLVM_DIR "${LLVM_BUILD_DIR}/lib/cmake/llvm")
set(Clang_DIR "${LLVM_BUILD_DIR}/lib/cmake/clang")
set(LLD_DIR "${LLVM_BUILD_DIR}/lib/cmake/lld")

# Find LLVM
find_package(LLVM REQUIRED CONFIG)
find_package(LLD REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

include_directories(${LLVM_INCLUDE_DIRS})
include_directories(${CLANG_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# Test executable
add_executable(test_repl test_repl.cpp)

# Enable RTTI (required for LLVM)
target_compile_options(test_repl PRIVATE -frtti -fexceptions)

# Link LLVM components (comprehensive list)
llvm_map_components_to_libnames(llvm_libs
  all
)

# Link Clang libraries (order matters!)
target_link_libraries(test_repl
  clangInterpreter
  clangFrontend
  clangDriver
  clangSerialization
  clangCodeGen
  clangParse
  clangSema
  clangAnalysis
  clangEdit
  clangAST
  clangLex
  clangBasic
  lldWasm
  lldCommon
  ${llvm_libs}
)

# Emscripten-specific flags
if(EMSCRIPTEN)
  set_target_properties(test_repl PROPERTIES
    LINK_FLAGS "-s ALLOW_MEMORY_GROWTH=1 -s INITIAL_MEMORY=512MB -s MAXIMUM_MEMORY=2GB -s STACK_SIZE=10MB -s ASSERTIONS=1 -s EXIT_RUNTIME=1 -s NODERAWFS=1"
  )
endif()
