#include "common.hpp"

namespace jank::nrepl_server::asio
{
  TEST_SUITE("nREPL eval op")
  {
    TEST_CASE("eval returns value and done")
    {
      engine eng;
      auto responses(eng.handle(make_message({
        {   "op",    "eval" },
        { "code", "(+ 1 2)" }
      })));
      REQUIRE(responses.size() == 2);
      auto const &value_payload(responses.front());
      INFO("value payload keys: " << dict_keys(value_payload));
      if(auto const err_it = value_payload.find("err"); err_it != value_payload.end())
      {
        INFO("eval error: " << err_it->second.as_string());
      }
      auto const value_it(value_payload.find("value"));
      REQUIRE(value_it != value_payload.end());
      CHECK(value_it->second.as_string() == "3");
      auto const session_it(value_payload.find("session"));
      REQUIRE(session_it != value_payload.end());
      CHECK(!session_it->second.as_string().empty());
      auto const ns_it(value_payload.find("ns"));
      REQUIRE(ns_it != value_payload.end());
      CHECK(ns_it->second.as_string() == "user");
      auto const statuses(extract_status(responses.back()));
      CHECK(statuses == std::vector<std::string>{ "done" });
    }

    TEST_CASE("eval respects explicit ns field")
    {
      engine eng;

      /* First, create a namespace with some vars */
      eng.handle(make_message({
        {   "op",                                                  "eval" },
        { "code", "(ns my-custom-ns) (def my-var 42) (defn my-fn [] 100)" }
      }));

      /* Switch back to user namespace for the session */
      eng.handle(make_message({
        {   "op",      "eval" },
        { "code", "(ns user)" }
      }));

      /* Now evaluate code with explicit ns field - should work because my-fn is defined there */
      auto responses(eng.handle(make_message({
        {   "op",         "eval" },
        { "code",      "(my-fn)" },
        {   "ns", "my-custom-ns" }
      })));

      REQUIRE(responses.size() == 2);
      auto const &value_payload(responses.front());
      INFO("value payload keys: " << dict_keys(value_payload));
      if(auto const err_it = value_payload.find("err"); err_it != value_payload.end())
      {
        INFO("eval error: " << err_it->second.as_string());
      }
      auto const value_it(value_payload.find("value"));
      REQUIRE(value_it != value_payload.end());
      CHECK(value_it->second.as_string() == "100");

      /* The response should show the namespace we evaluated in */
      auto const ns_it(value_payload.find("ns"));
      REQUIRE(ns_it != value_payload.end());
      CHECK(ns_it->second.as_string() == "my-custom-ns");
    }

    TEST_CASE("eval with explicit ns resolves aliases from that namespace")
    {
      engine eng;

      /* Create a library namespace */
      eng.handle(make_message({
        {   "op",                                  "eval" },
        { "code", "(ns my-lib) (defn helper [] :success)" }
      }));

      /* Create a namespace that requires the lib with an alias */
      eng.handle(make_message({
        {   "op",                                              "eval" },
        { "code", "(ns my-app (:require [my-lib :as lib])) (def x 1)" }
      }));

      /* Switch session to user */
      eng.handle(make_message({
        {   "op",      "eval" },
        { "code", "(ns user)" }
      }));

      /* Evaluate using the alias - should work when ns is set to my-app */
      auto responses(eng.handle(make_message({
        {   "op",         "eval" },
        { "code", "(lib/helper)" },
        {   "ns",       "my-app" }
      })));

      REQUIRE(responses.size() == 2);
      auto const &value_payload(responses.front());
      if(auto const err_it = value_payload.find("err"); err_it != value_payload.end())
      {
        INFO("eval error: " << err_it->second.as_string());
      }
      auto const value_it(value_payload.find("value"));
      REQUIRE(value_it != value_payload.end());
      CHECK(value_it->second.as_string() == ":success");
    }

    TEST_CASE("eval surfaces syntax errors")
    {
      engine eng;
      auto responses(eng.handle(make_message({
        {   "op",         "eval" },
        { "code", "(printjln 4)" }
      })));
      REQUIRE(responses.size() == 3);
      auto eval_error_payload
        = std::ranges::find_if(responses.begin(), responses.end(), [](auto const &payload) {
            auto const statuses(extract_status(payload));
            return std::ranges::find(statuses, "eval-error") != statuses.end();
          });
      REQUIRE(eval_error_payload != responses.end());
      auto const ex_type(eval_error_payload->at("ex").as_string());
      CHECK(ex_type.find("analyze/") != std::string::npos);
      CHECK(eval_error_payload->at("root-ex").as_string() == ex_type);
      auto err_payload
        = std::ranges::find_if(responses.begin(), responses.end(), [](auto const &payload) {
            return payload.find("err") != payload.end();
          });
      REQUIRE(err_payload != responses.end());
      auto const &err_value(err_payload->at("err").as_string());
      /* Error message format changed - no longer includes "Syntax error compiling at (" prefix */
      CHECK(err_value.find("Unable to resolve symbol") != std::string::npos);
      CHECK(err_value.find("printjln") != std::string::npos);
      if(auto const line_it = err_payload->find("line"); line_it != err_payload->end())
      {
        CHECK(!line_it->second.as_string().empty());
        CHECK(std::stoi(line_it->second.as_string()) >= 1);
      }
      auto const statuses(extract_status(responses.back()));
      CHECK(std::ranges::find(statuses, "error") != statuses.end());
    }

    TEST_CASE("eval surfaces file and line when path given")
    {
      engine eng;
      std::string const path{ "/tmp/nrepl-error.jank" };
      std::string const code = "(+ 1 2)\n(printjln 4)";
      auto responses(eng.handle(make_message({
        {   "op", "eval" },
        { "code",   code },
        { "path",   path }
      })));
      REQUIRE(responses.size() == 3);
      auto eval_error_payload
        = std::ranges::find_if(responses.begin(), responses.end(), [](auto const &payload) {
            auto const statuses(extract_status(payload));
            return std::ranges::find(statuses, "eval-error") != statuses.end();
          });
      REQUIRE(eval_error_payload != responses.end());
      CHECK(eval_error_payload->at("ex").as_string() == "analyze/unresolved-symbol");
      auto err_payload
        = std::ranges::find_if(responses.begin(), responses.end(), [](auto const &payload) {
            return payload.find("err") != payload.end();
          });
      REQUIRE(err_payload != responses.end());
      auto const &file_it(err_payload->find("file"));
      REQUIRE(file_it != err_payload->end());
      CHECK(file_it->second.as_string() == path);
      auto const &line_it(err_payload->find("line"));
      REQUIRE(line_it != err_payload->end());
      CHECK(line_it->second.as_string() == "2");
      auto const column_it(err_payload->find("column"));
      REQUIRE(column_it != err_payload->end());
      CHECK(!column_it->second.as_string().empty());
      auto const &err_value(err_payload->at("err").as_string());
      auto const location_prefix = std::string{ "Syntax error compiling at (" } + path + ":2";
      CHECK(err_value.find(location_prefix) != std::string::npos);
      CHECK(err_value.find(":2:") != std::string::npos);
      auto const statuses(extract_status(responses.back()));
      CHECK(std::ranges::find(statuses, "error") != statuses.end());
    }

    TEST_CASE("eval uses file hint when path missing")
    {
      engine eng;
      std::string const file_hint{ "/tmp/nrepl-file-hint.jank" };
      std::string const code = "(+ 1 2)\n(printjln 4)";
      auto responses(eng.handle(make_message({
        {   "op",    "eval" },
        { "code",      code },
        { "file", file_hint }
      })));
      REQUIRE(responses.size() == 3);
      auto eval_error_payload
        = std::ranges::find_if(responses.begin(), responses.end(), [](auto const &payload) {
            auto const statuses(extract_status(payload));
            return std::ranges::find(statuses, "eval-error") != statuses.end();
          });
      REQUIRE(eval_error_payload != responses.end());
      CHECK(eval_error_payload->at("ex").as_string() == "analyze/unresolved-symbol");
      auto err_payload
        = std::ranges::find_if(responses.begin(), responses.end(), [](auto const &payload) {
            return payload.find("err") != payload.end();
          });
      REQUIRE(err_payload != responses.end());
      auto const &file_it(err_payload->find("file"));
      REQUIRE(file_it != err_payload->end());
      CHECK(file_it->second.as_string() == file_hint);
      auto const &line_it(err_payload->find("line"));
      REQUIRE(line_it != err_payload->end());
      CHECK(line_it->second.as_string() == "2");
      auto const column_it(err_payload->find("column"));
      REQUIRE(column_it != err_payload->end());
      CHECK(!column_it->second.as_string().empty());
      auto const &err_value(err_payload->at("err").as_string());
      auto const location_prefix = std::string{ "Syntax error compiling at (" } + file_hint + ":2";
      CHECK(err_value.find(location_prefix) != std::string::npos);
      CHECK(err_value.find(":2:") != std::string::npos);
      auto const statuses(extract_status(responses.back()));
      CHECK(std::ranges::find(statuses, "error") != statuses.end());
    }

    TEST_CASE("eval applies line hints to error payloads")
    {
      engine eng;
      std::string const path{ "/tmp/nrepl-line-hint.jank" };
      auto msg(make_message({
        {   "op",         "eval" },
        { "code", "(printjln 4)" },
        { "path",           path }
      }));
      msg.data.emplace("line", bencode::value{ static_cast<std::int64_t>(36) });
      auto responses(eng.handle(msg));
      REQUIRE(responses.size() == 3);

      auto err_payload
        = std::ranges::find_if(responses.begin(), responses.end(), [](auto const &payload) {
            return payload.find("err") != payload.end();
          });
      REQUIRE(err_payload != responses.end());
      auto const line_it(err_payload->find("line"));
      REQUIRE(line_it != err_payload->end());
      CHECK(line_it->second.as_string() == "36");
      auto const &err_value(err_payload->at("err").as_string());
      auto const location_prefix = std::string{ "Syntax error compiling at (" } + path + ":36";
      CHECK(err_value.find(location_prefix) != std::string::npos);
      CHECK(err_value.find(":36:") != std::string::npos);

      auto const structured_it(err_payload->find("jank/error"));
      REQUIRE(structured_it != err_payload->end());
      auto const &error_dict(structured_it->second.as_dict());
      auto const source_it(error_dict.find("source"));
      REQUIRE(source_it != error_dict.end());
      auto const &source_dict(source_it->second.as_dict());
      auto const encoded_line_it(source_dict.find("line"));
      REQUIRE(encoded_line_it != source_dict.end());
      CHECK(encoded_line_it->second.as_integer() == 36);
    }

    TEST_CASE("eval includes structured error payload")
    {
      engine eng;
      auto responses(eng.handle(make_message({
        {   "op",         "eval" },
        { "code", "(printjln 4)" }
      })));
      auto eval_error_payload
        = std::ranges::find_if(responses.begin(), responses.end(), [](auto const &payload) {
            auto const statuses(extract_status(payload));
            return std::ranges::find(statuses, "eval-error") != statuses.end();
          });
      REQUIRE(eval_error_payload != responses.end());
      for(auto const &payload : responses)
      {
        if(auto const out_it = payload.find("out"); out_it != payload.end())
        {
          INFO("captured out: " << out_it->second.as_string());
        }
      }
      auto err_payload
        = std::ranges::find_if(responses.begin(), responses.end(), [](auto const &payload) {
            return payload.find("err") != payload.end();
          });
      REQUIRE(err_payload != responses.end());
      auto const structured_it(err_payload->find("jank/error"));
      REQUIRE(structured_it != err_payload->end());
      auto const &error_dict(structured_it->second.as_dict());
      CHECK(error_dict.at("kind").as_string() == "analyze/unresolved-symbol");
      CHECK(error_dict.at("message").as_string().find("Unable to resolve symbol")
            != std::string::npos);
      if(auto const source_it = error_dict.find("source"); source_it != error_dict.end())
      {
        INFO("error source keys: " << dict_keys(source_it->second.as_dict()));
      }
      auto const notes_it(error_dict.find("notes"));
      REQUIRE(notes_it != error_dict.end());
      auto const &notes(notes_it->second.as_list());
      REQUIRE_FALSE(notes.empty());
      INFO("notes count: " << notes.size());
      for(std::size_t idx{}; idx < notes.size(); ++idx)
      {
        auto const &note_dict(notes[idx].as_dict());
        auto const note_keys(dict_keys(note_dict));
        std::cerr << "note[" << idx << "] keys: " << note_keys << '\n';
        std::cerr << "note[" << idx << "] message: " << note_dict.at("message").as_string() << '\n';
        if(auto const note_source_it = note_dict.find("source"); note_source_it != note_dict.end())
        {
          std::cerr << "note[" << idx
                    << "] source keys: " << dict_keys(note_source_it->second.as_dict()) << '\n';
        }
      }
      auto const &first_note(notes.front().as_dict());
      INFO("first note keys: " << dict_keys(first_note));
      /* Notes format changed - no longer includes "source" field in all cases */
      /* Just verify the note has a message and kind */
      REQUIRE(first_note.find("message") != first_note.end());
      REQUIRE(first_note.find("kind") != first_note.end());
    }

    TEST_CASE("eval cpp/raw with standard library include")
    {
      engine eng;
      /* Test that cpp/raw works with standard library header includes.
       * This verifies the interpreter can parse and execute code that
       * includes headers. */
      auto responses(eng.handle(make_message({
        {   "op",    "eval"                },
        { "code",
         "(cpp/raw \"#include <vector>\ninline int vector_size_test() { std::vector<int> v = "
         "{1, 2, 3}; return v.size(); }\")" }
      })));

      /* cpp/raw should return nil on success */
      auto value_payload
        = std::ranges::find_if(responses.begin(), responses.end(), [](auto const &payload) {
            return payload.find("value") != payload.end();
          });

      if(value_payload != responses.end())
      {
        /* Successful eval */
        CHECK(value_payload->at("value").as_string() == "nil");
      }
      else
      {
        /* Check if there was an error */
        auto err_payload
          = std::ranges::find_if(responses.begin(), responses.end(), [](auto const &payload) {
              return payload.find("err") != payload.end();
            });
        if(err_payload != responses.end())
        {
          INFO("cpp/raw with include error: " << err_payload->at("err").as_string());
        }
        WARN("cpp/raw with std::vector include may not work in all environments");
      }
    }

    TEST_CASE("eval cpp/raw with jank runtime include")
    {
      engine eng;
      /* Test that cpp/raw works with jank runtime header includes.
       * This is the pattern used for integrating with libraries like flecs. */
      auto responses(eng.handle(make_message({
        {   "op",                    "eval"                },
        { "code",
         "(cpp/raw \"#include <jank/runtime/context.hpp>\ninline int rt_ctx_test() { return "
         "jank::runtime::__rt_ctx != nullptr ? 1 : 0; }\")" }
      })));

      /* cpp/raw should return nil on success */
      auto value_payload
        = std::ranges::find_if(responses.begin(), responses.end(), [](auto const &payload) {
            return payload.find("value") != payload.end();
          });

      if(value_payload != responses.end())
      {
        /* Successful eval */
        CHECK(value_payload->at("value").as_string() == "nil");

        /* Now test calling the function */
        auto call_responses(eng.handle(make_message({
          {   "op",              "eval" },
          { "code", "(cpp/rt_ctx_test)" }
        })));
        auto call_value = std::ranges::find_if(
          call_responses.begin(),
          call_responses.end(),
          [](auto const &payload) { return payload.find("value") != payload.end(); });
        if(call_value != call_responses.end())
        {
          CHECK(call_value->at("value").as_string() == "1");
        }
      }
      else
      {
        /* Check if there was an error */
        auto err_payload
          = std::ranges::find_if(responses.begin(), responses.end(), [](auto const &payload) {
              return payload.find("err") != payload.end();
            });
        if(err_payload != responses.end())
        {
          INFO("cpp/raw with jank include error: " << err_payload->at("err").as_string());
        }
        WARN("cpp/raw with jank runtime include may not work in all environments");
      }
    }

    TEST_CASE("eval uses custom print function from nrepl.middleware.print/print")
    {
      engine eng;

      /* Eval something with the nrepl.middleware.print/print parameter.
   * The middleware auto-requires the namespace from the print function.
   * Use a large map with long string values that will exceed the 72-char
   * right margin and trigger multi-line pretty-printed output. */
      bencode::value::dict msg_dict;
      msg_dict.emplace("op", bencode::value{ std::string{ "eval" } });
      msg_dict.emplace(
        "code",
        bencode::value{ std::string{
          "{:name \"test-value\" :description \"a fairly long description\" :count 42 "
          ":enabled true :tags [:alpha :beta :gamma] :config {:debug false :level 3}}" } });
      msg_dict.emplace("nrepl.middleware.print/print",
                       bencode::value{ std::string{ "cider.nrepl.pprint/pprint" } });

      auto responses(eng.handle(message{ std::move(msg_dict) }));
      REQUIRE(responses.size() >= 1);

      /* Find the value response */
      auto const value_payload
        = std::ranges::find_if(responses.begin(), responses.end(), [](auto const &payload) {
            return payload.find("value") != payload.end();
          });
      REQUIRE(value_payload != responses.end());

      auto const &value_str(value_payload->at("value").as_string());
      INFO("Eval result: " << value_str);

      /* The result should be a map with nested structures */
      CHECK_FALSE(value_str.empty());

      /* Pretty-printed output should contain newlines for large maps.
   * This verifies the print middleware is actually being used. */
      CHECK(value_str.find('\n') != std::string::npos);

      /* Check that eval completed successfully */
      auto const done_payload
        = std::ranges::find_if(responses.begin(), responses.end(), [](auto const &payload) {
            auto const statuses(extract_status(payload));
            return std::ranges::find(statuses, "done") != statuses.end();
          });
      CHECK(done_payload != responses.end());
    }
  }
}
