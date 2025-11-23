#include <ranges>

#include <doctest/doctest.h>

#include <jank/nrepl_server/native_header_index.hpp>

namespace jank::nrepl_server::asio
{
  namespace
  {
    runtime::ns::native_alias make_string_native_alias()
    {
      return runtime::ns::native_alias{ jtl::immutable_string{ "clojure/string_native.hpp" },
                                        jtl::immutable_string{ "<clojure/string_native.hpp>" },
                                        jtl::immutable_string{ "clojure.string_native" } };
    }
  }

  TEST_CASE("native header index caches enum results")
  {
    native_header_index index;
    auto alias(make_string_native_alias());

    auto const reverse_matches(index.list_functions(alias, "rev"));
    REQUIRE(std::ranges::find(reverse_matches, "reverse") != reverse_matches.end());

    auto const everything(index.list_functions(alias, ""));
    REQUIRE_FALSE(everything.empty());
    CHECK(index.contains(alias, "reverse"));
    CHECK(index.contains(alias, "lower_case"));
  }
}
