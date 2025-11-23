#include <algorithm>
#include <ranges>

#include <jank/nrepl_server/native_header_index.hpp>
#include <jank/nrepl_server/native_header_completion.hpp>

namespace jank::nrepl_server::asio
{
  using native_alias = jank::runtime::ns::native_alias;

  std::string native_header_index::make_cache_key(native_alias const &alias)
  {
    std::string key;
    key.reserve(alias.scope.size() + alias.header.size() + 1);
    key.append(alias.scope.begin(), alias.scope.end());
    key.push_back('|');
    key.append(alias.header.begin(), alias.header.end());
    return key;
  }

  std::vector<std::string> const &native_header_index::ensure_cache(native_alias const &alias) const
  {
    auto const key(make_cache_key(alias));
    std::scoped_lock<std::mutex> const lock{ mutex_ };
    auto const it(cache_.find(key));
    if(it != cache_.end())
    {
      return it->second;
    }

    auto entries(enumerate_native_header_functions(alias, ""));
    std::ranges::sort(entries);
    auto const unique_end(std::ranges::unique(entries));
    entries.erase(unique_end.begin(), unique_end.end());

    auto const [inserted_it, _] = cache_.emplace(key, std::move(entries));
    return inserted_it->second;
  }

  std::vector<std::string>
  native_header_index::list_functions(native_alias const &alias, std::string const &prefix) const
  {
    auto const &entries(ensure_cache(alias));
    if(prefix.empty())
    {
      return entries;
    }

    std::vector<std::string> matches;
    matches.reserve(entries.size());
    for(auto const &entry : entries)
    {
      if(entry.starts_with(prefix))
      {
        matches.push_back(entry);
      }
    }
    return matches;
  }

  bool native_header_index::contains(native_alias const &alias, std::string const &name) const
  {
    auto const &entries(ensure_cache(alias));
    return std::ranges::binary_search(entries, name);
  }
}
