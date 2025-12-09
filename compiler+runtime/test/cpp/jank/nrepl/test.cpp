#include "common.hpp"

namespace jank::nrepl_server::asio
{
  TEST_SUITE("nREPL test ops")
  {
TEST_CASE("test op runs clojure.test tests and returns results")
{
  engine eng;

  /* Define a test namespace with a passing test */
  eng.handle(make_message({
    {   "op",                                              "eval" },
    { "code", "(ns test-ns-for-nrepl (:require [clojure.test :refer [deftest is]]))" }
  }));

  /* Define a simple passing test */
  eng.handle(make_message({
    {   "op",                      "eval" },
    { "code", "(deftest my-passing-test (is (= 1 1)))" }
  }));

  /* Run the test op - construct message dict directly since tests is a list */
  bencode::value::dict msg_dict;
  msg_dict.emplace("op", "test");
  msg_dict.emplace("ns", "test-ns-for-nrepl");
  msg_dict.emplace("load?", "false");
  msg_dict.emplace("fail-fast", "false");

  bencode::value::list tests_list;
  tests_list.push_back(bencode::value{ std::string{ "my-passing-test" } });
  msg_dict.emplace("tests", bencode::value{ std::move(tests_list) });

  auto responses(eng.handle(message{ std::move(msg_dict) }));

  REQUIRE(responses.size() == 1);
  auto const &payload(responses.front());

  /* Check that we got results */
  auto const results_it(payload.find("results"));
  REQUIRE(results_it != payload.end());
  CHECK(results_it->second.is_dict());

  /* Check that we got a summary */
  auto const summary_it(payload.find("summary"));
  REQUIRE(summary_it != payload.end());
  CHECK(summary_it->second.is_dict());

  auto const &summary(summary_it->second.as_dict());
  auto const pass_it(summary.find("pass"));
  if(pass_it != summary.end())
  {
    /* Verify at least one pass (the test has one assertion) */
    CHECK(pass_it->second.as_integer() >= 1);
  }

  /* Check testing-ns is set */
  auto const testing_ns_it(payload.find("testing-ns"));
  REQUIRE(testing_ns_it != payload.end());
  CHECK(testing_ns_it->second.as_string() == "test-ns-for-nrepl");

  /* Check status is done */
  auto const status_it(payload.find("status"));
  REQUIRE(status_it != payload.end());
  CHECK(status_it->second.is_list());
}

TEST_CASE("test op handles failing tests")
{
  engine eng;

  /* Define a test namespace with a failing test */
  eng.handle(make_message({
    {   "op",                                              "eval" },
    { "code", "(ns test-ns-failing (:require [clojure.test :refer [deftest is]]))" }
  }));

  /* Define a failing test */
  eng.handle(make_message({
    {   "op",                      "eval" },
    { "code", "(deftest my-failing-test (is (= 1 2)))" }
  }));

  /* Run the test op - construct message dict directly since tests is a list */
  bencode::value::dict msg_dict;
  msg_dict.emplace("op", "test");
  msg_dict.emplace("ns", "test-ns-failing");
  msg_dict.emplace("load?", "false");
  msg_dict.emplace("fail-fast", "false");

  bencode::value::list tests_list;
  tests_list.push_back(bencode::value{ std::string{ "my-failing-test" } });
  msg_dict.emplace("tests", bencode::value{ std::move(tests_list) });

  auto responses(eng.handle(message{ std::move(msg_dict) }));

  REQUIRE(responses.size() == 1);
  auto const &payload(responses.front());

  /* Check that we got results with the failing test */
  auto const results_it(payload.find("results"));
  REQUIRE(results_it != payload.end());
  CHECK(results_it->second.is_dict());

  /* Check summary has failures */
  auto const summary_it(payload.find("summary"));
  REQUIRE(summary_it != payload.end());

  auto const &summary(summary_it->second.as_dict());
  auto const fail_it(summary.find("fail"));
  if(fail_it != summary.end())
  {
    CHECK(fail_it->second.as_integer() >= 1);
  }
}

TEST_CASE("test op returns correct actual/expected format for failing assertions")
{
  engine eng;

  /* Define a test namespace with a failing test that uses evaluated expressions */
  eng.handle(make_message({
    {   "op",                                              "eval" },
    { "code", "(ns test-ns-format (:require [clojure.test :refer [deftest is]]))" }
  }));

  /* Define a helper function and a failing test that compares evaluated values */
  eng.handle(make_message({
    {   "op",                      "eval" },
    { "code", "(defn get-value [] 42)" }
  }));

  eng.handle(make_message({
    {   "op",                      "eval" },
    { "code", "(deftest format-test (is (= 41 (get-value))))" }
  }));

  /* Run the test op */
  bencode::value::dict msg_dict;
  msg_dict.emplace("op", "test");
  msg_dict.emplace("ns", "test-ns-format");
  msg_dict.emplace("load?", "false");
  msg_dict.emplace("fail-fast", "false");

  bencode::value::list tests_list;
  tests_list.push_back(bencode::value{ std::string{ "format-test" } });
  msg_dict.emplace("tests", bencode::value{ std::move(tests_list) });

  auto responses(eng.handle(message{ std::move(msg_dict) }));

  REQUIRE(responses.size() == 1);
  auto const &payload(responses.front());

  /* Navigate to the test result */
  auto const results_it(payload.find("results"));
  REQUIRE(results_it != payload.end());
  REQUIRE(results_it->second.is_dict());

  auto const &ns_results(results_it->second.as_dict());
  auto const ns_it(ns_results.find("test-ns-format"));
  REQUIRE(ns_it != ns_results.end());
  REQUIRE(ns_it->second.is_dict());

  auto const &var_results(ns_it->second.as_dict());
  auto const var_it(var_results.find("format-test"));
  REQUIRE(var_it != var_results.end());
  REQUIRE(var_it->second.is_list());

  auto const &test_results_list(var_it->second.as_list());
  REQUIRE(!test_results_list.empty());

  /* Find the fail result */
  bool found_fail = false;
  for(auto const &result_entry : test_results_list)
  {
    REQUIRE(result_entry.is_dict());
    auto const &result(result_entry.as_dict());

    auto const type_it(result.find("type"));
    if(type_it != result.end() && type_it->second.as_string() == "fail")
    {
      found_fail = true;

      /* Check that "actual" contains the printed value "42\n", not the form "(not (= 41 42))" */
      auto const actual_it(result.find("actual"));
      REQUIRE(actual_it != result.end());
      std::string actual_str(actual_it->second.as_string());
      CHECK(actual_str.find("42") != std::string::npos);
      /* Should NOT contain the wrapped form */
      CHECK(actual_str.find("not") == std::string::npos);

      /* Check that "expected" contains the printed value "41\n", not the form "(= 41 (get-value))" */
      auto const expected_it(result.find("expected"));
      REQUIRE(expected_it != result.end());
      std::string expected_str(expected_it->second.as_string());
      CHECK(expected_str.find("41") != std::string::npos);
      /* Should NOT contain the function call */
      CHECK(expected_str.find("get-value") == std::string::npos);

      /* Check that "context" is the string "nil" for CIDER compatibility */
      auto const context_it(result.find("context"));
      REQUIRE(context_it != result.end());
      CHECK(context_it->second.is_string());
      CHECK(context_it->second.as_string() == "nil");

      /* Check that "file" is present */
      auto const file_it(result.find("file"));
      REQUIRE(file_it != result.end());
      CHECK(file_it->second.is_string());

      /* Check that "line" is present and is an integer */
      auto const line_it(result.find("line"));
      REQUIRE(line_it != result.end());
      CHECK(line_it->second.is_integer());

      break;
    }
  }

  CHECK(found_fail);
}

TEST_CASE("test op with nil tests runs all tests in namespace")
{
  engine eng;

  /* Define a test namespace with multiple tests */
  eng.handle(make_message({
    {   "op",                                              "eval" },
    { "code", "(ns test-all-ns (:require [clojure.test :refer [deftest is]]))" }
  }));

  eng.handle(make_message({
    {   "op",                      "eval" },
    { "code", "(deftest test-one (is (= 1 1)))" }
  }));

  eng.handle(make_message({
    {   "op",                      "eval" },
    { "code", "(deftest test-two (is (= 2 2)))" }
  }));

  /* Run test op without specifying tests (nil) - should run all */
  auto responses(eng.handle(make_message({
    {      "op",         "test" },
    {      "ns",  "test-all-ns" },
    {   "load?",        "false" }
  })));

  REQUIRE(responses.size() >= 1);
  auto const &payload(responses.front());

  /* Check summary shows 2 vars ran */
  auto const summary_it(payload.find("summary"));
  REQUIRE(summary_it != payload.end());

  auto const &summary(summary_it->second.as_dict());
  auto const var_it(summary.find("var"));
  REQUIRE(var_it != summary.end());
  CHECK(var_it->second.as_integer() == 2);

  /* Check pass count is at least 2 (one assertion per test) */
  auto const pass_it(summary.find("pass"));
  if(pass_it != summary.end())
  {
    CHECK(pass_it->second.as_integer() >= 2);
  }
}

TEST_CASE("deftest adds :test metadata for CIDER detection")
{
  engine eng;

  /* Define a test namespace and create a deftest */
  eng.handle(make_message({
    {   "op",                                              "eval" },
    { "code", "(ns test-metadata-ns (:require [clojure.test :refer [deftest is]]))" }
  }));

  eng.handle(make_message({
    {   "op",                      "eval" },
    { "code", "(deftest metadata-test (is true))" }
  }));

  /* Check that the var has :test metadata (required for CIDER to detect tests) */
  auto responses(eng.handle(make_message({
    {   "op",                      "eval" },
    { "code", "(contains? (meta (var metadata-test)) :test)" }
  })));

  REQUIRE(responses.size() >= 1);

  /* Find the value response */
  bool found_true{ false };
  for(auto const &resp : responses)
  {
    auto const value_it(resp.find("value"));
    if(value_it != resp.end())
    {
      CHECK(value_it->second.as_string() == "true");
      found_true = true;
    }
  }
  CHECK(found_true);
}

TEST_CASE("test-var-query runs all tests in namespace")
{
  engine eng;

  /* Define a test namespace with multiple tests */
  eng.handle(make_message({
    {   "op",                                              "eval" },
    { "code", "(ns test-var-query-ns (:require [clojure.test :refer [deftest is]]))" }
  }));

  eng.handle(make_message({
    {   "op",                      "eval" },
    { "code", "(deftest first-test (is (= 1 1)))" }
  }));

  eng.handle(make_message({
    {   "op",                      "eval" },
    { "code", "(deftest second-test (is (= 2 2)))" }
  }));

  /* Run test-var-query with ns-query to find all tests in namespace */
  bencode::value::dict msg_dict;
  msg_dict.emplace("op", "test-var-query");

  /* Build var-query with ns-query */
  bencode::value::dict var_query;
  bencode::value::dict ns_query;
  bencode::value::list ns_list;
  ns_list.push_back(bencode::value{ std::string{ "test-var-query-ns" } });
  ns_query.emplace("exactly", bencode::value{ std::move(ns_list) });
  var_query.emplace("ns-query", bencode::value{ std::move(ns_query) });
  msg_dict.emplace("var-query", bencode::value{ std::move(var_query) });

  auto responses(eng.handle(message{ std::move(msg_dict) }));

  REQUIRE(responses.size() >= 1);
  auto const &payload(responses.front());

  /* Check that we got results */
  auto const results_it(payload.find("results"));
  REQUIRE(results_it != payload.end());

  /* Check summary shows 2 tests ran */
  auto const summary_it(payload.find("summary"));
  REQUIRE(summary_it != payload.end());

  auto const &summary(summary_it->second.as_dict());
  auto const var_it(summary.find("var"));
  if(var_it != summary.end())
  {
    CHECK(var_it->second.as_integer() == 2);
  }
}

  }
}
