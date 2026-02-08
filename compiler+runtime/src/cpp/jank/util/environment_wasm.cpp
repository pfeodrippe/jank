// WASM stub for environment utilities
// Most of these don't make sense in a WASM context

#include <jank/util/environment.hpp>

namespace jank::util
{
  jtl::immutable_string const &user_home_dir()
  {
    static jtl::immutable_string res{ "/" };
    return res;
  }

  jtl::immutable_string const &user_cache_dir(jtl::immutable_string const &)
  {
    static jtl::immutable_string res{ "/cache" };
    return res;
  }

  jtl::immutable_string const &user_config_dir()
  {
    static jtl::immutable_string res{ "/config" };
    return res;
  }

  jtl::immutable_string const &binary_cache_dir(jtl::immutable_string const &)
  {
    static jtl::immutable_string res{ "/cache" };
    return res;
  }

  jtl::immutable_string const &binary_version()
  {
    static jtl::immutable_string res{ "jank-wasm-0.1" };
    return res;
  }

  jtl::immutable_string process_path()
  {
    return "/wasm/jank";
  }

  jtl::immutable_string process_dir()
  {
    return "/wasm";
  }

  jtl::immutable_string resource_dir()
  {
    return "/wasm/lib";
  }

  void add_system_flags(std::vector<char const *> &)
  {
    // No system flags for WASM
  }
}
