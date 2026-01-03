// iOS-specific environment utilities
// These return paths within the iOS app bundle

#include <jank/util/environment.hpp>
#include <iostream>

#if defined(__OBJC__) || defined(JANK_TARGET_IOS)
  #import <Foundation/Foundation.h>
  #import <TargetConditionals.h>
#endif

namespace jank::util
{
  namespace
  {
    // Get the app's bundle resource path
    jtl::immutable_string get_bundle_resource_path()
    {
      static jtl::immutable_string cached_path;
      if(cached_path.empty())
      {
#if defined(__OBJC__) || defined(JANK_TARGET_IOS)
        @autoreleasepool
        {
          NSBundle *bundle = [NSBundle mainBundle];
          NSString *resourcePath = [bundle resourcePath];
          if(resourcePath)
          {
            cached_path = jtl::immutable_string{ [resourcePath UTF8String] };
          }
          else
          {
            cached_path = jtl::immutable_string{ "/tmp" };
          }
        }
#else
        cached_path = jtl::immutable_string{ "/tmp" };
#endif
      }
      return cached_path;
    }

    // Get the app's documents directory for writable storage
    jtl::immutable_string get_documents_path()
    {
      static jtl::immutable_string cached_path;
      if(cached_path.empty())
      {
#if defined(__OBJC__) || defined(JANK_TARGET_IOS)
        @autoreleasepool
        {
          NSArray *paths
            = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
          if(paths.count > 0)
          {
            cached_path = jtl::immutable_string{ [paths[0] UTF8String] };
          }
          else
          {
            cached_path = jtl::immutable_string{ "/tmp" };
          }
        }
#else
        cached_path = jtl::immutable_string{ "/tmp" };
#endif
      }
      return cached_path;
    }

    // Get the app's caches directory
    jtl::immutable_string get_caches_path()
    {
      static jtl::immutable_string cached_path;
      if(cached_path.empty())
      {
#if defined(__OBJC__) || defined(JANK_TARGET_IOS)
        @autoreleasepool
        {
          NSArray *paths
            = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES);
          if(paths.count > 0)
          {
            cached_path = jtl::immutable_string{ [paths[0] UTF8String] };
          }
          else
          {
            cached_path = jtl::immutable_string{ "/tmp" };
          }
        }
#else
        cached_path = jtl::immutable_string{ "/tmp" };
#endif
      }
      return cached_path;
    }
  }

  jtl::immutable_string const &user_home_dir()
  {
    static jtl::immutable_string res{ get_documents_path() };
    return res;
  }

  jtl::immutable_string const &user_cache_dir(jtl::immutable_string const &)
  {
    static jtl::immutable_string res{ get_caches_path() + "/jank" };
    return res;
  }

  jtl::immutable_string const &user_config_dir()
  {
    static jtl::immutable_string res{ get_documents_path() + "/jank-config" };
    return res;
  }

  jtl::immutable_string const &binary_cache_dir(jtl::immutable_string const &)
  {
    static jtl::immutable_string res{ get_caches_path() + "/jank-cache" };
    return res;
  }

  jtl::immutable_string const &binary_version()
  {
    static jtl::immutable_string res{ "jank-ios-0.1" };
    return res;
  }

  jtl::immutable_string process_path()
  {
    return get_bundle_resource_path() + "/jank";
  }

  jtl::immutable_string process_dir()
  {
    return get_bundle_resource_path();
  }

  // This is the key function - returns path to jank resources
  // The module loader will look for src/jank under this path
  jtl::immutable_string resource_dir()
  {
    return get_bundle_resource_path();
  }

  void add_system_flags(std::vector<char const *> &args)
  {
    // Debug: print resource dir
    std::cerr << "[ios-jit] add_system_flags called, resource_dir = " << resource_dir().c_str()
              << std::endl;

    // Add iOS-specific flags for JIT compilation
    args.emplace_back("-target");
#if TARGET_OS_SIMULATOR
    args.emplace_back("arm64-apple-ios17.0-simulator");
#else
    args.emplace_back("arm64-apple-ios17.0");
#endif

    // For iOS JIT, we need bundled headers since the app can't access the macOS filesystem.
    // Headers should be bundled at:
    //   {resource_dir}/include/c++/v1       - libc++ headers
    //   {resource_dir}/include/sys_include  - C system headers (from SDK usr/include)
    //   {resource_dir}/clang                - clang resource headers (set via -resource-dir)
    //   {resource_dir}/include              - app-specific headers (vulkan, SDL, etc.)

    // libc++ headers (C++ standard library)
    static std::string libc_include_path;
    if(libc_include_path.empty())
    {
      libc_include_path = std::string(resource_dir().c_str()) + "/include/c++/v1";
    }
    std::cerr << "[ios-jit] libc++ include: " << libc_include_path << std::endl;
    args.emplace_back("-isystem");
    args.emplace_back(libc_include_path.c_str());

    // Clang builtin headers (stddef.h, stdarg.h, etc.)
    // These MUST come before SDK sys_include headers because they define
    // size_t, ptrdiff_t via __SIZE_TYPE__, __PTRDIFF_TYPE__ compiler builtins.
    // When libc++ does #include_next <stddef.h>, it should find clang's first.
    static std::string clang_include_path;
    if(clang_include_path.empty())
    {
      clang_include_path = std::string(resource_dir().c_str()) + "/clang/include";
    }
    std::cerr << "[ios-jit] clang builtin include: " << clang_include_path << std::endl;
    args.emplace_back("-isystem");
    args.emplace_back(clang_include_path.c_str());

    // C system headers (from iOS SDK usr/include)
    static std::string sys_include_path;
    if(sys_include_path.empty())
    {
      sys_include_path = std::string(resource_dir().c_str()) + "/include/sys_include";
    }
    std::cerr << "[ios-jit] sys include: " << sys_include_path << std::endl;
    args.emplace_back("-isystem");
    args.emplace_back(sys_include_path.c_str());

    // App-specific headers (vulkan, SDL, imgui, etc.)
    static std::string app_include_path;
    if(app_include_path.empty())
    {
      app_include_path = std::string(resource_dir().c_str()) + "/include";
    }
    std::cerr << "[ios-jit] app include: " << app_include_path << std::endl;
    args.emplace_back("-I");
    args.emplace_back(app_include_path.c_str());

    // C++ standard library
    args.emplace_back("-stdlib=libc++");

    // Define FLT_MIN/FLT_MAX that imgui.h needs (it doesn't include <cfloat>)
    // These are the standard IEEE 754 float values
    args.emplace_back("-DFLT_MIN=1.17549435e-38F");
    args.emplace_back("-DFLT_MAX=3.40282347e+38F");
    args.emplace_back("-DFLT_EPSILON=1.19209290e-07F");

    // Suppress some warnings that clang emits without a proper sysroot
    args.emplace_back("-Wno-stdlibcxx-not-found");
  }
}
