#include "common.hpp"

namespace jank::nrepl_server::asio
{
  TEST_SUITE("nREPL completions op")
  {
    TEST_CASE("completions respect prefix")
    {
      engine eng;
      eng.handle(make_message({
        {   "op",                   "eval" },
        { "code", "(defn sample-fn [] 42)" }
      }));
      auto responses(eng.handle(make_message({
        {     "op", "completions" },
        { "prefix",      "sample" }
      })));
      REQUIRE(responses.size() == 1);
      auto const &payload(responses.front());
      auto const &completions(payload.at("completions").as_list());
      REQUIRE_FALSE(completions.empty());
      bool found{ false };
      for(auto const &entry : completions)
      {
        auto const &dict(entry.as_dict());
        auto const &candidate(dict.at("candidate").as_string());
        if(candidate == "sample-fn")
        {
          found = true;
          break;
        }
      }
      CHECK(found);
    }

    TEST_CASE("namespace-qualified completions exclude referred vars")
    {
      engine eng;
      eng.handle(make_message({
        {   "op","eval"      },
        { "code",
         "(ns sample.server (:refer-clojure :refer :all)) (defn start [] 0) (defn stop [] "
         "1)" }
      }));
      eng.handle(make_message({
        {   "op",                                            "eval" },
        { "code", "(ns user (:require [sample.server :as server]))" }
      }));

      auto responses(eng.handle(make_message({
        {     "op", "completions" },
        { "prefix",    "server/s" },
        {     "ns",        "user" }
      })));
      REQUIRE(responses.size() == 1);
      auto const &payload(responses.front());
      auto const &completions(payload.at("completions").as_list());
      REQUIRE_FALSE(completions.empty());

      bool found_start{ false };
      bool found_stop{ false };
      for(auto const &entry_value : completions)
      {
        auto const &entry(entry_value.as_dict());
        auto const candidate(entry.at("candidate").as_string());
        CHECK(candidate.rfind("server/", 0) == 0);
        CHECK(candidate != "server/string?");
        if(candidate == "server/start")
        {
          found_start = true;
        }
        if(candidate == "server/stop")
        {
          found_stop = true;
        }
      }

      CHECK(found_start);
      CHECK(found_stop);
    }

TEST_CASE("completions returns namespaces in require context")
{
  engine eng;

  /* First, create a namespace to ensure we have something to complete */
  eng.handle(make_message({
    {   "op",                                    "eval" },
    { "code", "(ns sample.server) (defn run [] 42)" }
  }));
  eng.handle(make_message({
    {   "op",             "eval" },
    { "code", "(ns sample.client)" }
  }));
  eng.handle(make_message({
    {   "op",        "eval" },
    { "code", "(ns user)" }
  }));

  /* Test completions with require context - should return namespaces */
  auto responses(eng.handle(make_message({
    {      "op", "completions" },
    {  "prefix",     "sample." },
    { "context",   ":require [" }
  })));

  REQUIRE(responses.size() == 1);
  auto const &payload(responses.front());
  auto const &completions(payload.at("completions").as_list());
  REQUIRE_FALSE(completions.empty());

  bool found_server{ false };
  bool found_client{ false };
  for(auto const &entry : completions)
  {
    auto const &dict(entry.as_dict());
    auto const &candidate(dict.at("candidate").as_string());
    auto const &type(dict.at("type").as_string());

    /* All results should be namespace type */
    CHECK(type == "namespace");

    if(candidate == "sample.server")
    {
      found_server = true;
    }
    if(candidate == "sample.client")
    {
      found_client = true;
    }
  }

  CHECK(found_server);
  CHECK(found_client);
}

TEST_CASE("completions without require context returns vars not namespaces")
{
  engine eng;

  /* Create a namespace with a var that starts with 'clojure' */
  eng.handle(make_message({
    {   "op",                             "eval" },
    { "code", "(def clojure-version-str \"1.0\")" }
  }));

  /* Test completions without require context - should return vars, not namespaces */
  auto responses(eng.handle(make_message({
    {     "op", "completions" },
    { "prefix",    "clojure-" }
  })));

  REQUIRE(responses.size() == 1);
  auto const &payload(responses.front());
  auto const &completions(payload.at("completions").as_list());
  REQUIRE_FALSE(completions.empty());

  bool found_var{ false };
  for(auto const &entry : completions)
  {
    auto const &dict(entry.as_dict());
    auto const &candidate(dict.at("candidate").as_string());
    auto const &type(dict.at("type").as_string());

    if(candidate == "clojure-version-str")
    {
      found_var = true;
      CHECK(type == "var");
    }
  }

  CHECK(found_var);
}

TEST_CASE("completions op returns interned keywords with colon prefix")
{
  /* This test verifies that the 'completions' op (older CIDER style) also works for keywords */
  engine eng;

  /* Use some keywords to intern them */
  eng.handle(make_message({
    {   "op",                                              "eval" },
    { "code", "(def data {:test-keyword 1 :test-other 2})" }
  }));

  /* Complete keywords starting with :test using 'completions' op */
  auto responses(eng.handle(make_message({
    {     "op", "completions" },
    { "prefix",       ":test" }
  })));

  REQUIRE(responses.size() == 1);
  auto const &payload(responses.front());
  auto const &completions(payload.at("completions").as_list());

  bool found_test_keyword{ false };
  bool found_test_other{ false };

  for(auto const &entry : completions)
  {
    auto const &dict(entry.as_dict());
    auto const &candidate(dict.at("candidate").as_string());

    if(candidate == ":test-keyword")
    {
      found_test_keyword = true;
      CHECK(dict.at("type").as_string() == "keyword");
    }
    if(candidate == ":test-other")
    {
      found_test_other = true;
      CHECK(dict.at("type").as_string() == "keyword");
    }
  }

  CHECK(found_test_keyword);
  CHECK(found_test_other);
}

  }
}
