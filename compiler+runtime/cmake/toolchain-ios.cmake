# iOS Toolchain for jank
# Cross-compiles jank runtime for iOS (arm64-apple-ios)

set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_OSX_ARCHITECTURES arm64)
set(CMAKE_OSX_DEPLOYMENT_TARGET 17.0)
set(CMAKE_OSX_SYSROOT iphoneos)

# Use system clang for iOS cross-compilation
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)

# Force C++20 for iOS
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Disable features not available on iOS
set(CMAKE_MACOSX_BUNDLE NO)

# Set this so CMakeLists.txt can detect iOS build
set(jank_target_ios ON CACHE BOOL "Building for iOS" FORCE)
