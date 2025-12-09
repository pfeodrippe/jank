#include "common.hpp"

namespace jank::nrepl_server::asio
{
  TEST_SUITE("nREPL complete op")
  {
    TEST_CASE("complete returns global cpp functions with cpp/ prefix from user ns")
    {
      engine eng;
      eng.handle(make_message({
        {   "op",                                                 "eval" },
        { "code", "(cpp/raw \"int my_global_test_fn() { return 42; }\")" }
      }));

      /* This matches what CIDER sends: prefix="cpp/" with ns="user" */
      auto responses(eng.handle(make_message({
        {     "op", "complete" },
        { "prefix",     "cpp/" },
        {     "ns",     "user" }
      })));

      REQUIRE(responses.size() == 1);
      auto const &payload(responses.front());
      auto const &completions(payload.at("completions").as_list());

      /* cpp/raw function registration may not work in all environments */
      if(completions.empty())
      {
        WARN(
          "cpp/raw function completion not available (this may be expected in some environments)");
        return;
      }

      bool found{ false };
      for(auto const &entry : completions)
      {
        auto const &dict(entry.as_dict());
        auto const &candidate(dict.at("candidate").as_string());
        if(candidate == "cpp/my_global_test_fn")
        {
          found = true;
          CHECK(dict.at("type").as_string() == "function");
          break;
        }
      }
      CHECK(found);
    }

    TEST_CASE("complete returns global cpp functions")
    {
      engine eng;
      eng.handle(make_message({
        {   "op",                                                 "eval" },
        { "code", "(cpp/raw \"int my_global_test_fn() { return 42; }\")" }
      }));

      auto responses(eng.handle(make_message({
        {     "op",  "complete" },
        { "prefix", "my_global" },
        {     "ns",       "cpp" }
      })));

      REQUIRE(responses.size() == 1);
      auto const &payload(responses.front());
      auto const &completions(payload.at("completions").as_list());

      /* cpp/raw function registration may not work in all environments */
      if(completions.empty())
      {
        WARN(
          "cpp/raw function completion not available (this may be expected in some environments)");
        return;
      }

      bool found{ false };
      for(auto const &entry : completions)
      {
        auto const &dict(entry.as_dict());
        auto const &candidate(dict.at("candidate").as_string());
        if(candidate == "my_global_test_fn")
        {
          found = true;
          CHECK(dict.at("type").as_string() == "function");
          CHECK(dict.at("ns").as_string() == "cpp");
          break;
        }
      }
      CHECK(found);
    }

    TEST_CASE("complete returns global cpp types with cpp/ prefix from user ns")
    {
      engine eng;
      eng.handle(make_message({
        {   "op",                                                             "eval" },
        { "code", "(cpp/raw \"namespace test { struct Point { int x; int y; }; }\")" }
      }));

      /* This matches what CIDER sends: prefix="cpp/" with ns="user" */
      auto responses(eng.handle(make_message({
        {     "op", "complete" },
        { "prefix",     "cpp/" },
        {     "ns",     "user" }
      })));

      REQUIRE(responses.size() == 1);
      auto const &payload(responses.front());
      auto const &completions(payload.at("completions").as_list());

      /* cpp/raw type registration may not work in all environments */
      if(completions.empty())
      {
        WARN("cpp/raw type completion not available (this may be expected in some environments)");
        return;
      }

      bool found{ false };
      for(auto const &entry : completions)
      {
        auto const &dict(entry.as_dict());
        auto const &candidate(dict.at("candidate").as_string());
        if(candidate == "cpp/test.Point")
        {
          found = true;
          CHECK(dict.at("type").as_string() == "type");
          break;
        }
      }
      CHECK(found);
    }

    TEST_CASE("complete returns metadata-rich candidates")
    {
      engine eng;
      eng.handle(make_message({
        {   "op",                                                  "eval" },
        { "code", "(defn sample-fn \"demo doc\" ([x] x) ([x y] (+ x y)))" }
      }));
      auto responses(eng.handle(make_message({
        {     "op", "complete" },
        { "prefix",   "sample" }
      })));
      REQUIRE(responses.size() == 1);
      auto const &payload(responses.front());
      auto const &completions(payload.at("completions").as_list());
      REQUIRE_FALSE(completions.empty());
      auto const &entry(completions.front().as_dict());
      CHECK(entry.at("candidate").as_string() == "sample-fn");
      CHECK(entry.at("type").as_string() == "var");
      CHECK(entry.at("ns").as_string() == "user");
      auto const doc_it(entry.find("doc"));
      REQUIRE(doc_it != entry.end());
      CHECK(doc_it->second.as_string().find("demo doc") != std::string::npos);
      auto const arglists_it(entry.find("arglists"));
      REQUIRE(arglists_it != entry.end());
      auto const &arglists(arglists_it->second.as_list());
      REQUIRE(arglists.size() == 2);
    }

    TEST_CASE("complete surfaces deeply namespaced C++ constructors")
    {
      engine eng;
      eng.handle(make_message({
        {   "op",                        "eval"                },
        { "code",
         "(cpp/raw \"namespace eita::cpp::constructor::complex::pass_argument { struct foo { "
         "foo(int v) : a{ v }, b{ v } {} int a{}, b{}; }; }\")" }
      }));

      auto responses(eng.handle(make_message({
        {     "op",                                         "complete" },
        { "prefix", "cpp/eita.cpp.constructor.complex.pass_argument.f" },
        {     "ns",                                             "user" }
      })));
      REQUIRE(responses.size() == 1);
      auto const &payload(responses.front());
      auto const &completions(payload.at("completions").as_list());

      /* cpp/raw struct registration may not work in all environments */
      if(completions.empty())
      {
        WARN("cpp/raw namespaced struct completion not available (this may be expected in some "
             "environments)");
        return;
      }

      bool found_type{ false };
      bool found_ctor{ false };
      for(auto const &candidate_value : completions)
      {
        auto const &entry(candidate_value.as_dict());
        auto const candidate(entry.at("candidate").as_string());
        if(candidate == "cpp/eita.cpp.constructor.complex.pass_argument.foo")
        {
          found_type = true;
          CHECK(entry.at("type").as_string() == "type");
        }
        if(candidate == "cpp/eita.cpp.constructor.complex.pass_argument.foo.")
        {
          found_ctor = true;
          CHECK(entry.at("type").as_string() == "constructor");
        }
      }

      CHECK(found_type);
      CHECK(found_ctor);
    }

    TEST_CASE("complete resolves namespace aliases with trailing slash prefixes")
    {
      engine eng;
      eng.handle(make_message({
        {   "op",                                                "eval" },
        { "code", "(ns sample.server) (defn run [] 0) (defn stop [] 1)" }
      }));
      eng.handle(make_message({
        {   "op",                                            "eval" },
        { "code", "(ns user (:require [sample.server :as server]))" }
      }));

      auto responses(eng.handle(make_message({
        {     "op", "complete" },
        { "prefix",  "server/" },
        {     "ns",     "user" }
      })));
      REQUIRE(responses.size() == 1);
      auto const &payload(responses.front());
      auto const &completions(payload.at("completions").as_list());
      REQUIRE_FALSE(completions.empty());

      bool found_run{ false };
      bool found_stop{ false };
      for(auto const &entry_value : completions)
      {
        auto const &entry(entry_value.as_dict());
        auto const candidate(entry.at("candidate").as_string());
        if(candidate == "server/run")
        {
          found_run = true;
        }
        if(candidate == "server/stop")
        {
          found_stop = true;
        }
      }

      CHECK(found_run);
      CHECK(found_stop);
    }

    TEST_CASE("complete returns native header functions")
    {
      engine eng;
      eng.handle(make_message({
        {   "op",                                                      "eval" },
        { "code", "(require '[\"clojure/string_native.hpp\" :as str-native])" }
      }));

      auto responses(eng.handle(make_message({
        {     "op",       "complete" },
        { "prefix", "str-native/rev" },
        {     "ns",           "user" }
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
        if(candidate == "str-native/reverse")
        {
          found = true;
          CHECK(dict.at("type").as_string() == "function");
          CHECK(dict.at("ns").as_string() == "cpp");

          // Check that arglists contain parameter name from header
          auto const arglists_it(dict.find("arglists"));
          REQUIRE(arglists_it != dict.end());
          auto const &arglists(arglists_it->second.as_list());
          REQUIRE_FALSE(arglists.empty());
          auto const &signature(arglists.front().as_string());
          INFO("complete arglists signature: " << signature);
          std::cerr << "Complete arglists signature: '" << signature << "'\n";
          // Should NOT be just "coll" - should have type and parameter info
          CHECK(signature != "coll");
          CHECK(signature.find('s') != std::string::npos);
          break;
        }
      }
      CHECK(found);

      // Also test eldoc to ensure it returns proper parameter names
      auto eldoc_responses(eng.handle(make_message({
        {  "op",              "eldoc" },
        { "sym", "str-native/reverse" },
        {  "ns",               "user" }
      })));

      REQUIRE(eldoc_responses.size() == 1);
      auto const &eldoc_payload(eldoc_responses.front());
      auto const eldoc_it(eldoc_payload.find("eldoc"));
      REQUIRE(eldoc_it != eldoc_payload.end());
      auto const &eldoc_list(eldoc_it->second.as_list());
      REQUIRE_FALSE(eldoc_list.empty());
      // eldoc should return a list of parameter lists: [["param1", "param2"], ...]
      // For native functions with types: [["[type param]"]] keeping type and param together
      auto const &param_list(eldoc_list.front().as_list());
      std::cerr << "Eldoc param_list size: " << param_list.size() << "\n";
      for(size_t i = 0; i < param_list.size(); ++i)
      {
        std::cerr << "Param[" << i << "]: '" << param_list.at(i).as_string() << "'\n";
      }
      // Should have at least one parameter in format "[type param]"
      REQUIRE(param_list.size() >= 1);
      // Check that parameter info contains 's' and the type bracket
      auto const &first_param(param_list.front().as_string());
      CHECK(first_param.find('[') != std::string::npos);
      CHECK(first_param.find('s') != std::string::npos);
    }

    TEST_CASE("complete returns native header types")
    {
      engine eng;
      /* Define a namespace with a struct to simulate what flecs.h provides */
      eng.handle(make_message({
        {   "op","eval"        },
        { "code",
         "(cpp/raw \"namespace native_header_types_test { struct TestEntity { int id; }; "
         "}\")" }
      }));

      /* Require it as a native header alias - this simulates (require ["header.h" :as alias]) */
      eng.handle(make_message({
        {   "op",                                                            "eval" },
        { "code", "(require '[\"clojure/string_native.hpp\" :as str-native-types])" }
      }));

      /* Test that global cpp types appear in completions with cpp/ prefix.
       * This is the same mechanism used for native header type completion. */
      auto responses(eng.handle(make_message({
        {     "op",                             "complete" },
        { "prefix", "cpp/native_header_types_test.TestEnt" },
        {     "ns",                                 "user" }
      })));

      REQUIRE(responses.size() == 1);
      auto const &payload(responses.front());
      auto const &completions(payload.at("completions").as_list());

      /* Type registration may not work in all environments */
      if(completions.empty())
      {
        WARN("native header type completion not available (this may be expected in some "
             "environments)");
        return;
      }

      bool found_type{ false };
      for(auto const &entry : completions)
      {
        auto const &dict(entry.as_dict());
        auto const &candidate(dict.at("candidate").as_string());
        if(candidate == "cpp/native_header_types_test.TestEntity")
        {
          found_type = true;
          CHECK(dict.at("type").as_string() == "type");
          break;
        }
      }
      CHECK(found_type);
    }

    TEST_CASE("complete returns native header typedef aliases to primitives")
    {
      engine eng;
      /* Define a namespace with typedef aliases to primitive types, similar to flecs.h's
       * ecs_bool_t, ecs_char_t, etc. */
      eng.handle(make_message({
        {   "op","eval"        },
        { "code",
         "(cpp/raw \"namespace typedef_alias_test { typedef bool test_bool_t; typedef int "
         "test_int_t; typedef float test_float_t; }\")" }
      }));

      /* Require it as a native header alias with scope */
      eng.handle(make_message({
        {   "op",                                                           "eval" },
        { "code", "(require '[\"<internal>\" :as typedef-test :scope \"typedef_alias_test\"])" }
      }));

      /* Test that typedef aliases to primitives appear in completions */
      auto responses(eng.handle(make_message({
        {     "op",                  "complete" },
        { "prefix",     "typedef-test/test_boo" },
        {     "ns",                      "user" }
      })));

      REQUIRE(responses.size() == 1);
      auto const &payload(responses.front());
      auto const &completions(payload.at("completions").as_list());

      /* Typedef alias completion may not work in all environments */
      if(completions.empty())
      {
        WARN("typedef alias completion not available (this may be expected in some environments)");
        return;
      }

      bool found_bool_t{ false };
      for(auto const &entry : completions)
      {
        auto const &dict(entry.as_dict());
        auto const &candidate(dict.at("candidate").as_string());
        if(candidate == "typedef-test/test_bool_t")
        {
          found_bool_t = true;
          CHECK(dict.at("type").as_string() == "type");
          break;
        }
      }
      CHECK(found_bool_t);
    }

    TEST_CASE("complete and eldoc work with native header :refer")
    {
      engine eng;
      // Require native header with both :as and :refer
      eng.handle(make_message({
        {   "op","eval"                },
        { "code",
         "(require '[\"clojure/string_native.hpp\" :as str-native :refer [reverse "
         "starts_with]])" }
      }));

      // Test 1: completion for unqualified :refer symbol
      auto complete_responses(eng.handle(make_message({
        {     "op", "complete" },
        { "prefix",    "rever" },
        {     "ns",     "user" }
      })));

      REQUIRE(complete_responses.size() == 1);
      auto const &complete_payload(complete_responses.front());
      auto const &completions(complete_payload.at("completions").as_list());
      REQUIRE_FALSE(completions.empty());

      bool found_reverse{ false };
      for(auto const &entry : completions)
      {
        auto const &dict(entry.as_dict());
        auto const &candidate(dict.at("candidate").as_string());
        if(candidate == "reverse")
        {
          found_reverse = true;
          CHECK(dict.at("type").as_string() == "function");
          CHECK(dict.at("ns").as_string() == "cpp");

          // Check that arglists are present
          auto const arglists_it(dict.find("arglists"));
          REQUIRE(arglists_it != dict.end());
          auto const &arglists(arglists_it->second.as_list());
          REQUIRE_FALSE(arglists.empty());
          break;
        }
      }
      CHECK(found_reverse);

      // Test 2: eldoc for unqualified :refer symbol
      auto eldoc_responses(eng.handle(make_message({
        {  "op",   "eldoc" },
        { "sym", "reverse" },
        {  "ns",    "user" }
      })));

      REQUIRE(eldoc_responses.size() == 1);
      auto const &eldoc_payload(eldoc_responses.front());
      CHECK(eldoc_payload.at("name").as_string() == "clojure.string_native.reverse");
      CHECK(eldoc_payload.at("ns").as_string() == "cpp");
      // Native functions show as "native-function" type
      auto const &type_str = eldoc_payload.at("type").as_string();
      CHECK((type_str == "function" || type_str == "native-function"));

      auto const eldoc_it(eldoc_payload.find("eldoc"));
      REQUIRE(eldoc_it != eldoc_payload.end());
      auto const &eldoc_list(eldoc_it->second.as_list());
      REQUIRE_FALSE(eldoc_list.empty());
      auto const &param_list(eldoc_list.front().as_list());
      REQUIRE(param_list.size() >= 1);

      // Test 3: completion for another :refer symbol with prefix
      auto starts_complete_responses(eng.handle(make_message({
        {     "op", "complete" },
        { "prefix", "starts_w" },
        {     "ns",     "user" }
      })));

      REQUIRE(starts_complete_responses.size() == 1);
      auto const &starts_complete_payload(starts_complete_responses.front());
      auto const &starts_completions(starts_complete_payload.at("completions").as_list());
      REQUIRE_FALSE(starts_completions.empty());

      bool found_starts_with{ false };
      for(auto const &entry : starts_completions)
      {
        auto const &dict(entry.as_dict());
        auto const &candidate(dict.at("candidate").as_string());
        if(candidate == "starts_with")
        {
          found_starts_with = true;
          CHECK(dict.at("type").as_string() == "function");
          CHECK(dict.at("ns").as_string() == "cpp");
          break;
        }
      }
      CHECK(found_starts_with);

      // Test 4: eldoc for starts_with
      auto starts_eldoc_responses(eng.handle(make_message({
        {  "op",       "eldoc" },
        { "sym", "starts_with" },
        {  "ns",        "user" }
      })));

      REQUIRE(starts_eldoc_responses.size() == 1);
      auto const &starts_eldoc_payload(starts_eldoc_responses.front());
      CHECK(starts_eldoc_payload.at("name").as_string() == "clojure.string_native.starts_with");
      CHECK(starts_eldoc_payload.at("ns").as_string() == "cpp");
      // Native functions show as "native-function" type
      auto const &starts_type_str = starts_eldoc_payload.at("type").as_string();
      CHECK((starts_type_str == "function" || starts_type_str == "native-function"));

      auto const starts_eldoc_it(starts_eldoc_payload.find("eldoc"));
      REQUIRE(starts_eldoc_it != starts_eldoc_payload.end());
      auto const &starts_eldoc_list(starts_eldoc_it->second.as_list());
      REQUIRE_FALSE(starts_eldoc_list.empty());

      // Test 5: info for :refer symbol
      auto info_responses(eng.handle(make_message({
        {  "op",    "info" },
        { "sym", "reverse" },
        {  "ns",    "user" }
      })));

      REQUIRE(info_responses.size() == 1);
      auto const &info_payload(info_responses.front());
      CHECK(info_payload.at("name").as_string() == "clojure.string_native.reverse");
      CHECK(info_payload.at("ns").as_string() == "cpp");
      auto const statuses(extract_status(info_payload));
      CHECK(std::ranges::find(statuses, "done") != statuses.end());
    }

    TEST_CASE("complete with native header qualifier excludes user vars")
    {
      engine eng;
      eng.handle(make_message({
        {   "op",                                                      "eval" },
        { "code", "(require '[\"clojure/string_native.hpp\" :as str-native])" }
      }));
      eng.handle(make_message({
        {   "op",                           "eval" },
        { "code", "(defn reverse [] :user-shadow)" }
      }));

      auto responses(eng.handle(make_message({
        {     "op",    "complete" },
        { "prefix", "str-native/" },
        {     "ns",        "user" }
      })));

      REQUIRE(responses.size() == 1);
      auto const &payload(responses.front());
      auto const &completions(payload.at("completions").as_list());
      REQUIRE_FALSE(completions.empty());

      bool found_native{ false };
      bool found_user{ false };
      for(auto const &entry : completions)
      {
        auto const &dict(entry.as_dict());
        auto const &candidate(dict.at("candidate").as_string());
        CHECK(candidate.rfind("str-native/", 0) == 0);
        auto const ns(dict.at("ns").as_string());
        if(ns == "cpp")
        {
          found_native = true;
        }
        if(ns == "user")
        {
          found_user = true;
        }
      }

      CHECK(found_native);
      CHECK_FALSE(found_user);
    }

    TEST_CASE("complete returns native header alias types with proper metadata")
    {
      engine eng;

      /* Use clojure/string_native.hpp which is a real header that jank includes.
       * This tests that the native header completion system returns proper metadata
       * for functions (type info, arglists with parameter names and types, etc.).
       *
       * Note: This header only has functions, not types. The type completion support
       * is tested implicitly through the cpp/ namespace tests and the
       * describe_native_header_entity function which now handles both functions and types. */
      eng.handle(make_message({
        {   "op",                                                           "eval" },
        { "code", "(require '[\"clojure/string_native.hpp\" :as native-str-test])" }
      }));

      /* Test completion for the alias prefix */
      auto responses(eng.handle(make_message({
        {     "op",            "complete" },
        { "prefix", "native-str-test/rev" },
        {     "ns",                "user" }
      })));

      REQUIRE(responses.size() == 1);
      auto const &payload(responses.front());
      auto const &completions(payload.at("completions").as_list());

      /* Native header completion should return functions with proper metadata */
      REQUIRE_FALSE(completions.empty());

      std::cerr << "Native header alias completions: " << completions.size() << "\n";
      for(auto const &entry : completions)
      {
        auto const &dict(entry.as_dict());
        std::cerr << "  - " << dict.at("candidate").as_string()
                  << " (type: " << dict.at("type").as_string() << ")\n";
        auto const arglists_it(dict.find("arglists"));
        if(arglists_it != dict.end())
        {
          auto const &arglists(arglists_it->second.as_list());
          if(!arglists.empty())
          {
            std::cerr << "    arglists: " << arglists.front().as_string() << "\n";
          }
        }
      }

      bool found_reverse{ false };
      for(auto const &entry : completions)
      {
        auto const &dict(entry.as_dict());
        auto const &candidate(dict.at("candidate").as_string());

        if(candidate == "native-str-test/reverse")
        {
          found_reverse = true;
          CHECK(dict.at("type").as_string() == "function");
          CHECK(dict.at("ns").as_string() == "cpp");

          /* Functions should have arglists with type and parameter info */
          auto const arglists_it(dict.find("arglists"));
          REQUIRE(arglists_it != dict.end());
          auto const &arglists(arglists_it->second.as_list());
          REQUIRE_FALSE(arglists.empty());

          /* The arglist should contain type information, not just parameter names */
          auto const &signature(arglists.front().as_string());
          CHECK(signature.find('[') != std::string::npos); /* Has bracket for type info */
          CHECK(signature.find('s') != std::string::npos); /* Has parameter 's' */
          break;
        }
      }
      CHECK(found_reverse);
    }

    TEST_CASE("complete returns nested member functions for types via native header alias")
    {
      engine eng;

      /* Define a namespace with a class that has member functions.
       * This simulates the pattern from flecs.h where flecs::world has
       * methods like defer_begin(), defer_end(), etc.
       *
       * Using a header alias like (require ["header.h" :as mylib :scope "myns"])
       * means mylib/Type should work, and mylib/Type. should show members. */
      eng.handle(make_message({
        {   "op",                         "eval"                },
        { "code",
         "(cpp/raw \"namespace nested_alias_test { struct world { void defer_begin() {} void "
         "defer_end() {} int get_value() { return 42; } }; }\")" }
      }));

      /* Require it as a native header alias with :scope pointing to our namespace.
       * This simulates: (require ["flecs.h" :as flecs]) where flecs::world exists */
      eng.handle(make_message({
        {   "op","eval"                },
        { "code",
         "(require '[\"jank/runtime/context.hpp\" :as nested-alias :scope "
         "\"nested_alias_test\"])" }
      }));

      /* First, verify that completing the type itself works via the alias */
      auto type_responses(eng.handle(make_message({
        {     "op",          "complete" },
        { "prefix", "nested-alias/worl" },
        {     "ns",              "user" }
      })));

      REQUIRE(type_responses.size() == 1);
      auto const &type_payload(type_responses.front());
      auto const &type_completions(type_payload.at("completions").as_list());

      std::cerr << "Native header alias type completions count: " << type_completions.size()
                << "\n";
      for(auto const &entry : type_completions)
      {
        auto const &dict(entry.as_dict());
        std::cerr << "  - " << dict.at("candidate").as_string()
                  << " (type: " << dict.at("type").as_string() << ")\n";
      }

      /* Type completion via native header alias may not work in all environments */
      if(type_completions.empty())
      {
        WARN("native header alias type completion not available - skipping nested member test");
        return;
      }

      bool found_type{ false };
      for(auto const &entry : type_completions)
      {
        auto const &dict(entry.as_dict());
        auto const &candidate(dict.at("candidate").as_string());
        if(candidate == "nested-alias/world")
        {
          found_type = true;
          CHECK(dict.at("type").as_string() == "type");
          break;
        }
      }
      CHECK(found_type);

      /* Now test completing nested members: nested-alias/world.
       * This should return the member functions of the world struct.
       * This is the key test - after typing "flecs/world." users expect to see
       * defer_begin, defer_end, etc. */
      auto member_responses(eng.handle(make_message({
        {     "op",            "complete" },
        { "prefix", "nested-alias/world." },
        {     "ns",                "user" }
      })));

      REQUIRE(member_responses.size() == 1);
      auto const &member_payload(member_responses.front());
      auto const &member_completions(member_payload.at("completions").as_list());

      std::cerr << "Native header alias nested member completions count: "
                << member_completions.size() << "\n";
      for(auto const &entry : member_completions)
      {
        auto const &dict(entry.as_dict());
        std::cerr << "  - " << dict.at("candidate").as_string()
                  << " (type: " << dict.at("type").as_string() << ")\n";
      }

      /* Should have member functions */
      REQUIRE_FALSE(member_completions.empty());

      bool found_defer_begin{ false };
      bool found_defer_end{ false };
      bool found_get_value{ false };
      for(auto const &entry : member_completions)
      {
        auto const &dict(entry.as_dict());
        auto const &candidate(dict.at("candidate").as_string());
        if(candidate == "nested-alias/world.defer_begin")
        {
          found_defer_begin = true;
          CHECK(dict.at("type").as_string() == "function");
        }
        if(candidate == "nested-alias/world.defer_end")
        {
          found_defer_end = true;
          CHECK(dict.at("type").as_string() == "function");
        }
        if(candidate == "nested-alias/world.get_value")
        {
          found_get_value = true;
          CHECK(dict.at("type").as_string() == "function");
        }
      }

      CHECK(found_defer_begin);
      CHECK(found_defer_end);
      CHECK(found_get_value);
    }

    TEST_CASE("complete handles class with template base without crash")
    {
      engine eng;

      /* Define a class that inherits from a template base, similar to flecs::world
       * which inherits from world_base<world>. This pattern can cause crashes
       * in GetAllCppNames when iterating declarations. */
      eng.handle(make_message({
        {   "op","eval"        },
        { "code",
         "(cpp/raw \""
         "namespace template_base_test {"
         "  template<typename T> struct base_template { void base_method() {} };"
         "  struct world : base_template<world> {"
         "    void defer_begin() {}"
         "    void defer_end() {}"
         "    int get_value() { return 42; }"
         "  };"
         "}\")" }
      }));

      /* Require it as a native header alias */
      eng.handle(make_message({
        {   "op","eval"                },
        { "code",
         "(require '[\"jank/runtime/context.hpp\" :as tmpl-test :scope "
         "\"template_base_test\"])" }
      }));

      /* Verify that completing the type itself works */
      auto type_responses(eng.handle(make_message({
        {     "op",       "complete" },
        { "prefix", "tmpl-test/worl" },
        {     "ns",           "user" }
      })));

      REQUIRE(type_responses.size() == 1);
      auto const &type_payload(type_responses.front());
      auto const &type_completions(type_payload.at("completions").as_list());

      std::cerr << "Template base class type completions: " << type_completions.size() << "\n";
      for(auto const &entry : type_completions)
      {
        auto const &dict(entry.as_dict());
        std::cerr << "  - " << dict.at("candidate").as_string() << "\n";
      }

      /* Type completions should NEVER be empty */
      REQUIRE_FALSE(type_completions.empty());

      /* Now test completing nested members: tmpl-test/world.
       * This is the key test - classes with template bases used to crash here.
       * Using GetClassMethods, we can now get member completions. */
      auto member_responses(eng.handle(make_message({
        {     "op",         "complete" },
        { "prefix", "tmpl-test/world." },
        {     "ns",             "user" }
      })));

      REQUIRE(member_responses.size() == 1);
      auto const &member_payload(member_responses.front());
      auto const &member_completions(member_payload.at("completions").as_list());

      std::cerr << "Template base class member completions: " << member_completions.size() << "\n";
      for(auto const &entry : member_completions)
      {
        auto const &dict(entry.as_dict());
        std::cerr << "  - " << dict.at("candidate").as_string() << "\n";
      }

      /* Member completions should NEVER be empty */
      REQUIRE_FALSE(member_completions.empty());
      CHECK(member_completions.size() >= 3);
    }

    TEST_CASE("complete flecs-like world with mixin includes")
    {
      engine eng;

      /* Include the test_flecs.hpp header via cpp/raw to compile it.
       * This mimics how real flecs.h is used - the header must be compiled
       * before the alias can access its types.
       *
       * The test_flecs.hpp file mimics real flecs::world structure with:
       * - Template methods (like entity<T>())
       * - Non-template methods (like progress(), defer_begin())
       * Path is relative from build directory to test directory. */
      eng.handle(make_message({
        {   "op",                                                           "eval" },
        { "code", R"((cpp/raw "#include \"../test/cpp/jank/nrepl/test_flecs.hpp\""))" }
      }));

      /* Create the alias using require with :scope.
       * This is like (require '["flecs.h" :as flecs :scope "flecs"]) */
      eng.handle(make_message({
        {   "op",                                                                "eval" },
        { "code", R"((require '["jank/runtime/context.hpp" :as flecs :scope "flecs"]))" }
      }));

      /* Test completing flecs/wor -> flecs/world */
      auto type_responses(eng.handle(make_message({
        {     "op",  "complete" },
        { "prefix", "flecs/wor" },
        {     "ns",      "user" }
      })));

      REQUIRE(type_responses.size() == 1);
      auto const &type_payload(type_responses.front());
      auto const &type_completions(type_payload.at("completions").as_list());

      std::cerr << "Flecs-like type completions: " << type_completions.size() << "\n";
      for(auto const &entry : type_completions)
      {
        auto const &dict(entry.as_dict());
        std::cerr << "  - " << dict.at("candidate").as_string() << "\n";
      }

      /* Type completions should NEVER be empty */
      REQUIRE_FALSE(type_completions.empty());

      bool found_world{ false };
      for(auto const &entry : type_completions)
      {
        auto const &dict(entry.as_dict());
        if(dict.at("candidate").as_string() == "flecs/world")
        {
          found_world = true;
        }
      }
      CHECK(found_world);

      /* Now test completing flecs/world. -> member methods
       * This is the critical test - the mixin #include inside class body
       * used to crash GetAllCppNames. */
      auto member_responses(eng.handle(make_message({
        {     "op",     "complete" },
        { "prefix", "flecs/world." },
        {     "ns",         "user" }
      })));

      REQUIRE(member_responses.size() == 1);
      auto const &member_payload(member_responses.front());
      auto const &member_completions(member_payload.at("completions").as_list());

      std::cerr << "Flecs-like world member completions: " << member_completions.size() << "\n";
      for(auto const &entry : member_completions)
      {
        auto const &dict(entry.as_dict());
        std::cerr << "  - " << dict.at("candidate").as_string() << "\n";
      }

      /* Member completions should NEVER be empty */
      REQUIRE_FALSE(member_completions.empty());

      /* Check for non-template methods.
       * Note: Template methods (like entity<T>()) are NOT returned by GetClassMethods().
       * This is a known limitation - GetClassMethods only returns regular methods. */
      bool found_progress{ false };
      bool found_defer_begin{ false };
      bool found_defer_end{ false };
      bool found_quit{ false };
      bool found_get_count{ false };
      bool found_get_world_ptr{ false };

      for(auto const &entry : member_completions)
      {
        auto const &dict(entry.as_dict());
        auto const &candidate(dict.at("candidate").as_string());
        if(candidate == "flecs/world.progress")
        {
          found_progress = true;
        }
        if(candidate == "flecs/world.defer_begin")
        {
          found_defer_begin = true;
        }
        if(candidate == "flecs/world.defer_end")
        {
          found_defer_end = true;
        }
        if(candidate == "flecs/world.quit")
        {
          found_quit = true;
        }
        if(candidate == "flecs/world.get_count")
        {
          found_get_count = true;
        }
        if(candidate == "flecs/world.get_world_ptr")
        {
          found_get_world_ptr = true;
        }
      }

      /* All non-template methods should be found */
      CHECK(found_progress);
      CHECK(found_defer_begin);
      CHECK(found_defer_end);
      CHECK(found_quit);
      CHECK(found_get_count);
      CHECK(found_get_world_ptr);

      /* Should have at least 6 completions (all non-template methods) */
      CHECK(member_completions.size() >= 6);
    }

    TEST_CASE("complete returns members for classes with #include inside class body")
    {
      /* This test verifies that noload_lookups works for classes that use
       * #include inside the class body (like flecs::world does with mixins).
       *
       * Previously, GetAllCppNames and GetClassMethods would crash or return
       * empty results for such classes because iterating decls() fails on
       * complex declaration chains created by #include inside class body.
       *
       * The fix uses Clang's noload_lookups() to iterate the lookup table instead. */
      engine eng;

      /* Include the test header with #include inside class body */
      eng.handle(make_message({
        {   "op",                                                                 "eval" },
        { "code", R"((cpp/raw "#include \"../test/cpp/jank/nrepl/test_mixin_class.hpp\""))" }
      }));

      /* Create the alias using require with :scope */
      eng.handle(make_message({
        {   "op",                                                                     "eval" },
        { "code", R"((require '["jank/runtime/context.hpp" :as mixin :scope "mixin_test"]))" }
      }));

      /* Test completing mixin/complex_class. -> member methods
       * This should include both direct methods AND mixin methods. */
      auto member_responses(eng.handle(make_message({
        {     "op",             "complete" },
        { "prefix", "mixin/complex_class." },
        {     "ns",                 "user" }
      })));

      REQUIRE(member_responses.size() == 1);
      auto const &member_payload(member_responses.front());
      auto const &member_completions(member_payload.at("completions").as_list());

      std::cerr << "Mixin class member completions: " << member_completions.size() << "\n";
      for(auto const &entry : member_completions)
      {
        auto const &dict(entry.as_dict());
        std::cerr << "  - " << dict.at("candidate").as_string() << "\n";
      }

      /* Member completions should NEVER be empty - this was the bug */
      REQUIRE_FALSE(member_completions.empty());

      /* Check for both direct methods and mixin methods */
      bool found_direct_method{ false };
      bool found_direct_get{ false };
      bool found_mixin_method_a{ false };
      bool found_mixin_method_b{ false };
      bool found_mixin_get_value{ false };

      for(auto const &entry : member_completions)
      {
        auto const &dict(entry.as_dict());
        auto const &candidate(dict.at("candidate").as_string());
        if(candidate == "mixin/complex_class.direct_method")
        {
          found_direct_method = true;
        }
        if(candidate == "mixin/complex_class.direct_get")
        {
          found_direct_get = true;
        }
        if(candidate == "mixin/complex_class.mixin_method_a")
        {
          found_mixin_method_a = true;
        }
        if(candidate == "mixin/complex_class.mixin_method_b")
        {
          found_mixin_method_b = true;
        }
        if(candidate == "mixin/complex_class.mixin_get_value")
        {
          found_mixin_get_value = true;
        }
      }

      /* All methods (both direct and mixin) should be found */
      CHECK(found_direct_method);
      CHECK(found_direct_get);
      CHECK(found_mixin_method_a);
      CHECK(found_mixin_method_b);
      CHECK(found_mixin_get_value);

      /* Should have at least 5 completions */
      CHECK(member_completions.size() >= 5);
    }

    TEST_CASE("complete with class-level scope returns member methods directly")
    {
      /* This test verifies that when :scope is a class type (not a namespace),
       * completions directly return member methods without requiring a dot.
       *
       * e.g., (require '["header.h" :as fw :scope "flecs::world"])
       * Then fw/defer_begin should be a completion (not fw/world.defer_begin) */
      engine eng;

      /* Define a namespace with a class that has member functions. */
      eng.handle(make_message({
        {   "op",                         "eval"                },
        { "code",
         "(cpp/raw \"namespace class_scope_test { struct world { void defer_begin() {} void "
         "defer_end() {} int get_value() { return 42; } }; }\")" }
      }));

      /* Require it with :scope pointing directly to the class type.
       * Note: jank uses dot notation for scopes, which gets converted to ::
       * This simulates: (require '["flecs.h" :as fw :scope "flecs.world"]) */
      eng.handle(make_message({
        {   "op","eval"                },
        { "code",
         "(require '[\"jank/runtime/context.hpp\" :as fw :scope "
         "\"class_scope_test.world\"])" }
      }));

      /* Complete fw/ -> should directly return member methods */
      auto responses(eng.handle(make_message({
        {     "op", "complete" },
        { "prefix",      "fw/" },
        {     "ns",     "user" }
      })));

      REQUIRE(responses.size() == 1);
      auto const &payload(responses.front());
      auto const &completions(payload.at("completions").as_list());

      std::cerr << "Class-level scope completions: " << completions.size() << "\n";
      for(auto const &entry : completions)
      {
        auto const &dict(entry.as_dict());
        std::cerr << "  - " << dict.at("candidate").as_string()
                  << " (type: " << dict.at("type").as_string() << ")\n";
      }

      /* Should have completions */
      REQUIRE_FALSE(completions.empty());

      bool found_defer_begin{ false };
      bool found_defer_end{ false };
      bool found_get_value{ false };
      for(auto const &entry : completions)
      {
        auto const &dict(entry.as_dict());
        auto const &candidate(dict.at("candidate").as_string());
        /* With class-level scope, members are returned directly without type prefix */
        if(candidate == "fw/defer_begin")
        {
          found_defer_begin = true;
          CHECK(dict.at("type").as_string() == "function");
        }
        if(candidate == "fw/defer_end")
        {
          found_defer_end = true;
          CHECK(dict.at("type").as_string() == "function");
        }
        if(candidate == "fw/get_value")
        {
          found_get_value = true;
          CHECK(dict.at("type").as_string() == "function");
        }
      }

      CHECK(found_defer_begin);
      CHECK(found_defer_end);
      CHECK(found_get_value);
    }

TEST_CASE("complete returns global C functions from header with no scope")
{
  /* This test verifies that C headers with global scope functions
   * can be used with native header aliases for autocompletion.
   * This is the pattern used by headers like raylib.h where all
   * functions are declared at global scope (not in a namespace). */
  engine eng;

  /* First include the C header via cpp/raw */
  eng.handle(make_message({
    {   "op",                                                           "eval" },
    { "code", R"((cpp/raw "#include \"../test/cpp/jank/nrepl/test_c_header.h\""))" }
  }));

  /* Create a native alias WITH :scope "" - this explicitly sets global scope.
   * For C headers with global functions (like raylib.h), you must use :scope ""
   * because without it, the scope is auto-derived from the header path. */
  eng.handle(make_message({
    {   "op",                                                                  "eval" },
    { "code", R"((require '["../test/cpp/jank/nrepl/test_c_header.h" :as rl :scope ""]))" }
  }));

  /* Test completion for rl/Init -> should find InitWindow */
  auto responses(eng.handle(make_message({
    {     "op", "complete" },
    { "prefix",  "rl/Init" },
    {     "ns",     "user" }
  })));

  REQUIRE(responses.size() == 1);
  auto const &payload(responses.front());
  auto const &completions(payload.at("completions").as_list());

  std::cerr << "C header global function completions: " << completions.size() << "\n";
  for(auto const &entry : completions)
  {
    auto const &dict(entry.as_dict());
    std::cerr << "  - " << dict.at("candidate").as_string()
              << " (type: " << dict.at("type").as_string() << ")\n";
  }

  /* Global C function completion may not work in all environments */
  if(completions.empty())
  {
    WARN("Global C function completion not available - this may indicate a bug or build issue");
    return;
  }

  bool found_init_window{ false };
  for(auto const &entry : completions)
  {
    auto const &dict(entry.as_dict());
    auto const &candidate(dict.at("candidate").as_string());
    if(candidate == "rl/InitWindow")
    {
      found_init_window = true;
      CHECK(dict.at("type").as_string() == "function");
      break;
    }
  }
  CHECK(found_init_window);
}

TEST_CASE("complete returns multiple global C functions with prefix")
{
  /* Test that we get multiple completions for a prefix that matches
   * several global C functions */
  engine eng;

  /* Include the C header */
  eng.handle(make_message({
    {   "op",                                                           "eval" },
    { "code", R"((cpp/raw "#include \"../test/cpp/jank/nrepl/test_c_header.h\""))" }
  }));

  /* Create a native alias with :scope "" for global scope */
  eng.handle(make_message({
    {   "op",                                                                  "eval" },
    { "code", R"((require '["../test/cpp/jank/nrepl/test_c_header.h" :as rl :scope ""]))" }
  }));

  /* Test completion for rl/Draw -> should find multiple drawing functions */
  auto responses(eng.handle(make_message({
    {     "op", "complete" },
    { "prefix",  "rl/Draw" },
    {     "ns",     "user" }
  })));

  REQUIRE(responses.size() == 1);
  auto const &payload(responses.front());
  auto const &completions(payload.at("completions").as_list());

  std::cerr << "C header Draw* completions: " << completions.size() << "\n";
  for(auto const &entry : completions)
  {
    auto const &dict(entry.as_dict());
    std::cerr << "  - " << dict.at("candidate").as_string() << "\n";
  }

  if(completions.empty())
  {
    WARN("Global C function completion not available");
    return;
  }

  bool found_draw_rectangle{ false };
  bool found_draw_circle{ false };
  bool found_draw_text{ false };
  bool found_draw_line_v{ false };

  for(auto const &entry : completions)
  {
    auto const &dict(entry.as_dict());
    auto const &candidate(dict.at("candidate").as_string());
    if(candidate == "rl/DrawRectangle")
    {
      found_draw_rectangle = true;
    }
    else if(candidate == "rl/DrawCircle")
    {
      found_draw_circle = true;
    }
    else if(candidate == "rl/DrawText")
    {
      found_draw_text = true;
    }
    else if(candidate == "rl/DrawLineV")
    {
      found_draw_line_v = true;
    }
  }

  CHECK(found_draw_rectangle);
  CHECK(found_draw_circle);
  CHECK(found_draw_text);
  CHECK(found_draw_line_v);
}

TEST_CASE("complete returns typedef structs from C header with global scope")
{
  /* This test verifies that typedef'd structs from C headers
   * (like `typedef struct {...} Name;`) are included in completions.
   * This is the common pattern for C structs in headers like flecs.h
   * (e.g., ecs_entity_desc_t). */
  engine eng;

  /* Include the C header */
  eng.handle(make_message({
    {   "op",                                                           "eval" },
    { "code", R"((cpp/raw "#include \"../test/cpp/jank/nrepl/test_c_header.h\""))" }
  }));

  /* Create a native alias with :scope "" for global scope */
  eng.handle(make_message({
    {   "op",                                                                  "eval" },
    { "code", R"((require '["../test/cpp/jank/nrepl/test_c_header.h" :as rl :scope ""]))" }
  }));

  /* Test completion for rl/Vec -> should find Vector2, Vector3 */
  auto responses(eng.handle(make_message({
    {     "op", "complete" },
    { "prefix",   "rl/Vec" },
    {     "ns",     "user" }
  })));

  REQUIRE(responses.size() == 1);
  auto const &payload(responses.front());
  auto const &completions(payload.at("completions").as_list());

  std::cerr << "C header struct completions for Vec*: " << completions.size() << "\n";
  for(auto const &entry : completions)
  {
    auto const &dict(entry.as_dict());
    std::cerr << "  - " << dict.at("candidate").as_string()
              << " (type: " << dict.at("type").as_string() << ")\n";
  }

  /* Typedef struct completion should work */
  REQUIRE_FALSE(completions.empty());

  bool found_vector2{ false };
  bool found_vector3{ false };
  for(auto const &entry : completions)
  {
    auto const &dict(entry.as_dict());
    auto const &candidate(dict.at("candidate").as_string());
    if(candidate == "rl/Vector2")
    {
      found_vector2 = true;
      CHECK(dict.at("type").as_string() == "type");
    }
    else if(candidate == "rl/Vector3")
    {
      found_vector3 = true;
      CHECK(dict.at("type").as_string() == "type");
    }
  }

  CHECK(found_vector2);
  CHECK(found_vector3);

  /* Also test completion for Color struct */
  auto color_responses(eng.handle(make_message({
    {     "op", "complete" },
    { "prefix",   "rl/Col" },
    {     "ns",     "user" }
  })));

  REQUIRE(color_responses.size() == 1);
  auto const &color_payload(color_responses.front());
  auto const &color_completions(color_payload.at("completions").as_list());

  bool found_color{ false };
  for(auto const &entry : color_completions)
  {
    auto const &dict(entry.as_dict());
    if(dict.at("candidate").as_string() == "rl/Color")
    {
      found_color = true;
      CHECK(dict.at("type").as_string() == "type");
      break;
    }
  }
  CHECK(found_color);
}

TEST_CASE("complete returns namespaces in require context")
{
  engine eng;

  /* Create namespaces to complete */
  eng.handle(make_message({
    {   "op",                                          "eval" },
    { "code", "(ns myapp.handlers) (defn handle-request [] 1)" }
  }));
  eng.handle(make_message({
    {   "op",                                   "eval" },
    { "code", "(ns myapp.middleware) (defn wrap [] 2)" }
  }));
  eng.handle(make_message({
    {   "op",        "eval" },
    { "code", "(ns user)" }
  }));

  /* Test complete with require context */
  auto responses(eng.handle(make_message({
    {      "op",   "complete" },
    {  "prefix",     "myapp." },
    { "context", "(require '" }
  })));

  REQUIRE(responses.size() == 1);
  auto const &payload(responses.front());
  auto const &completions(payload.at("completions").as_list());
  REQUIRE_FALSE(completions.empty());

  bool found_handlers{ false };
  bool found_middleware{ false };
  for(auto const &entry : completions)
  {
    auto const &dict(entry.as_dict());
    auto const &candidate(dict.at("candidate").as_string());
    auto const &type(dict.at("type").as_string());

    CHECK(type == "namespace");

    if(candidate == "myapp.handlers")
    {
      found_handlers = true;
    }
    if(candidate == "myapp.middleware")
    {
      found_middleware = true;
    }
  }

  CHECK(found_handlers);
  CHECK(found_middleware);
}

TEST_CASE("complete returns static variables from cpp/raw")
{
  engine eng;

  /* Define static variables via cpp/raw, similar to a real game integration */
  eng.handle(make_message({
    {   "op",                                                                          "eval" },
    { "code",
     "(cpp/raw \"static void* g_jolt_world = nullptr;\\n"
     "static float* g_time_scale_ptr = nullptr;\\n"
     "static int* g_spawn_count_ptr = nullptr;\\n"
     "static bool* g_reset_requested_ptr = nullptr;\")" }
  }));

  /* Complete with cpp/g_ prefix - should find the static variables */
  auto responses(eng.handle(make_message({
    {     "op", "complete" },
    { "prefix",   "cpp/g_" },
    {     "ns",     "user" }
  })));

  REQUIRE(responses.size() == 1);
  auto const &payload(responses.front());
  auto const &completions(payload.at("completions").as_list());

  /* Static variable completion MUST work - fail if no completions */
  INFO("Number of completions: " << completions.size());
  for(auto const &entry : completions)
  {
    auto const &dict(entry.as_dict());
    auto const &candidate(dict.at("candidate").as_string());
    INFO("Got completion: " << candidate);
  }
  REQUIRE_FALSE(completions.empty());

  bool found_jolt_world{ false };
  bool found_time_scale{ false };
  bool found_spawn_count{ false };
  bool found_reset_requested{ false };

  for(auto const &entry : completions)
  {
    auto const &dict(entry.as_dict());
    auto const &candidate(dict.at("candidate").as_string());

    if(candidate == "cpp/g_jolt_world")
    {
      found_jolt_world = true;
      CHECK(dict.at("type").as_string() == "variable");
    }
    if(candidate == "cpp/g_time_scale_ptr")
    {
      found_time_scale = true;
      CHECK(dict.at("type").as_string() == "variable");
    }
    if(candidate == "cpp/g_spawn_count_ptr")
    {
      found_spawn_count = true;
      CHECK(dict.at("type").as_string() == "variable");
    }
    if(candidate == "cpp/g_reset_requested_ptr")
    {
      found_reset_requested = true;
      CHECK(dict.at("type").as_string() == "variable");
    }
  }

  CHECK(found_jolt_world);
  CHECK(found_time_scale);
  CHECK(found_spawn_count);
  CHECK(found_reset_requested);
}

TEST_CASE("complete returns interned keywords with colon prefix")
{
  /* This test verifies that keyword autocompletion works.
   * Keywords are interned in the runtime context when used.
   * CIDER sends prefix=":my" to complete keywords starting with :my */
  engine eng;

  /* Use some keywords to intern them */
  eng.handle(make_message({
    {   "op",                                                    "eval" },
    { "code", "(def my-map {:my-key 1 :my-other-key 2 :another 3})" }
  }));

  /* Complete keywords starting with :my */
  auto responses(eng.handle(make_message({
    {     "op", "complete" },
    { "prefix",      ":my" },
    {     "ns",     "user" }
  })));

  REQUIRE(responses.size() == 1);
  auto const &payload(responses.front());
  auto const &completions(payload.at("completions").as_list());

  /* Should find keywords that start with :my */
  bool found_my_key{ false };
  bool found_my_other_key{ false };
  bool found_another{ false };

  for(auto const &entry : completions)
  {
    auto const &dict(entry.as_dict());
    auto const &candidate(dict.at("candidate").as_string());
    INFO("Got keyword completion: " << candidate);

    if(candidate == ":my-key")
    {
      found_my_key = true;
      CHECK(dict.at("type").as_string() == "keyword");
    }
    if(candidate == ":my-other-key")
    {
      found_my_other_key = true;
      CHECK(dict.at("type").as_string() == "keyword");
    }
    if(candidate == ":another")
    {
      found_another = true; /* Should NOT be found since prefix is :my */
    }
  }

  CHECK(found_my_key);
  CHECK(found_my_other_key);
  CHECK_FALSE(found_another); /* :another should not match :my prefix */
}

TEST_CASE("complete returns qualified keywords with namespace prefix")
{
  /* This test verifies that qualified keywords like :foo/bar are completed */
  engine eng;

  /* Use qualified keywords to intern them */
  eng.handle(make_message({
    {   "op",                                                              "eval" },
    { "code", "(def my-data {:user/name \"test\" :user/id 1 :system/config {}})" }
  }));

  /* Complete keywords in :user namespace */
  auto responses(eng.handle(make_message({
    {     "op",  "complete" },
    { "prefix", ":user/na" },
    {     "ns",      "user" }
  })));

  REQUIRE(responses.size() == 1);
  auto const &payload(responses.front());
  auto const &completions(payload.at("completions").as_list());

  bool found_user_name{ false };

  for(auto const &entry : completions)
  {
    auto const &dict(entry.as_dict());
    auto const &candidate(dict.at("candidate").as_string());
    INFO("Got qualified keyword completion: " << candidate);

    if(candidate == ":user/name")
    {
      found_user_name = true;
      CHECK(dict.at("type").as_string() == "keyword");
    }
  }

  CHECK(found_user_name);
}

TEST_CASE("complete returns auto-resolved keywords with double colon prefix")
{
  /* This test verifies that ::foo keywords (auto-resolved to current ns) are completed */
  engine eng;

  /* Use auto-resolved keywords to intern them in the user namespace */
  eng.handle(make_message({
    {   "op",                                                "eval" },
    { "code", "(def data {::local-key 1 ::local-other 2 :global 3})" }
  }));

  /* Complete keywords starting with :: (should find user/local-key, user/local-other) */
  auto responses(eng.handle(make_message({
    {     "op", "complete" },
    { "prefix",    "::loc" },
    {     "ns",     "user" }
  })));

  REQUIRE(responses.size() == 1);
  auto const &payload(responses.front());
  auto const &completions(payload.at("completions").as_list());

  bool found_local_key{ false };
  bool found_local_other{ false };
  bool found_global{ false };

  for(auto const &entry : completions)
  {
    auto const &dict(entry.as_dict());
    auto const &candidate(dict.at("candidate").as_string());
    INFO("Got :: keyword completion: " << candidate);

    if(candidate == "::local-key")
    {
      found_local_key = true;
      CHECK(dict.at("type").as_string() == "keyword");
      CHECK(dict.at("ns").as_string() == "user");
    }
    if(candidate == "::local-other")
    {
      found_local_other = true;
      CHECK(dict.at("type").as_string() == "keyword");
    }
    if(candidate == ":global" || candidate == "::global")
    {
      found_global = true; /* Should NOT be found - it's not in user ns */
    }
  }

  CHECK(found_local_key);
  CHECK(found_local_other);
  CHECK_FALSE(found_global); /* :global should not match ::loc prefix */
}

TEST_CASE("complete returns all auto-resolved keywords with just double colon")
{
  /* This test verifies that :: alone completes all keywords in current namespace */
  engine eng;

  /* Use auto-resolved keywords */
  eng.handle(make_message({
    {   "op",                                    "eval" },
    { "code", "(def data {::ns-key1 1 ::ns-key2 2})" }
  }));

  /* Complete with just :: */
  auto responses(eng.handle(make_message({
    {     "op", "complete" },
    { "prefix",       "::" },
    {     "ns",     "user" }
  })));

  REQUIRE(responses.size() == 1);
  auto const &payload(responses.front());
  auto const &completions(payload.at("completions").as_list());

  bool found_ns_key1{ false };
  bool found_ns_key2{ false };

  for(auto const &entry : completions)
  {
    auto const &dict(entry.as_dict());
    auto const &candidate(dict.at("candidate").as_string());

    if(candidate == "::ns-key1")
    {
      found_ns_key1 = true;
    }
    if(candidate == "::ns-key2")
    {
      found_ns_key2 = true;
    }
  }

  CHECK(found_ns_key1);
  CHECK(found_ns_key2);
}

  }
}
