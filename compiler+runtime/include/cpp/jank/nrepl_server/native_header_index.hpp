#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <jtl/immutable_string.hpp>

#include <jank/runtime/ns.hpp>

namespace jank::nrepl_server::asio
{
  class native_header_index
  {
  public:
    std::vector<std::string>
    list_functions(jank::runtime::ns::native_alias const &alias, std::string const &prefix) const;

    bool contains(jank::runtime::ns::native_alias const &alias, std::string const &name) const;

  private:
    using cache_map = std::unordered_map<std::string, std::vector<std::string>>;

    std::vector<std::string> const &
    ensure_cache(jank::runtime::ns::native_alias const &alias) const;
    static std::string make_cache_key(jank::runtime::ns::native_alias const &alias);

    mutable std::mutex mutex_;
    mutable cache_map cache_;
  };
}
