# iOS Simulator Toolchain for CMake
# Target: arm64-apple-ios17.0-simulator
# Note: iOS 17.0+ required for std::format with floating point support

set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_SYSTEM_VERSION 17.0)
set(CMAKE_OSX_DEPLOYMENT_TARGET "17.0" CACHE STRING "Minimum iOS version")

# Target simulator (not device)
set(CMAKE_OSX_SYSROOT iphonesimulator CACHE STRING "iOS SDK")
set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "Build for ARM64")

# Force iOS simulator platform
set(CMAKE_XCODE_ATTRIBUTE_SUPPORTED_PLATFORMS "iphonesimulator")
set(CMAKE_XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH "YES")

# C/C++ compiler settings
set(CMAKE_C_COMPILER_TARGET arm64-apple-ios17.0-simulator)
set(CMAKE_CXX_COMPILER_TARGET arm64-apple-ios17.0-simulator)

# Use Apple's clang
find_program(CMAKE_C_COMPILER clang)
find_program(CMAKE_CXX_COMPILER clang++)

# Standard settings
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static libraries only
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static libraries" FORCE)

# Skip try_compile for cross-compiling
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# iOS-specific flags (similar to WASM - no JIT)
set(CMAKE_C_FLAGS_INIT "-fembed-bitcode")
set(CMAKE_CXX_FLAGS_INIT "-fembed-bitcode")
