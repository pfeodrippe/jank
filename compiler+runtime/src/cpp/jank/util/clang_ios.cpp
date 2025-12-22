// iOS-specific stubs for clang utilities
// On iOS, we embed LLVM/Clang as libraries - no external executables needed

#include <filesystem>

#include <jank/util/clang.hpp>
#include <jank/util/environment.hpp>
#include <jank/error/system.hpp>

namespace jank::util
{
  jtl::option<jtl::immutable_string> find_clang()
  {
    // On iOS, clang is embedded as libraries - return a path relative to resource_dir
    // The processor uses parent_path()/"../include" to find includes
    // So we return {resource_dir}/bin/clang++ which results in {resource_dir}/include
    static jtl::immutable_string result;
    if(result.empty())
    {
      result = jtl::immutable_string{ resource_dir() } + "/bin/clang++";
    }
    return result;
  }

  jtl::option<jtl::immutable_string> find_clang_resource_dir()
  {
    // Resource dir should be bundled in the app at runtime
    // The iOS app should set this up appropriately via resource_dir()
    static jtl::immutable_string result;
    if(result.empty())
    {
      result = jtl::immutable_string{ resource_dir() } + "/clang";
    }
    return result;
  }

  jtl::result<void, error_ref> invoke_clang(std::vector<char const *>)
  {
    // Cannot invoke external clang on iOS
    return error::system_failure("External clang invocation not supported on iOS");
  }

  jtl::option<jtl::immutable_string> find_pch(jtl::immutable_string const &)
  {
    // PCH should be pre-built on macOS and bundled in the iOS app
    // Look for it in the app's resource directory
    static jtl::immutable_string result;
    if(result.empty())
    {
      auto pch_path = jtl::immutable_string{ resource_dir() } + "/incremental.pch";
      if(std::filesystem::exists(pch_path.c_str()))
      {
        result = pch_path;
      }
    }
    if(!result.empty())
    {
      return result;
    }
    return jtl::none;
  }

  jtl::result<jtl::immutable_string, error_ref>
  build_pch(std::vector<char const *>, jtl::immutable_string const &)
  {
    // Cannot build PCH on iOS - it should be pre-built and bundled
    return error::system_failure("PCH building not supported on iOS - PCH must be pre-bundled");
  }

  jtl::immutable_string default_target_triple()
  {
    // iOS simulator arm64 target
    return "arm64-apple-ios17.0-simulator";
  }
}
