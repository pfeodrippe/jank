#include <ranges>

#include <doctest/doctest.h>

#include <jank/nrepl_server/engine.hpp>
#include <jank/nrepl_server/native_header_index.hpp>

namespace jank::nrepl_server::asio
{
  namespace
  {
    message make_message(std::initializer_list<std::pair<std::string, std::string>> fields)
    {
      bencode::value::dict dict;
      for(auto const &entry : fields)
      {
        dict.emplace(entry.first, bencode::value{ entry.second });
      }
      return message{ std::move(dict) };
    }

    runtime::ns::native_alias make_string_native_alias()
    {
      return runtime::ns::native_alias{ jtl::immutable_string{ "clojure/string_native.hpp" },
                                        jtl::immutable_string{ "<clojure/string_native.hpp>" },
                                        jtl::immutable_string{ "clojure.string_native" } };
    }
  }

  TEST_CASE("native header index caches enum results")
  {
    /* Initialize the engine to set up the clang interpreter and runtime context.
     * We need to require the header first so that the namespace is loaded. */
    engine eng;

    /* Require the header to load the namespace into clang */
    eng.handle(make_message({
      {   "op", "eval" },
      { "code", "(require '[\"clojure/string_native.hpp\" :as str-native :scope \"clojure.string_native\"])" }
    }));

    native_header_index const index;
    auto alias(make_string_native_alias());

    auto const reverse_matches(index.list_functions(alias, "rev"));
    REQUIRE(std::ranges::find(reverse_matches, "reverse") != reverse_matches.end());

    auto const everything(index.list_functions(alias, ""));
    REQUIRE_FALSE(everything.empty());
    CHECK(index.contains(alias, "reverse"));
    CHECK(index.contains(alias, "lower_case"));
  }
}
