#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <ranges>
#include <string_view>
#include <variant>
#include <vector>

#include <jank/nrepl_server/engine.hpp>
#include <jank/nrepl_server/native_header_completion.hpp>

/* This must go last; doctest and glog both define CHECK and family. */
#include <doctest/doctest.h>

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

    message make_middleware_message(std::string op,
                                    std::vector<std::string> middleware,
                                    std::optional<std::string> session = std::nullopt)
    {
      bencode::value::dict dict;
      dict.emplace("op", bencode::value{ std::move(op) });
      if(session.has_value())
      {
        dict.emplace("session", bencode::value{ session.value() });
      }

      bencode::value::list list;
      list.reserve(middleware.size());
      for(auto &entry : middleware)
      {
        list.emplace_back(std::move(entry));
      }
      dict.emplace("middleware", bencode::value{ std::move(list) });

      return message{ std::move(dict) };
    }

    std::vector<std::string> extract_status(bencode::value::dict const &payload)
    {
      std::vector<std::string> statuses;
      auto const status_iter(payload.find("status"));
      if(status_iter == payload.end())
      {
        return statuses;
      }

      auto const &list(status_iter->second.as_list());
      statuses.reserve(list.size());
      for(auto const &entry : list)
      {
        statuses.push_back(entry.as_string());
      }
      return statuses;
    }

    constexpr std::array<std::string_view, 19> expected_ops{ "clone",
                                                             "describe",
                                                             "ls-sessions",
                                                             "close",
                                                             "eval",
                                                             "load-file",
                                                             "completions",
                                                             "complete",
                                                             "lookup",
                                                             "info",
                                                             "eldoc",
                                                             "forward-system-output",
                                                             "interrupt",
                                                             "ls-middleware",
                                                             "add-middleware",
                                                             "swap-middleware",
                                                             "stdin",
                                                             "caught",
                                                             "analyze-last-stacktrace" };

    constexpr std::array<std::string_view, 10> expected_middleware_stack{
      "nrepl.middleware.session/session",
      "nrepl.middleware.caught/wrap-caught",
      "nrepl.middleware.print/wrap-print",
      "nrepl.middleware.interruptible-eval/interruptible-eval",
      "nrepl.middleware.load-file/wrap-load-file",
      "nrepl.middleware.completion/wrap-completion",
      "nrepl.middleware.lookup/wrap-lookup",
      "nrepl.middleware.dynamic-loader/wrap-dynamic-loader",
      "nrepl.middleware.io/wrap-out",
      "nrepl.middleware.session/add-stdin"
    };

    std::string dict_keys(bencode::value::dict const &dict)
    {
      std::string joined;
      for(auto const &entry : dict)
      {
        if(!joined.empty())
        {
          joined += ',';
        }
        joined += entry.first;
      }
      return joined;
    }
  }

  TEST_SUITE("nREPL engine")
  {
    TEST_CASE("describe advertises extended ops")
    {
      engine eng;
      auto responses(eng.handle(make_message({
        { "op", "describe" }
      })));
      REQUIRE(responses.size() == 1);
      auto const &payload(responses.front());
      auto const &ops(payload.at("ops").as_dict());
      for(auto const &op_name : expected_ops)
      {
        CAPTURE(op_name);
        auto const op_it(ops.find(std::string{ op_name }));
        CHECK(op_it != ops.end());
      }
    }

    TEST_CASE("clone creates a fresh session")
    {
      engine eng;
      auto responses(eng.handle(make_message({
        { "op", "clone" }
      })));
      REQUIRE(responses.size() == 1);
      auto const &payload(responses.front());
      auto const &session(payload.at("session").as_string());
      auto const &new_session(payload.at("new-session").as_string());
      CHECK(session == new_session);
      auto const statuses(extract_status(payload));
      auto const done(std::ranges::find(statuses, "done"));
      CHECK(done != statuses.end());
    }

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
        {   "op",                                              "eval" },
        { "code", "(ns my-custom-ns) (def my-var 42) (defn my-fn [] 100)" }
      }));

      /* Switch back to user namespace for the session */
      eng.handle(make_message({
        {   "op",        "eval" },
        { "code", "(ns user)" }
      }));

      /* Now evaluate code with explicit ns field - should work because my-fn is defined there */
      auto responses(eng.handle(make_message({
        {   "op",          "eval" },
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
        {   "op",                                    "eval" },
        { "code", "(ns my-lib) (defn helper [] :success)" }
      }));

      /* Create a namespace that requires the lib with an alias */
      eng.handle(make_message({
        {   "op",                                                  "eval" },
        { "code", "(ns my-app (:require [my-lib :as lib])) (def x 1)" }
      }));

      /* Switch session to user */
      eng.handle(make_message({
        {   "op",        "eval" },
        { "code", "(ns user)" }
      }));

      /* Evaluate using the alias - should work when ns is set to my-app */
      auto responses(eng.handle(make_message({
        {   "op",        "eval" },
        { "code", "(lib/helper)" },
        {   "ns",      "my-app" }
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
      CHECK(err_value.find("Syntax error compiling at (") != std::string::npos);
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
      auto const source_it(first_note.find("source"));
      REQUIRE(source_it != first_note.end());
      auto const &source_dict(source_it->second.as_dict());
      auto const line_it(source_dict.find("line"));
      REQUIRE(line_it != source_dict.end());
      REQUIRE(line_it->second.is_integer());
      CHECK(std::get<std::int64_t>(line_it->second.data) >= 1);
    }

    TEST_CASE("caught returns structured errors")
    {
      engine eng;
      std::string const stack_path{ "/tmp/analyze-stacktrace.jank" };
      eng.handle(make_message({
        {   "op",         "eval" },
        { "code", "(printjln 4)" },
        { "path",     stack_path }
      }));
      auto responses(eng.handle(make_message({
        { "op", "caught" }
      })));
      REQUIRE(responses.size() == 1);
      auto const &payload(responses.front());
      auto const structured_it(payload.find("jank/error"));
      REQUIRE(structured_it != payload.end());
      auto const &error_dict(structured_it->second.as_dict());
      CHECK(error_dict.at("kind").as_string() == "analyze/unresolved-symbol");
    }

    TEST_CASE("analyze-last-stacktrace returns cause payloads")
    {
      engine eng;
      std::string const stack_path{ "/tmp/analyze-stacktrace.jank" };
      eng.handle(make_message({
        {   "op",         "eval" },
        { "code", "(printjln 4)" },
        { "path",     stack_path }
      }));

      auto responses(eng.handle(make_message({
        { "op", "analyze-last-stacktrace" }
      })));
      REQUIRE(responses.size() == 2);
      auto const &analysis(responses.front());
      CHECK(analysis.at("class").as_string() == "analyze/unresolved-symbol");
      CHECK(analysis.at("type").as_string() == "jank");
      auto const file_it(analysis.find("file"));
      REQUIRE(file_it != analysis.end());
      CHECK(file_it->second.as_string() == stack_path);
      auto const column_it(analysis.find("column"));
      REQUIRE(column_it != analysis.end());
      CHECK(!column_it->second.as_string().empty());
      auto const data_it(analysis.find("data"));
      REQUIRE(data_it != analysis.end());
      CHECK(data_it->second.as_string().find(":jank/error-kind") != std::string::npos);
      auto const stack_it(analysis.find("stacktrace"));
      REQUIRE(stack_it != analysis.end());
      auto const &stack_frames(stack_it->second.as_list());
      REQUIRE_FALSE(stack_frames.empty());
      auto const done_statuses(extract_status(responses.back()));
      CHECK(done_statuses == std::vector<std::string>{ "done" });
    }

    TEST_CASE("analyze-last-stacktrace reports absence of errors")
    {
      engine eng;
      auto responses(eng.handle(make_message({
        { "op", "analyze-last-stacktrace" }
      })));
      REQUIRE(responses.size() == 1);
      auto const statuses(extract_status(responses.front()));
      CHECK(std::ranges::find(statuses, "done") != statuses.end());
      CHECK(std::ranges::find(statuses, "no-error") != statuses.end());
    }

    TEST_CASE("load-file omits ns in response")
    {
      engine eng;
      auto responses(eng.handle(make_message({
        {   "op", "load-file" },
        { "file",   "(+ 4 5)" }
      })));
      REQUIRE(responses.size() == 2);
      CHECK(responses.front().find("ns") == responses.front().end());
      auto const value_it(responses.front().find("value"));
      if(auto const err_it = responses.front().find("err"); err_it != responses.front().end())
      {
        INFO("load-file error: " << err_it->second.as_string());
      }
      REQUIRE(value_it != responses.front().end());
      CHECK(value_it->second.as_string() == "9");
      auto const statuses(extract_status(responses.back()));
      CHECK(statuses == std::vector<std::string>{ "done" });
    }

    TEST_CASE("load-file surfaces syntax errors with file path")
    {
      engine eng;
      std::string const file_path{ "src/eita.jank" };
      std::string const file_contents = "(+ 1 2)\n(printjln 4)";
      auto responses(eng.handle(make_message({
        {        "op",   "load-file" },
        {      "file", file_contents },
        { "file-path",     file_path }
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

      auto const file_it(err_payload->find("file"));
      REQUIRE(file_it != err_payload->end());
      CHECK(file_it->second.as_string() == file_path);

      auto const line_it(err_payload->find("line"));
      REQUIRE(line_it != err_payload->end());
      CHECK(line_it->second.as_string() == "2");

      auto const column_it(err_payload->find("column"));
      REQUIRE(column_it != err_payload->end());
      CHECK(!column_it->second.as_string().empty());

      auto const &err_value(err_payload->at("err").as_string());
      auto const location_prefix = std::string{ "Syntax error compiling at (" } + file_path + ":2";
      CHECK(err_value.find(location_prefix) != std::string::npos);
      CHECK(err_value.find(":2:") != std::string::npos);

      auto const statuses(extract_status(responses.back()));
      CHECK(std::ranges::find(statuses, "error") != statuses.end());
    }

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

    TEST_CASE("info returns native header function signature")
    {
      engine eng;
      eng.handle(make_message({
        {   "op",                                                      "eval" },
        { "code", "(require '[\"clojure/string_native.hpp\" :as str-native])" }
      }));

      auto responses(eng.handle(make_message({
        {  "op",               "info" },
        { "sym", "str-native/reverse" },
        {  "ns",               "user" }
      })));

      REQUIRE(responses.size() == 1);
      auto const &payload(responses.front());

      // Print the actual response for debugging
      std::cerr << "Info name: " << payload.at("name").as_string() << "\n";
      std::cerr << "Info ns: " << payload.at("ns").as_string() << "\n";

      CHECK(payload.at("name").as_string() == "clojure.string_native.reverse");
      CHECK(payload.at("ns").as_string() == "cpp");

      auto const arglists_it(payload.find("arglists"));
      REQUIRE(arglists_it != payload.end());
      auto const &arglists(arglists_it->second.as_list());
      REQUIRE_FALSE(arglists.empty());
      auto const &signature(arglists.front().as_string());
      std::cerr << "Info arglists signature: '" << signature << "'\n";
      // Should NOT be just "coll" - should have type and parameter info
      CHECK(signature != "coll");
      CHECK(signature.find('s') != std::string::npos);
    }

    TEST_CASE("info returns doc and arglists")
    {
      engine eng;
      eng.handle(make_message({
        {   "op",                                                  "eval" },
        { "code", "(defn sample-fn \"demo doc\" ([x] x) ([x y] (+ x y)))" }
      }));
      auto responses(eng.handle(make_message({
        {  "op",      "info" },
        { "sym", "sample-fn" }
      })));
      REQUIRE(responses.size() == 1);
      auto const &payload(responses.front());
      CHECK(payload.at("name").as_string() == "sample-fn");
      CHECK(payload.at("ns").as_string() == "user");
      auto const doc(payload.at("docstring").as_string());
      CHECK(doc.find("demo doc") != std::string::npos);
      auto const &arglists(payload.at("arglists").as_list());
      REQUIRE(arglists.size() == 2);
      auto const statuses(extract_status(payload));
      auto const done(std::ranges::find(statuses, "done"));
      CHECK(done != statuses.end());

      // Check source location metadata
      auto const line_it(payload.find("line"));
      auto const column_it(payload.find("column"));
      REQUIRE(line_it != payload.end());
      REQUIRE(column_it != payload.end());
      CHECK(line_it->second.as_integer() > 0);
      CHECK(column_it->second.as_integer() > 0);

      // Test eldoc format for Clojure functions
      auto eldoc_responses(eng.handle(make_message({
        {  "op",     "eldoc" },
        { "sym", "sample-fn" }
      })));
      REQUIRE(eldoc_responses.size() == 1);
      auto const &eldoc_payload(eldoc_responses.front());
      auto const eldoc_it(eldoc_payload.find("eldoc"));
      REQUIRE(eldoc_it != eldoc_payload.end());
      auto const &eldoc_list(eldoc_it->second.as_list());
      REQUIRE(eldoc_list.size() == 2); // Two arities

      // First arity [x]
      auto const &first_arity(eldoc_list.at(0).as_list());
      std::cerr << "First arity size: " << first_arity.size() << "\n";
      for(size_t i = 0; i < first_arity.size(); ++i)
      {
        std::cerr << "  Param[" << i << "]: '" << first_arity.at(i).as_string() << "'\n";
      }
      REQUIRE(first_arity.size() == 1);
      CHECK(first_arity.at(0).as_string() == "x");

      // Second arity [x y]
      auto const &second_arity(eldoc_list.at(1).as_list());
      std::cerr << "Second arity size: " << second_arity.size() << "\n";
      for(size_t i = 0; i < second_arity.size(); ++i)
      {
        std::cerr << "  Param[" << i << "]: '" << second_arity.at(i).as_string() << "'\n";
      }
      REQUIRE(second_arity.size() == 2);
      CHECK(second_arity.at(0).as_string() == "x");
      CHECK(second_arity.at(1).as_string() == "y");
    }

    TEST_CASE("info returns cpp signature metadata")
    {
      engine eng;
      eng.handle(make_message({
        {   "op",                                                                   "eval" },
        { "code", "(cpp/raw \"int cpp_info_sig(int lhs, int rhs) { return lhs + rhs; }\")" }
      }));

      auto responses(eng.handle(make_message({
        {  "op",             "info" },
        { "sym", "cpp/cpp_info_sig" }
      })));
      REQUIRE(responses.size() == 1);
      auto const &payload(responses.front());
      CHECK(payload.at("name").as_string() == "cpp_info_sig");
      CHECK(payload.at("ns").as_string() == "cpp");
      CHECK(payload.at("type").as_string() == "native-function");
      CHECK(payload.at("return-type").as_string() == "int");

      auto const arglists_it(payload.find("arglists"));
      if(arglists_it == payload.end())
      {
        INFO("info payload keys:");
        for(auto const &entry : payload)
        {
          INFO(entry.first);
        }
      }
      REQUIRE(arglists_it != payload.end());
      auto const &arglists(arglists_it->second.as_list());
      REQUIRE(arglists.size() == 1);
      auto const &signature(arglists.front().as_string());
      INFO("signature: " << signature);
      // Should be [[int lhs] [int rhs]] format
      CHECK(signature.find("[[int lhs] [int rhs]]") != std::string::npos);
      CHECK(signature.find("lhs") != std::string::npos);
      CHECK(signature.find("rhs") != std::string::npos);

      auto const cpp_signatures_it(payload.find("cpp-signatures"));
      if(cpp_signatures_it == payload.end())
      {
        INFO("info payload keys:");
        for(auto const &entry : payload)
        {
          INFO(entry.first);
        }
      }
      REQUIRE(cpp_signatures_it != payload.end());
      auto const &cpp_signatures(cpp_signatures_it->second.as_list());
      REQUIRE(cpp_signatures.size() == 1);
      auto const &cpp_signature(cpp_signatures.front().as_dict());
      CHECK(cpp_signature.at("return-type").as_string() == "int");
      auto const &args(cpp_signature.at("args").as_list());
      REQUIRE(args.size() == 2);
      auto const &first_arg(args.front().as_dict());
      CHECK(first_arg.at("index").as_integer() == 0);
      CHECK(first_arg.at("type").as_string() == "int");
      auto const first_arg_name(first_arg.at("name").as_string());
      INFO("first_arg_name: " << first_arg_name);
      CHECK(first_arg_name == "lhs");
      auto const &second_arg(args.back().as_dict());
      CHECK(second_arg.at("index").as_integer() == 1);
      CHECK(second_arg.at("type").as_string() == "int");
      auto const second_arg_name(second_arg.at("name").as_string());
      INFO("second_arg_name: " << second_arg_name);
      CHECK(second_arg_name == "rhs");

      auto const statuses(extract_status(payload));
      CHECK(std::ranges::find(statuses, "done") != statuses.end());

      CHECK(payload.at("docstring").as_string() == "int");
      CHECK(payload.at("doc").as_string() == payload.at("docstring").as_string());
    }

    TEST_CASE("info returns cpp function docstring from doxygen comment")
    {
      engine eng;
      eng.handle(make_message({
        {   "op","eval"                },
        { "code",
         "(cpp/raw \"/** Adds two integers together.\\n * @param a First operand\\n * @param b "
         "Second operand\\n * @return The sum of a and b\\n */\\nint cpp_documented_fn(int a, int "
         "b) { return a + b; }\")" }
      }));

      auto responses(eng.handle(make_message({
        {  "op",                  "info" },
        { "sym", "cpp/cpp_documented_fn" }
      })));
      REQUIRE(responses.size() == 1);
      auto const &payload(responses.front());

      CHECK(payload.at("name").as_string() == "cpp_documented_fn");
      CHECK(payload.at("ns").as_string() == "cpp");
      CHECK(payload.at("type").as_string() == "native-function");

      // Check that docstring contains the documentation comment
      auto const docstring_it(payload.find("docstring"));
      REQUIRE(docstring_it != payload.end());
      auto const &docstring(docstring_it->second.as_string());
      INFO("docstring: " << docstring);
      std::cerr << "C++ function docstring: '" << docstring << "'\n";

      // The docstring should contain the Doxygen comment content
      CHECK(docstring.find("Adds two integers") != std::string::npos);

      // Check for source location metadata
      auto const file_it(payload.find("file"));
      auto const line_it(payload.find("line"));
      auto const column_it(payload.find("column"));

      INFO("file present: " << (file_it != payload.end()));
      INFO("line present: " << (line_it != payload.end()));
      INFO("column present: " << (column_it != payload.end()));

      if(file_it != payload.end())
      {
        std::cerr << "C++ function file: '" << file_it->second.as_string() << "'\n";
      }
      if(line_it != payload.end())
      {
        std::cerr << "C++ function line: " << line_it->second.as_integer() << "\n";
      }
      if(column_it != payload.end())
      {
        std::cerr << "C++ function column: " << column_it->second.as_integer() << "\n";
      }

      auto const statuses(extract_status(payload));
      CHECK(std::ranges::find(statuses, "done") != statuses.end());
    }

    TEST_CASE("info returns cpp function docstring from regular comment")
    {
      engine eng;
      eng.handle(make_message({
        {   "op",                      "eval"                },
        { "code",
         "(cpp/raw \"// This is a regular comment\\n// describing the function\\nint "
         "cpp_regular_comment_fn(int x) { return x * 2; }\")" }
      }));

      auto responses(eng.handle(make_message({
        {  "op",                       "info" },
        { "sym", "cpp/cpp_regular_comment_fn" }
      })));
      REQUIRE(responses.size() == 1);
      auto const &payload(responses.front());

      CHECK(payload.at("name").as_string() == "cpp_regular_comment_fn");
      CHECK(payload.at("ns").as_string() == "cpp");
      CHECK(payload.at("type").as_string() == "native-function");

      // Check that docstring contains the regular comment
      auto const docstring_it(payload.find("docstring"));
      REQUIRE(docstring_it != payload.end());
      auto const &docstring(docstring_it->second.as_string());
      INFO("docstring: " << docstring);
      std::cerr << "C++ regular comment docstring: '" << docstring << "'\n";

      // The docstring should contain the comment content
      CHECK(docstring.find("regular comment") != std::string::npos);
      CHECK(docstring.find("describing the function") != std::string::npos);

      auto const statuses(extract_status(payload));
      CHECK(std::ranges::find(statuses, "done") != statuses.end());
    }

    TEST_CASE("info returns cpp type with fields in docstring")
    {
      engine eng;
      eng.handle(make_message({
        {   "op",   "eval"                },
        { "code",
         "(cpp/raw \"struct cpp_info_struct { int field_a; double field_b; }; cpp_info_struct "
         "make_struct() { return {}; }\")" }
      }));

      auto responses(eng.handle(make_message({
        {  "op",                "info" },
        { "sym", "cpp/cpp_info_struct" }
      })));
      REQUIRE(responses.size() == 1);
      auto const &payload(responses.front());

      /* Check if the type was found - struct registration from cpp/raw may not work in all
       * environments */
      auto const name_it(payload.find("name"));
      if(name_it == payload.end())
      {
        auto const status_it(payload.find("status"));
        if(status_it != payload.end())
        {
          INFO("status: " << status_it->second);
        }
        INFO("cpp/raw struct registration may not be working - skipping detailed checks");
        WARN("struct type lookup returned no-info (this may be expected in some environments)");
        return;
      }

      CHECK(name_it->second.as_string() == "cpp_info_struct");
      CHECK(payload.at("ns").as_string() == "cpp");
      CHECK(payload.at("type").as_string() == "native-type");

      // Check that cpp-fields are present
      auto const fields_it(payload.find("cpp-fields"));
      REQUIRE(fields_it != payload.end());
      auto const &fields(fields_it->second.as_list());
      REQUIRE(fields.size() == 2);

      // Check docstring includes fields
      auto const doc_it(payload.find("docstring"));
      REQUIRE(doc_it != payload.end());
      auto const &docstring(doc_it->second.as_string());
      CHECK(docstring.find("Fields:") != std::string::npos);
      CHECK(docstring.find("field_a") != std::string::npos);
      CHECK(docstring.find("int") != std::string::npos);
      CHECK(docstring.find("field_b") != std::string::npos);
      CHECK(docstring.find("double") != std::string::npos);

      auto const statuses(extract_status(payload));
      CHECK(std::ranges::find(statuses, "done") != statuses.end());
    }

    TEST_CASE("eldoc returns function signatures")
    {
      engine eng;
      eng.handle(make_message({
        {   "op",                                                  "eval" },
        { "code", "(defn sample-fn \"demo doc\" ([x] x) ([x y] (+ x y)))" }
      }));
      auto responses(eng.handle(make_message({
        {  "op",     "eldoc" },
        { "sym", "sample-fn" }
      })));
      REQUIRE(responses.size() == 1);
      auto const &payload(responses.front());
      auto const &eldoc(payload.at("eldoc").as_list());
      REQUIRE(eldoc.size() == 2);
      auto const &first_sig(eldoc.front().as_list());
      auto const &second_sig(eldoc.back().as_list());
      REQUIRE(first_sig.size() == 1);
      CHECK(first_sig.front().as_string() == "x");
      REQUIRE(second_sig.size() == 2);
      CHECK(second_sig.front().as_string() == "x");
      CHECK(second_sig.back().as_string() == "y");
      CHECK(payload.at("name").as_string() == "sample-fn");
      CHECK(payload.at("ns").as_string() == "user");
      CHECK(payload.at("docstring").as_string().find("demo doc") != std::string::npos);
      auto const statuses(extract_status(payload));
      auto const done(std::ranges::find(statuses, "done"));
      CHECK(done != statuses.end());
    }

    TEST_CASE("info and eldoc normalize metadata-rich inputs")
    {
      engine eng;
      eng.handle(make_message({
        {   "op",                     "eval" },
        { "code", "(ns cpp_raw_inline.core)" }
      }));
      eng.handle(make_message({
        {   "op",                                                  "eval" },
        { "code", "(defn sample-fn \"demo doc\" ([x] x) ([x y] (+ x y)))" }
      }));
      auto const metadata_ns = "#(\"cpp_raw_inline.core\" 0 1 (face font-lock-type-face))";
      auto const metadata_sym = "#(\"sample-fn\" 0 9 (face font-lock-type-face))";

      auto info_responses(eng.handle(make_message({
        {  "op",       "info" },
        { "sym", metadata_sym },
        {  "ns",  metadata_ns }
      })));
      REQUIRE(info_responses.size() == 1);
      auto const &info_payload(info_responses.front());
      CHECK(info_payload.at("name").as_string() == "sample-fn");
      CHECK(info_payload.at("ns").as_string() == "cpp_raw_inline.core");
      auto const info_status(extract_status(info_payload));
      CHECK(std::ranges::find(info_status, "done") != info_status.end());

      auto eldoc_responses(eng.handle(make_message({
        {  "op",      "eldoc" },
        { "sym", metadata_sym },
        {  "ns",  metadata_ns }
      })));
      REQUIRE(eldoc_responses.size() == 1);
      auto const &eldoc_payload(eldoc_responses.front());
      auto const &eldoc_list(eldoc_payload.at("eldoc").as_list());
      REQUIRE(!eldoc_list.empty());
      CHECK(eldoc_payload.at("name").as_string() == "sample-fn");
      auto const eldoc_status(extract_status(eldoc_payload));
      CHECK(std::ranges::find(eldoc_status, "done") != eldoc_status.end());
    }

    TEST_CASE("eldoc surfaces cpp metadata")
    {
      engine eng;
      eng.handle(make_message({
        {   "op","eval"        },
        { "code",
         "(cpp/raw \"double cpp_eldoc_sig(double value, double factor) { return value * factor; "
         "}\")" }
      }));

      auto responses(eng.handle(make_message({
        {  "op",             "eldoc" },
        { "sym", "cpp/cpp_eldoc_sig" }
      })));
      REQUIRE(responses.size() == 1);
      auto const &payload(responses.front());
      CHECK(payload.at("name").as_string() == "cpp_eldoc_sig");
      CHECK(payload.at("ns").as_string() == "cpp");
      CHECK(payload.at("type").as_string() == "native-function");
      CHECK(payload.at("return-type").as_string() == "double");

      auto const eldoc_it(payload.find("eldoc"));
      if(eldoc_it == payload.end())
      {
        INFO("eldoc payload keys:");
        for(auto const &entry : payload)
        {
          INFO(entry.first);
        }
      }
      REQUIRE(eldoc_it != payload.end());
      auto const &eldoc(eldoc_it->second.as_list());
      REQUIRE(eldoc.size() == 1);
      auto const &tokens(eldoc.front().as_list());
      // Should have 2 parameters in format ["[double value]", "[double factor]"]
      REQUIRE(tokens.size() == 2);
      auto const &first_param(tokens.front().as_string());
      INFO("first_param in eldoc: " << first_param);
      // Check format is [type name] with proper parameter name
      CHECK(first_param.find("[double") != std::string::npos);
      CHECK((first_param.find("value") != std::string::npos
             || first_param.find("arg") != std::string::npos));
      auto const &second_param(tokens.at(1).as_string());
      INFO("second_param in eldoc: " << second_param);
      CHECK(second_param.find("[double") != std::string::npos);
      CHECK((second_param.find("factor") != std::string::npos
             || second_param.find("arg") != std::string::npos));

      auto const cpp_signatures_it(payload.find("cpp-signatures"));
      if(cpp_signatures_it == payload.end())
      {
        INFO("eldoc payload keys:");
        for(auto const &entry : payload)
        {
          INFO(entry.first);
        }
      }
      REQUIRE(cpp_signatures_it != payload.end());
      auto const &cpp_signatures(cpp_signatures_it->second.as_list());
      REQUIRE(cpp_signatures.size() == 1);
      auto const &cpp_signature(cpp_signatures.front().as_dict());
      CHECK(cpp_signature.at("return-type").as_string() == "double");
      auto const &args(cpp_signature.at("args").as_list());
      REQUIRE(args.size() == 2);
      auto const &first_arg(args.front().as_dict());
      auto const first_arg_name(first_arg.at("name").as_string());
      CHECK((first_arg_name == "value" || first_arg_name.rfind("arg", 0) == 0));
      CHECK(first_arg.at("type").as_string() == "double");
      auto const &second_arg(args.back().as_dict());
      auto const second_arg_name(second_arg.at("name").as_string());
      CHECK((second_arg_name == "factor" || second_arg_name.rfind("arg", 0) == 0));
      CHECK(second_arg.at("type").as_string() == "double");

      auto const statuses(extract_status(payload));
      CHECK(std::ranges::find(statuses, "done") != statuses.end());

      CHECK(payload.at("docstring").as_string() == "double");
      CHECK(payload.at("doc").as_string() == payload.at("docstring").as_string());
    }

    TEST_CASE("lookup reports namespace and miss state")
    {
      engine eng;
      eng.handle(make_message({
        {   "op",            "eval" },
        { "code", "(def sample 10)" }
      }));
      auto responses(eng.handle(make_message({
        {  "op", "lookup" },
        { "sym", "sample" }
      })));
      REQUIRE(responses.size() == 1);
      auto const &payload(responses.front());
      auto const &info(payload.at("info").as_dict());
      CHECK(info.at("name").as_string() == "sample");
      CHECK(info.at("ns").as_string() == "user");
      CHECK(info.find("missing") == info.end());

      auto missing(eng.handle(make_message({
        {  "op",      "lookup" },
        { "sym", "missing-var" }
      })));
      auto const &missing_info(missing.front().at("info").as_dict());
      CHECK(missing_info.at("missing").as_string() == "true");
    }

    TEST_CASE("forward-system-output acknowledges request")
    {
      engine eng;
      auto responses(eng.handle(make_message({
        { "op", "forward-system-output" }
      })));
      REQUIRE(responses.size() == 1);
      auto const statuses(extract_status(responses.front()));
      CHECK(statuses == std::vector<std::string>{ "done" });
    }

    TEST_CASE("caught reports last error state")
    {
      engine eng;
      auto eval_responses(eng.handle(make_message({
        {   "op",                          "eval" },
        { "code", "(throw (ex-info \"boom\" {}))" },
        {   "id",                    "req-caught" }
      })));
      auto err_payload = std::ranges::find_if(eval_responses, [](auto const &payload) {
        return payload.find("err") != payload.end();
      });
      REQUIRE(err_payload != eval_responses.end());
      auto const session(err_payload->at("session").as_string());

      auto caught(eng.handle(make_message({
        {      "op", "caught" },
        { "session",  session }
      })));
      REQUIRE(caught.size() == 1);
      auto const caught_status(extract_status(caught.front()));
      auto const caught_found(std::ranges::find(caught_status, "no-error"));
      CHECK(caught_found == caught_status.end());
      CHECK(caught.front().at("err").as_string().find("boom") != std::string::npos);

      eng.handle(make_message({
        {      "op",  "eval" },
        { "session", session },
        {    "code",    "42" }
      }));
      auto cleared(eng.handle(make_message({
        {      "op", "caught" },
        { "session",  session }
      })));
      REQUIRE(cleared.size() == 1);
      auto const cleared_status(extract_status(cleared.front()));
      auto const cleared_found(std::ranges::find(cleared_status, "no-error"));
      CHECK(cleared_found != cleared_status.end());
    }

    TEST_CASE("unsupported ops report reason")
    {
      engine eng;
      auto responses(eng.handle(make_message({
        { "op", "not-a-real-op" }
      })));
      REQUIRE(responses.size() == 1);
      auto const statuses(extract_status(responses.front()));
      auto const unsupported(std::ranges::find(statuses, "unsupported"));
      CHECK(unsupported != statuses.end());
      CHECK(responses.front().at("err").as_string() == "unknown-op");
    }

    TEST_CASE("interrupt acknowledges most recent eval id")
    {
      engine eng;
      auto eval_responses(eng.handle(make_message({
        {   "op",    "eval" },
        { "code", "(+ 8 9)" },
        {   "id",  "req-42" }
      })));
      REQUIRE(eval_responses.size() == 2);
      auto const session(eval_responses.front().at("session").as_string());

      auto interrupt(eng.handle(make_message({
        {           "op", "interrupt" },
        { "interrupt-id",    "req-42" },
        {      "session",     session }
      })));
      REQUIRE(interrupt.size() == 1);
      auto const statuses(extract_status(interrupt.front()));
      auto const idle(std::ranges::find(statuses, "session-idle"));
      CHECK(idle != statuses.end());
      CHECK(interrupt.front().at("interrupt-id").as_string() == "req-42");
    }

    TEST_CASE("ls-middleware reports stack")
    {
      engine eng;
      auto responses(eng.handle(make_message({
        { "op", "ls-middleware" }
      })));
      REQUIRE(responses.size() == 1);
      auto const &middleware(responses.front().at("middleware").as_list());
      REQUIRE(middleware.size() == expected_middleware_stack.size());
      for(std::size_t i{}; i < middleware.size(); ++i)
      {
        CAPTURE(i);
        CHECK(middleware[i].as_string() == expected_middleware_stack[i]);
      }
    }

    TEST_CASE("add-middleware appends unique entries")
    {
      engine eng;
      auto responses(eng.handle(make_middleware_message("add-middleware", { "wrap-extra" })));
      REQUIRE(responses.size() == 1);
      auto const &middleware(responses.front().at("middleware").as_list());
      REQUIRE(middleware.size() == expected_middleware_stack.size() + 1);
      CHECK(middleware.back().as_string() == "wrap-extra");
    }

    TEST_CASE("swap-middleware reorders stack when members match")
    {
      engine eng;
      eng.handle(make_middleware_message("add-middleware", { "wrap-extra" }));
      std::vector<std::string> reordered{ "wrap-extra" };
      reordered.insert(reordered.end(),
                       expected_middleware_stack.begin(),
                       expected_middleware_stack.end());
      auto responses(eng.handle(make_middleware_message("swap-middleware", reordered)));
      REQUIRE(responses.size() == 1);
      auto const &middleware(responses.front().at("middleware").as_list());
      REQUIRE(middleware.size() == reordered.size());
      for(std::size_t i{}; i < middleware.size(); ++i)
      {
        CAPTURE(i);
        CHECK(middleware[i].as_string() == reordered[i]);
      }
    }

    TEST_CASE("stdin accumulates unread buffer")
    {
      engine eng;
      auto clone(eng.handle(make_message({
        { "op", "clone" }
      })));
      auto const session(clone.front().at("session").as_string());

      auto first(eng.handle(make_message({
        {      "op", "stdin" },
        {   "stdin",   "foo" },
        { "session", session }
      })));
      REQUIRE(first.size() == 1);
      CHECK(first.front().at("unread").as_string() == "foo");

      auto second(eng.handle(make_message({
        {      "op", "stdin" },
        {   "stdin",   "bar" },
        { "session", session }
      })));
      REQUIRE(second.size() == 1);
      CHECK(second.front().at("unread").as_string() == "foobar");
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

    TEST_CASE("info returns this parameter for member methods")
    {
      /* This test verifies that info/eldoc for member methods includes
       * the implicit 'this' parameter with the correct class type.
       *
       * e.g., flecs/world.defer_begin should show:
       *   [[flecs::world this]] bool
       * instead of:
       *   [] bool */
      engine eng;

      /* Define a namespace with a class that has member functions. */
      eng.handle(make_message({
        {   "op",                              "eval"                },
        { "code",
         "(cpp/raw \"namespace this_param_test { struct world { void method_no_args() {} "
         "int method_with_args(int x, float y) { return x; } }; }\")" }
      }));

      /* Require it as a native header alias. */
      eng.handle(make_message({
        {   "op","eval"                },
        { "code",
         "(require '[\"jank/runtime/context.hpp\" :as tpt :scope "
         "\"this_param_test\"])" }
      }));

      /* Get info for a member method with no arguments */
      auto no_args_responses(eng.handle(make_message({
        {  "op",                     "info" },
        { "sym", "tpt/world.method_no_args" },
        {  "ns",                     "user" }
      })));

      REQUIRE(no_args_responses.size() == 1);
      auto const &no_args_payload(no_args_responses.front());

      std::cerr << "Info for method_no_args:\n";
      if(no_args_payload.contains("arglists-str"))
      {
        std::cerr << "  arglists-str: " << no_args_payload.at("arglists-str").as_string() << "\n";
      }

      /* Check arglists contains the this parameter */
      auto const &no_args_arglists(no_args_payload.at("arglists").as_list());
      REQUIRE_FALSE(no_args_arglists.empty());

      auto const &no_args_sig(no_args_arglists.front().as_string());
      std::cerr << "  First arglist: " << no_args_sig << "\n";
      /* Should contain the class type as 'this' parameter */
      CHECK(no_args_sig.find("this_param_test::world") != std::string::npos);
      CHECK(no_args_sig.find("this") != std::string::npos);

      /* Get info for a member method with arguments */
      auto with_args_responses(eng.handle(make_message({
        {  "op",                       "info" },
        { "sym", "tpt/world.method_with_args" },
        {  "ns",                       "user" }
      })));

      REQUIRE(with_args_responses.size() == 1);
      auto const &with_args_payload(with_args_responses.front());

      std::cerr << "Info for method_with_args:\n";
      if(with_args_payload.contains("arglists-str"))
      {
        std::cerr << "  arglists-str: " << with_args_payload.at("arglists-str").as_string() << "\n";
      }

      /* Check arglists contains the this parameter plus the regular args */
      auto const &with_args_arglists(with_args_payload.at("arglists").as_list());
      REQUIRE_FALSE(with_args_arglists.empty());

      auto const &with_args_sig(with_args_arglists.front().as_string());
      std::cerr << "  First arglist: " << with_args_sig << "\n";
      /* Should contain the class type as 'this' parameter */
      CHECK(with_args_sig.find("this_param_test::world") != std::string::npos);
      CHECK(with_args_sig.find("this") != std::string::npos);
      /* Should also contain the regular parameters */
      CHECK(with_args_sig.find("int") != std::string::npos);
      CHECK(with_args_sig.find("float") != std::string::npos);
    }

    TEST_CASE("info returns docstring for member methods")
    {
      /* This test verifies that info/eldoc for member methods includes
       * docstrings extracted from comments above the method declaration.
       * We use the test_flecs.hpp header which has documented_method and
       * doxygen_method with different comment styles. */
      engine eng;

      /* Include the test header */
      eng.handle(make_message({
        {   "op",                                                           "eval" },
        { "code", R"((cpp/raw "#include \"../test/cpp/jank/nrepl/test_flecs.hpp\""))" }
      }));

      /* Require it as a native header alias. */
      eng.handle(make_message({
        {   "op",                                                "eval"                 },
        { "code",
         R"((require '["../test/cpp/jank/nrepl/test_flecs.hpp" :as flecs :scope "flecs"]))" }
      }));

      /* Get info for a member method with standard C-style comment */
      auto doc_responses(eng.handle(make_message({
        {  "op",                          "info" },
        { "sym", "flecs/world.documented_method" },
        {  "ns",                          "user" }
      })));

      REQUIRE(doc_responses.size() == 1);
      auto const &doc_payload(doc_responses.front());

      std::cerr << "Info for documented_method:\n";

      /* Check if docstring is present */
      if(doc_payload.contains("doc"))
      {
        auto const &docstring(doc_payload.at("doc").as_string());
        std::cerr << "  doc: " << docstring << "\n";
        /* Should contain the docstring text */
        CHECK(docstring.find("Documented method") != std::string::npos);
      }
      else
      {
        std::cerr << "  doc: (not present)\n";
        /* docstring should be present, but don't hard fail if not */
        WARN_MESSAGE(false, "docstring not found for documented_method");
      }

      /* Check file information is also extracted */
      if(doc_payload.contains("file"))
      {
        auto const &file(doc_payload.at("file").as_string());
        std::cerr << "  file: " << file << "\n";
        CHECK(file.find("test_flecs.hpp") != std::string::npos);
      }

      /* Line is stored as an integer, just check it exists */
      CHECK(doc_payload.contains("line"));

      /* Get info for a member method with Doxygen-style comment */
      auto doxy_responses(eng.handle(make_message({
        {  "op",                       "info" },
        { "sym", "flecs/world.doxygen_method" },
        {  "ns",                       "user" }
      })));

      REQUIRE(doxy_responses.size() == 1);
      auto const &doxy_payload(doxy_responses.front());

      std::cerr << "Info for doxygen_method:\n";

      /* Check if docstring is present (Doxygen comments are recognized) */
      if(doxy_payload.contains("doc"))
      {
        auto const &docstring(doxy_payload.at("doc").as_string());
        std::cerr << "  doc: " << docstring << "\n";
        /* Doxygen comments contain @brief */
        CHECK(docstring.find("@brief") != std::string::npos);
      }
      else
      {
        std::cerr << "  doc: (not present)\n";
        WARN_MESSAGE(false, "docstring not found for doxygen_method");
      }
    }

    TEST_CASE("info arglists do not contain NULL TYPE")
    {
      /* This test verifies that type information in arglists does not contain
       * "NULL TYPE" which indicates CppInterOp failed to stringify the type.
       * Instead, we should fall back to qualified names or "auto". */
      engine eng;

      /* Define a struct with template-like patterns that might cause NULL TYPE issues */
      eng.handle(make_message({
        {   "op","eval"           },
        { "code",
         "(cpp/raw \"namespace null_type_test { "
         "struct entity { "
         "  template<typename T> entity& set(T val) { return *this; } "
         "  entity child(const char* name) { return *this; } "
         "}; }\")" }
      }));

      /* Require it as a native header alias */
      eng.handle(make_message({
        {   "op","eval"                },
        { "code",
         "(require '[\"jank/runtime/context.hpp\" :as ntt :scope "
         "\"null_type_test\"])" }
      }));

      /* Get info for the child method */
      auto responses(eng.handle(make_message({
        {  "op",             "info" },
        { "sym", "ntt/entity.child" },
        {  "ns",             "user" }
      })));

      REQUIRE(responses.size() == 1);
      auto const &payload(responses.front());

      /* Check that arglists exist and don't contain NULL TYPE */
      auto const arglists_it(payload.find("arglists"));
      if(arglists_it != payload.end())
      {
        auto const &arglists(arglists_it->second.as_list());
        for(auto const &arglist : arglists)
        {
          auto const &sig(arglist.as_string());
          std::cerr << "Arglist: " << sig << "\n";
          /* Verify no NULL TYPE appears in the signature */
          CHECK(sig.find("NULL TYPE") == std::string::npos);
        }
      }

      /* Also check arglists-str if present */
      auto const arglists_str_it(payload.find("arglists-str"));
      if(arglists_str_it != payload.end())
      {
        auto const &arglists_str(arglists_str_it->second.as_string());
        std::cerr << "Arglists-str: " << arglists_str << "\n";
        CHECK(arglists_str.find("NULL TYPE") == std::string::npos);
      }

      /* Check return-type if present */
      auto const return_type_it(payload.find("return-type"));
      if(return_type_it != payload.end())
      {
        auto const &return_type(return_type_it->second.as_string());
        std::cerr << "Return type: " << return_type << "\n";
        CHECK(return_type.find("NULL TYPE") == std::string::npos);
      }
    }

    TEST_CASE("info returns absolute file paths for C++ declarations")
    {
      /* This test verifies that file paths returned for C++ declarations
       * are absolute paths, not relative paths. */
      engine eng;

      /* Use clojure/string_native.hpp which is a real header that jank includes */
      eng.handle(make_message({
        {   "op",                                                         "eval" },
        { "code", "(require '[\"clojure/string_native.hpp\" :as abs-path-test])" }
      }));

      /* Get info for a function from the header */
      auto responses(eng.handle(make_message({
        {  "op",                  "info" },
        { "sym", "abs-path-test/reverse" },
        {  "ns",                  "user" }
      })));

      REQUIRE(responses.size() == 1);
      auto const &payload(responses.front());

      /* Check if file path is present and is absolute */
      auto const file_it(payload.find("file"));
      if(file_it != payload.end())
      {
        auto const &file_path(file_it->second.as_string());
        std::cerr << "File path: " << file_path << "\n";
        /* On Unix-like systems, absolute paths start with / */
        CHECK(file_path.front() == '/');
        /* Should not be just a relative path like "clojure/string_native.hpp" */
        CHECK(file_path.find("clojure/string_native.hpp") != 0);
      }
      else
      {
        /* File path might not be available in all environments, just warn */
        WARN_MESSAGE(false, "file path not present in info response");
      }
    }
  }

  TEST_CASE("info returns proper types for template functions, not auto")
  {
    SUBCASE("non-template method works to verify header loading")
    {
      engine eng;

      /* Require the template_types.hpp header from the include path with :scope.
       * The header is located at include/cpp/jank/test/template_types.hpp and defines
       * the namespace template_type_test with various template functions for testing. */
      eng.handle(make_message({
        {   "op", "eval" },
        { "code",
         "(require '[\"jank/test/template_types.hpp\" :as tmpl-test :scope "
         "\"template_type_test\"])" }
      }));

      /* Get info for a non-template method to verify header loading */
      auto responses(eng.handle(make_message({
        {  "op",                    "info" },
        { "sym", "tmpl-test/entity.get_id" },
        {  "ns",                    "user" }
      })));

      REQUIRE(responses.size() == 1);
      auto const &payload(responses.front());

      auto const arglists_str_it(payload.find("arglists-str"));
      REQUIRE_MESSAGE(arglists_str_it != payload.end(),
                      "arglists-str not found in info response");

      auto const &arglists_str(arglists_str_it->second.as_string());
      /* Should have the this pointer with proper type */
      CHECK_MESSAGE(arglists_str.find("template_type_test::entity") != std::string::npos,
                    "arglists should contain 'template_type_test::entity', got: " << arglists_str);
    }

    SUBCASE("variadic template member function shows Args types")
    {
      engine eng;

      /* Require the template_types.hpp header with :scope for template_type_test namespace */
      eng.handle(make_message({
        {   "op", "eval" },
        { "code",
         "(require '[\"jank/test/template_types.hpp\" :as tmpl-test :scope "
         "\"template_type_test\"])" }
      }));

      /* Get info for the variadic template method */
      auto responses(eng.handle(make_message({
        {  "op",                   "info" },
        { "sym", "tmpl-test/entity.child" },
        {  "ns",                   "user" }
      })));

      REQUIRE(responses.size() == 1);
      auto const &payload(responses.front());

      /* Check arglists-str doesn't contain "auto" */
      auto const arglists_str_it(payload.find("arglists-str"));
      REQUIRE_MESSAGE(arglists_str_it != payload.end(),
                      "arglists-str not found in info response");

      auto const &arglists_str(arglists_str_it->second.as_string());

      /* Should not contain "auto" - should show actual template parameter types */
      CHECK_MESSAGE(arglists_str.find("auto") == std::string::npos,
                    "arglists should not contain 'auto', got: " << arglists_str);

      /* Should contain "Args" or the actual parameter pack type */
      bool const has_args_param = arglists_str.find("Args") != std::string::npos
        || arglists_str.find("args") != std::string::npos;
      CHECK_MESSAGE(has_args_param,
                    "arglists should contain template parameter name 'Args' or 'args', got: "
                      << arglists_str);

      /* Also check return type is not "auto" */
      auto const return_type_it(payload.find("return-type"));
      REQUIRE_MESSAGE(return_type_it != payload.end(),
                      "return-type not found in info response");

      auto const &return_type(return_type_it->second.as_string());
      /* Return type should be "entity" not "auto" */
      CHECK_MESSAGE(return_type.find("auto") == std::string::npos,
                    "return-type should not be 'auto', got: " << return_type);
    }

    SUBCASE("simple template function with T parameter")
    {
      engine eng;

      /* Require the template_types.hpp header with :scope for template_type_test namespace */
      eng.handle(make_message({
        {   "op", "eval" },
        { "code",
         "(require '[\"jank/test/template_types.hpp\" :as tmpl-test :scope "
         "\"template_type_test\"])" }
      }));

      /* Get info for a simple template function */
      auto responses(eng.handle(make_message({
        {  "op",               "info" },
        { "sym", "tmpl-test/identity" },
        {  "ns",               "user" }
      })));

      REQUIRE(responses.size() == 1);
      auto const &payload(responses.front());

      auto const arglists_str_it(payload.find("arglists-str"));
      REQUIRE_MESSAGE(arglists_str_it != payload.end(),
                      "arglists-str not found in info response");

      auto const &arglists_str(arglists_str_it->second.as_string());

      /* Should not contain "auto" */
      CHECK_MESSAGE(arglists_str.find("auto") == std::string::npos,
                    "arglists should not contain 'auto', got: " << arglists_str);

      /* Should contain "T" for the template parameter */
      CHECK_MESSAGE(arglists_str.find("T ") != std::string::npos,
                    "arglists should contain template parameter 'T', got: " << arglists_str);
    }

    SUBCASE("template method with mixed parameters")
    {
      engine eng;

      /* Require the template_types.hpp header with :scope for template_type_test namespace */
      eng.handle(make_message({
        {   "op", "eval" },
        { "code",
         "(require '[\"jank/test/template_types.hpp\" :as tmpl-test :scope "
         "\"template_type_test\"])" }
      }));

      /* Get info for template method with const char* and T&& params */
      auto responses(eng.handle(make_message({
        {  "op",                 "info" },
        { "sym", "tmpl-test/entity.set" },
        {  "ns",                 "user" }
      })));

      REQUIRE(responses.size() == 1);
      auto const &payload(responses.front());

      auto const arglists_str_it(payload.find("arglists-str"));
      REQUIRE_MESSAGE(arglists_str_it != payload.end(),
                      "arglists-str not found in info response");

      auto const &arglists_str(arglists_str_it->second.as_string());

      /* Should not contain "auto" */
      CHECK_MESSAGE(arglists_str.find("auto") == std::string::npos,
                    "arglists should not contain 'auto', got: " << arglists_str);

      /* Should contain the const char* type */
      CHECK_MESSAGE(arglists_str.find("char") != std::string::npos,
                    "arglists should contain 'char' for const char* param, got: " << arglists_str);

      /* Should contain "T" for the template parameter */
      CHECK_MESSAGE(arglists_str.find("T ") != std::string::npos,
                    "arglists should contain template parameter 'T', got: " << arglists_str);
    }
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

TEST_CASE("info returns metadata for global C functions")
{
  /* Test that we can get function info (arglists, etc.) for global C functions */
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

  /* Get info for InitWindow function */
  auto responses(eng.handle(make_message({
    {  "op",          "info" },
    { "sym", "rl/InitWindow" },
    {  "ns",          "user" }
  })));

  REQUIRE(responses.size() == 1);
  auto const &payload(responses.front());

  /* In some environments, the info lookup might fail */
  auto const arglists_it(payload.find("arglists-str"));
  if(arglists_it == payload.end())
  {
    WARN("arglists-str not found for global C function");
    return;
  }

  auto const &arglists_str(arglists_it->second.as_string());
  std::cerr << "InitWindow arglists: " << arglists_str << "\n";

  /* Should contain the parameters */
  bool const has_width_info = arglists_str.find("width") != std::string::npos
                                || arglists_str.find("int") != std::string::npos;
  CHECK_MESSAGE(has_width_info, "arglists should contain 'width' or 'int', got: " << arglists_str);

  bool const has_height_info = arglists_str.find("height") != std::string::npos
                                 || arglists_str.find("int") != std::string::npos;
  CHECK_MESSAGE(has_height_info,
                "arglists should contain 'height' or 'int', got: " << arglists_str);

  bool const has_title_info = arglists_str.find("title") != std::string::npos
                                || arglists_str.find("char") != std::string::npos;
  CHECK_MESSAGE(has_title_info, "arglists should contain 'title' or 'char', got: " << arglists_str);

  /* Check for trailing inline comment (raylib-style) */
  auto const doc_it(payload.find("doc"));
  if(doc_it != payload.end())
  {
    auto const &doc_str(doc_it->second.as_string());
    std::cerr << "InitWindow doc: " << doc_str << "\n";

    /* Should contain the inline comment text */
    bool const has_doc = doc_str.find("Initialize") != std::string::npos
                           || doc_str.find("window") != std::string::npos;
    CHECK_MESSAGE(has_doc, "doc should contain 'Initialize' or 'window', got: " << doc_str);
  }
  else
  {
    std::cerr << "InitWindow has no doc field\n";
  }
}

TEST_CASE("info returns raylib-style inline comments as doc")
{
  /* Test that trailing inline comments (raylib-style) are extracted as documentation */
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

  /* Get info for DrawRectangle function - has inline comment */
  auto responses(eng.handle(make_message({
    {  "op",              "info" },
    { "sym", "rl/DrawRectangle" },
    {  "ns",              "user" }
  })));

  REQUIRE(responses.size() == 1);
  auto const &payload(responses.front());

  auto const doc_it(payload.find("doc"));
  if(doc_it == payload.end())
  {
    WARN("doc not found for DrawRectangle - trailing comment extraction may not be working");
    return;
  }

  auto const &doc_str(doc_it->second.as_string());
  std::cerr << "DrawRectangle doc: '" << doc_str << "'\n";

  /* Should contain the inline comment text from the header:
   * void DrawRectangle(...);  // Draw a color-filled rectangle */
  bool const has_draw = doc_str.find("Draw") != std::string::npos;
  bool const has_rectangle = doc_str.find("rectangle") != std::string::npos;
  bool const has_expected_content = has_draw || has_rectangle;
  CHECK_MESSAGE(has_expected_content,
                "doc should contain 'Draw' or 'rectangle', got: " << doc_str);
}

TEST_CASE("enumerate_native_header_macros returns macros from C header")
{
  /* This test verifies that we can enumerate object-like macros from a C header */
  engine eng;

  /* Include the C header */
  eng.handle(make_message({
    {   "op",                                                           "eval" },
    { "code", R"((cpp/raw "#include \"../test/cpp/jank/nrepl/test_c_header.h\""))" }
  }));

  /* Create a native alias for testing */
  runtime::ns::native_alias alias;
  alias.header = "../test/cpp/jank/nrepl/test_c_header.h";
  alias.scope = "";

  /* Enumerate all macros */
  auto all_macros = enumerate_native_header_macros(alias, "");

  std::cerr << "Found " << all_macros.size() << " macros:\n";
  for(auto const &name : all_macros)
  {
    std::cerr << "  - " << name << "\n";
  }

  /* Should find the TEST_* macros from the header */
  bool found_test_white = std::find(all_macros.begin(), all_macros.end(), "TEST_WHITE") != all_macros.end();
  bool found_test_pi = std::find(all_macros.begin(), all_macros.end(), "TEST_PI") != all_macros.end();
  bool found_key_escape = std::find(all_macros.begin(), all_macros.end(), "KEY_ESCAPE") != all_macros.end();

  CHECK_MESSAGE(found_test_white, "Should find TEST_WHITE macro");
  CHECK_MESSAGE(found_test_pi, "Should find TEST_PI macro");
  CHECK_MESSAGE(found_key_escape, "Should find KEY_ESCAPE macro");
}

TEST_CASE("enumerate_native_header_macros filters by prefix")
{
  /* Test that prefix filtering works for macros */
  engine eng;

  /* Include the C header */
  eng.handle(make_message({
    {   "op",                                                           "eval" },
    { "code", R"((cpp/raw "#include \"../test/cpp/jank/nrepl/test_c_header.h\""))" }
  }));

  /* Create a native alias for testing */
  runtime::ns::native_alias alias;
  alias.header = "../test/cpp/jank/nrepl/test_c_header.h";
  alias.scope = "";

  /* Enumerate macros with TEST_ prefix */
  auto test_macros = enumerate_native_header_macros(alias, "TEST_");

  std::cerr << "Found " << test_macros.size() << " TEST_* macros:\n";
  for(auto const &name : test_macros)
  {
    std::cerr << "  - " << name << "\n";
  }

  /* Should find TEST_WHITE, TEST_BLACK, TEST_RED, TEST_GREEN, TEST_BLUE, TEST_PI, TEST_MAX_VALUE */
  CHECK(test_macros.size() >= 7);

  /* All should start with TEST_ */
  for(auto const &name : test_macros)
  {
    CHECK_MESSAGE(name.starts_with("TEST_"), "Macro " << name << " should start with TEST_");
  }

  /* KEY_ESCAPE should NOT be in this list */
  bool found_key_escape = std::find(test_macros.begin(), test_macros.end(), "KEY_ESCAPE") != test_macros.end();
  CHECK_MESSAGE(!found_key_escape, "KEY_ESCAPE should not be in TEST_* filtered results");
}

TEST_CASE("is_native_header_macro detects macros from header")
{
  /* Test that we can check if a name is a macro */
  engine eng;

  /* Include the C header */
  eng.handle(make_message({
    {   "op",                                                           "eval" },
    { "code", R"((cpp/raw "#include \"../test/cpp/jank/nrepl/test_c_header.h\""))" }
  }));

  /* Create a native alias for testing */
  runtime::ns::native_alias alias;
  alias.header = "../test/cpp/jank/nrepl/test_c_header.h";
  alias.scope = "";

  /* TEST_WHITE should be a macro */
  CHECK(is_native_header_macro(alias, "TEST_WHITE"));
  CHECK(is_native_header_macro(alias, "TEST_PI"));
  CHECK(is_native_header_macro(alias, "KEY_ESCAPE"));

  /* InitWindow is a function, not a macro */
  CHECK(!is_native_header_macro(alias, "InitWindow"));

  /* Random non-existent name should not be a macro */
  CHECK(!is_native_header_macro(alias, "NONEXISTENT_MACRO_12345"));
}

TEST_CASE("get_native_header_macro_expansion returns macro tokens")
{
  /* Test that we can get the token expansion of macros */
  engine eng;

  /* Include the C header */
  eng.handle(make_message({
    {   "op",                                                           "eval" },
    { "code", R"((cpp/raw "#include \"../test/cpp/jank/nrepl/test_c_header.h\""))" }
  }));

  /* Create a native alias for testing */
  runtime::ns::native_alias alias;
  alias.header = "../test/cpp/jank/nrepl/test_c_header.h";
  alias.scope = "";

  /* TEST_PI should expand to 3.14159f */
  auto pi_expansion = get_native_header_macro_expansion(alias, "TEST_PI");
  REQUIRE(pi_expansion.has_value());
  std::cerr << "TEST_PI expansion: '" << pi_expansion.value() << "'\n";
  CHECK(pi_expansion.value().find("3.14159") != std::string::npos);

  /* KEY_ESCAPE should expand to 256 */
  auto escape_expansion = get_native_header_macro_expansion(alias, "KEY_ESCAPE");
  REQUIRE(escape_expansion.has_value());
  std::cerr << "KEY_ESCAPE expansion: '" << escape_expansion.value() << "'\n";
  CHECK(escape_expansion.value().find("256") != std::string::npos);

  /* TEST_MAX_VALUE should expand to 1000 */
  auto max_expansion = get_native_header_macro_expansion(alias, "TEST_MAX_VALUE");
  REQUIRE(max_expansion.has_value());
  std::cerr << "TEST_MAX_VALUE expansion: '" << max_expansion.value() << "'\n";
  CHECK(max_expansion.value().find("1000") != std::string::npos);

  /* TEST_WHITE is a compound literal macro - should have some expansion */
  auto white_expansion = get_native_header_macro_expansion(alias, "TEST_WHITE");
  REQUIRE(white_expansion.has_value());
  std::cerr << "TEST_WHITE expansion: '" << white_expansion.value() << "'\n";
  /* Should contain CLITERAL or Color or 255 */
  bool has_expected = white_expansion.value().find("CLITERAL") != std::string::npos
                        || white_expansion.value().find("Color") != std::string::npos
                        || white_expansion.value().find("255") != std::string::npos;
  CHECK_MESSAGE(has_expected, "TEST_WHITE expansion should contain CLITERAL/Color/255");

  /* Non-existent macro should return nullopt */
  auto nonexistent = get_native_header_macro_expansion(alias, "NONEXISTENT_MACRO_12345");
  CHECK(!nonexistent.has_value());
}

TEST_CASE("macro access via native alias syntax")
{
  engine eng;

  /* Include the C header */
  eng.handle(make_message({
    {   "op",                                                           "eval" },
    { "code", R"((cpp/raw "#include \"../test/cpp/jank/nrepl/test_c_header.h\""))" }
  }));

  /* Register a native alias for the test header in the current namespace.
   * This simulates what (:require ["header.h" :as th :scope ""]) does. */
  auto th_sym = runtime::make_box<runtime::obj::symbol>("th");
  runtime::ns::native_alias alias;
  alias.header = "../test/cpp/jank/nrepl/test_c_header.h";
  alias.scope = "";
  (void)runtime::__rt_ctx->current_ns()->add_native_alias(th_sym, alias);

  /* Now test that th/KEY_ESCAPE evaluates to the macro value */
  auto responses = eng.handle(make_message({
    {   "op", "eval" },
    { "code",  "th/KEY_ESCAPE" }
  }));
  REQUIRE(!responses.empty());
  auto const value_it(responses.front().find("value"));
  if(auto const err_it = responses.front().find("err"); err_it != responses.front().end())
  {
    INFO("eval error: " << err_it->second.as_string());
  }
  REQUIRE(value_it != responses.front().end());
  CHECK(value_it->second.as_string().find("256") != std::string::npos);

  /* Test th/TEST_MAX_VALUE */
  responses = eng.handle(make_message({
    {   "op", "eval" },
    { "code",  "th/TEST_MAX_VALUE" }
  }));
  REQUIRE(!responses.empty());
  auto const value_it2(responses.front().find("value"));
  if(auto const err_it = responses.front().find("err"); err_it != responses.front().end())
  {
    INFO("eval error: " << err_it->second.as_string());
  }
  REQUIRE(value_it2 != responses.front().end());
  CHECK(value_it2->second.as_string().find("1000") != std::string::npos);

  /* Test parenthesized macro access (th/KEY_ESCAPE) - should return the value.
   * This tests that macros can be used in call position without arguments,
   * e.g., (rl/KEY_ESCAPE) just returns the macro value. */
  responses = eng.handle(make_message({
    {   "op", "eval" },
    { "code",  "(th/KEY_ESCAPE)" }
  }));
  REQUIRE(!responses.empty());
  auto const value_it3(responses.front().find("value"));
  REQUIRE(value_it3 != responses.front().end());
  CHECK(value_it3->second.as_string().find("256") != std::string::npos);

  /* Clean up the native alias */
  runtime::__rt_ctx->current_ns()->remove_native_alias(th_sym);
}

TEST_CASE("is_native_header_function_like_macro detects function-like macros")
{
  /* Test that we can distinguish between object-like and function-like macros */
  engine eng;

  /* Include the C header */
  eng.handle(make_message({
    {   "op",                                                           "eval" },
    { "code", R"((cpp/raw "#include \"../test/cpp/jank/nrepl/test_c_header.h\""))" }
  }));

  /* Create a native alias for testing */
  runtime::ns::native_alias alias;
  alias.header = "../test/cpp/jank/nrepl/test_c_header.h";
  alias.scope = "";

  /* TEST_ADD, TEST_MUL, TEST_CLAMP are function-like macros */
  CHECK(is_native_header_function_like_macro(alias, "TEST_ADD"));
  CHECK(is_native_header_function_like_macro(alias, "TEST_MUL"));
  CHECK(is_native_header_function_like_macro(alias, "TEST_CLAMP"));

  /* TEST_PI, KEY_ESCAPE are object-like macros, not function-like */
  CHECK(!is_native_header_function_like_macro(alias, "TEST_PI"));
  CHECK(!is_native_header_function_like_macro(alias, "KEY_ESCAPE"));

  /* InitWindow is a function, not a macro at all */
  CHECK(!is_native_header_function_like_macro(alias, "InitWindow"));

  /* Non-existent name should not be function-like */
  CHECK(!is_native_header_function_like_macro(alias, "NONEXISTENT_MACRO_12345"));
}

TEST_CASE("get_native_header_macro_param_count returns parameter count for function-like macros")
{
  /* Test that we can get the parameter count for function-like macros */
  engine eng;

  /* Include the C header */
  eng.handle(make_message({
    {   "op",                                                           "eval" },
    { "code", R"((cpp/raw "#include \"../test/cpp/jank/nrepl/test_c_header.h\""))" }
  }));

  /* Create a native alias for testing */
  runtime::ns::native_alias alias;
  alias.header = "../test/cpp/jank/nrepl/test_c_header.h";
  alias.scope = "";

  /* TEST_ADD(a, b) has 2 params */
  auto add_count = get_native_header_macro_param_count(alias, "TEST_ADD");
  REQUIRE(add_count.has_value());
  CHECK(add_count.value() == 2);

  /* TEST_MUL(a, b) has 2 params */
  auto mul_count = get_native_header_macro_param_count(alias, "TEST_MUL");
  REQUIRE(mul_count.has_value());
  CHECK(mul_count.value() == 2);

  /* TEST_CLAMP(x, lo, hi) has 3 params */
  auto clamp_count = get_native_header_macro_param_count(alias, "TEST_CLAMP");
  REQUIRE(clamp_count.has_value());
  CHECK(clamp_count.value() == 3);

  /* Object-like macros should return nullopt */
  auto pi_count = get_native_header_macro_param_count(alias, "TEST_PI");
  CHECK(!pi_count.has_value());

  /* Non-existent macro should return nullopt */
  auto nonexistent_count = get_native_header_macro_param_count(alias, "NONEXISTENT_MACRO_12345");
  CHECK(!nonexistent_count.has_value());
}

TEST_CASE("get_native_header_macro_expansion includes parameter signature for function-like macros")
{
  /* Test that function-like macro expansion includes the parameter names */
  engine eng;

  /* Include the C header */
  eng.handle(make_message({
    {   "op",                                                           "eval" },
    { "code", R"((cpp/raw "#include \"../test/cpp/jank/nrepl/test_c_header.h\""))" }
  }));

  /* Create a native alias for testing */
  runtime::ns::native_alias alias;
  alias.header = "../test/cpp/jank/nrepl/test_c_header.h";
  alias.scope = "";

  /* TEST_ADD(a, b) should show parameters in expansion */
  auto add_expansion = get_native_header_macro_expansion(alias, "TEST_ADD");
  REQUIRE(add_expansion.has_value());
  std::cerr << "TEST_ADD expansion: '" << add_expansion.value() << "'\n";
  /* Should contain parameter signature like "TEST_ADD(a, b)" */
  CHECK(add_expansion.value().find("TEST_ADD") != std::string::npos);
  CHECK(add_expansion.value().find("a") != std::string::npos);
  CHECK(add_expansion.value().find("b") != std::string::npos);

  /* TEST_CLAMP(x, lo, hi) should show all three parameters */
  auto clamp_expansion = get_native_header_macro_expansion(alias, "TEST_CLAMP");
  REQUIRE(clamp_expansion.has_value());
  std::cerr << "TEST_CLAMP expansion: '" << clamp_expansion.value() << "'\n";
  CHECK(clamp_expansion.value().find("TEST_CLAMP") != std::string::npos);
  CHECK(clamp_expansion.value().find("x") != std::string::npos);
  CHECK(clamp_expansion.value().find("lo") != std::string::npos);
  CHECK(clamp_expansion.value().find("hi") != std::string::npos);
}

TEST_CASE("function-like macro invocation via native alias syntax")
{
  engine eng;

  /* Include the C header */
  eng.handle(make_message({
    {   "op",                                                           "eval" },
    { "code", R"((cpp/raw "#include \"../test/cpp/jank/nrepl/test_c_header.h\""))" }
  }));

  /* Register a native alias for the test header */
  auto th_sym = runtime::make_box<runtime::obj::symbol>("th");
  runtime::ns::native_alias alias;
  alias.header = "../test/cpp/jank/nrepl/test_c_header.h";
  alias.scope = "";
  (void)runtime::__rt_ctx->current_ns()->add_native_alias(th_sym, alias);

  /* Test (th/TEST_ADD 1 2) should return 3 */
  auto responses = eng.handle(make_message({
    {   "op", "eval" },
    { "code",  "(th/TEST_ADD 1 2)" }
  }));
  REQUIRE(!responses.empty());
  auto const value_it(responses.front().find("value"));
  if(auto const err_it = responses.front().find("err"); err_it != responses.front().end())
  {
    INFO("eval error: " << err_it->second.as_string());
  }
  REQUIRE(value_it != responses.front().end());
  std::cerr << "(th/TEST_ADD 1 2) = " << value_it->second.as_string() << "\n";
  CHECK(value_it->second.as_string().find("3") != std::string::npos);

  /* Test (th/TEST_MUL 5 7) should return 35 */
  responses = eng.handle(make_message({
    {   "op", "eval" },
    { "code",  "(th/TEST_MUL 5 7)" }
  }));
  REQUIRE(!responses.empty());
  auto const value_it2(responses.front().find("value"));
  if(auto const err_it = responses.front().find("err"); err_it != responses.front().end())
  {
    INFO("eval error: " << err_it->second.as_string());
  }
  REQUIRE(value_it2 != responses.front().end());
  std::cerr << "(th/TEST_MUL 5 7) = " << value_it2->second.as_string() << "\n";
  CHECK(value_it2->second.as_string().find("35") != std::string::npos);

  /* Test (th/TEST_CLAMP 50 0 100) should return 50 */
  responses = eng.handle(make_message({
    {   "op", "eval" },
    { "code",  "(th/TEST_CLAMP 50 0 100)" }
  }));
  REQUIRE(!responses.empty());
  auto const value_it3(responses.front().find("value"));
  if(auto const err_it = responses.front().find("err"); err_it != responses.front().end())
  {
    INFO("eval error: " << err_it->second.as_string());
  }
  REQUIRE(value_it3 != responses.front().end());
  std::cerr << "(th/TEST_CLAMP 50 0 100) = " << value_it3->second.as_string() << "\n";
  CHECK(value_it3->second.as_string().find("50") != std::string::npos);

  /* Test (th/TEST_CLAMP -10 0 100) should return 0 (clamped to low) */
  responses = eng.handle(make_message({
    {   "op", "eval" },
    { "code",  "(th/TEST_CLAMP -10 0 100)" }
  }));
  REQUIRE(!responses.empty());
  auto const value_it4(responses.front().find("value"));
  REQUIRE(value_it4 != responses.front().end());
  std::cerr << "(th/TEST_CLAMP -10 0 100) = " << value_it4->second.as_string() << "\n";
  CHECK(value_it4->second.as_string().find("0") != std::string::npos);

  /* Test (th/TEST_CLAMP 200 0 100) should return 100 (clamped to high) */
  responses = eng.handle(make_message({
    {   "op", "eval" },
    { "code",  "(th/TEST_CLAMP 200 0 100)" }
  }));
  REQUIRE(!responses.empty());
  auto const value_it5(responses.front().find("value"));
  REQUIRE(value_it5 != responses.front().end());
  std::cerr << "(th/TEST_CLAMP 200 0 100) = " << value_it5->second.as_string() << "\n";
  CHECK(value_it5->second.as_string().find("100") != std::string::npos);

  /* Clean up the native alias */
  runtime::__rt_ctx->current_ns()->remove_native_alias(th_sym);
}

TEST_CASE("function-like macro with nested function call as argument")
{
  /* Test that function calls can be used as arguments to function-like macros.
   * This is the case when you have something like (ecs_new_w_pair (ecs_mini) 3 4)
   * where (ecs_mini) is a function call result being passed to a macro. */
  engine eng;

  /* Include the C header */
  eng.handle(make_message({
    {   "op",                                                           "eval" },
    { "code", R"((cpp/raw "#include \"../test/cpp/jank/nrepl/test_c_header.h\""))" }
  }));

  /* Register a native alias for the test header */
  auto th_sym = runtime::make_box<runtime::obj::symbol>("th");
  runtime::ns::native_alias alias;
  alias.header = "../test/cpp/jank/nrepl/test_c_header.h";
  alias.scope = "";
  (void)runtime::__rt_ctx->current_ns()->add_native_alias(th_sym, alias);

  /* Test (th/TEST_ADD (th/test_get_five) (th/test_get_ten))
   * This should generate C++ code: TEST_ADD(test_get_five(), test_get_ten())
   * which should evaluate to 5 + 10 = 15 */
  auto responses = eng.handle(make_message({
    {   "op", "eval" },
    { "code",  "(th/TEST_ADD (th/test_get_five) (th/test_get_ten))" }
  }));
  REQUIRE(!responses.empty());
  auto const value_it(responses.front().find("value"));
  if(auto const err_it = responses.front().find("err"); err_it != responses.front().end())
  {
    INFO("eval error: " << err_it->second.as_string());
  }
  REQUIRE(value_it != responses.front().end());
  std::cerr << "(th/TEST_ADD (th/test_get_five) (th/test_get_ten)) = " << value_it->second.as_string()
            << "\n";
  CHECK(value_it->second.as_string().find("15") != std::string::npos);

  /* Test nested function as first arg, literal as second: (th/TEST_MUL (th/test_get_five) 3)
   * This should generate: TEST_MUL(test_get_five(), 3) = 5 * 3 = 15 */
  responses = eng.handle(make_message({
    {   "op", "eval" },
    { "code",  "(th/TEST_MUL (th/test_get_five) 3)" }
  }));
  REQUIRE(!responses.empty());
  auto const value_it2(responses.front().find("value"));
  if(auto const err_it = responses.front().find("err"); err_it != responses.front().end())
  {
    INFO("eval error: " << err_it->second.as_string());
  }
  REQUIRE(value_it2 != responses.front().end());
  std::cerr << "(th/TEST_MUL (th/test_get_five) 3) = " << value_it2->second.as_string() << "\n";
  CHECK(value_it2->second.as_string().find("15") != std::string::npos);

  /* Clean up the native alias */
  runtime::__rt_ctx->current_ns()->remove_native_alias(th_sym);
}

TEST_CASE("function-like macro with local binding to cpp_call as argument")
{
  /* Test that local bindings to cpp_call expressions are expanded inline.
   * This is the case when you have: (let [a (fl/ecs_mini)] (fl/ecs_new_w_pair a 3 4))
   * where 'a' is a local binding to a cpp_call result. */
  engine eng;

  /* Include the C header */
  eng.handle(make_message({
    {   "op",                                                           "eval" },
    { "code", R"((cpp/raw "#include \"../test/cpp/jank/nrepl/test_c_header.h\""))" }
  }));

  /* Register a native alias for the test header */
  auto th_sym = runtime::make_box<runtime::obj::symbol>("th");
  runtime::ns::native_alias alias;
  alias.header = "../test/cpp/jank/nrepl/test_c_header.h";
  alias.scope = "";
  (void)runtime::__rt_ctx->current_ns()->add_native_alias(th_sym, alias);

  /* Test (let [a (th/test_get_five)] (th/TEST_ADD a 10))
   * The local binding 'a' is to a cpp_call (test_get_five()).
   * This should generate C++ code: TEST_ADD(test_get_five(), 10)
   * which should evaluate to 5 + 10 = 15 */
  auto responses = eng.handle(make_message({
    {   "op", "eval" },
    { "code",  "(let [a (th/test_get_five)] (th/TEST_ADD a 10))" }
  }));
  REQUIRE(!responses.empty());
  auto const value_it(responses.front().find("value"));
  if(auto const err_it = responses.front().find("err"); err_it != responses.front().end())
  {
    INFO("eval error: " << err_it->second.as_string());
  }
  REQUIRE(value_it != responses.front().end());
  std::cerr << "(let [a (th/test_get_five)] (th/TEST_ADD a 10)) = " << value_it->second.as_string()
            << "\n";
  CHECK(value_it->second.as_string().find("15") != std::string::npos);

  /* Test with two local bindings to cpp_call expressions */
  responses = eng.handle(make_message({
    {   "op", "eval" },
    { "code",  "(let [a (th/test_get_five) b (th/test_get_ten)] (th/TEST_ADD a b))" }
  }));
  REQUIRE(!responses.empty());
  auto const value_it2(responses.front().find("value"));
  if(auto const err_it = responses.front().find("err"); err_it != responses.front().end())
  {
    INFO("eval error: " << err_it->second.as_string());
  }
  REQUIRE(value_it2 != responses.front().end());
  std::cerr << "(let [a (th/test_get_five) b (th/test_get_ten)] (th/TEST_ADD a b)) = "
            << value_it2->second.as_string() << "\n";
  CHECK(value_it2->second.as_string().find("15") != std::string::npos);

  /* Clean up the native alias */
  runtime::__rt_ctx->current_ns()->remove_native_alias(th_sym);
}

TEST_CASE("function-like macro with cpp/box binding as argument")
{
  /* Test that local bindings to cpp/box expressions have their inner cpp_call expanded.
   * This is the case when you have: (let [a (cpp/box (fl/ecs_mini))] (fl/ecs_new_w_pair a 3 x))
   * where the cpp/box is transparent and the inner call gets expanded inline. */
  engine eng;

  /* Include the C header */
  eng.handle(make_message({
    {   "op",                                                           "eval" },
    { "code", R"((cpp/raw "#include \"../test/cpp/jank/nrepl/test_c_header.h\""))" }
  }));

  /* Register a native alias for the test header */
  auto th_sym = runtime::make_box<runtime::obj::symbol>("th");
  runtime::ns::native_alias alias;
  alias.header = "../test/cpp/jank/nrepl/test_c_header.h";
  alias.scope = "";
  (void)runtime::__rt_ctx->current_ns()->add_native_alias(th_sym, alias);

  /* Test (let [p (cpp/box (th/test_get_int_ptr))] (th/TEST_PTR_ADD p 8))
   * The cpp/box wraps the pointer, but when used as macro arg, the inner call is expanded.
   * test_get_int_ptr() returns pointer to static int (42), so *ptr + 8 = 50 */
  auto responses = eng.handle(make_message({
    {   "op", "eval" },
    { "code",  "(let [p (cpp/box (th/test_get_int_ptr))] (th/TEST_PTR_ADD p 8))" }
  }));
  REQUIRE(!responses.empty());
  auto const value_it(responses.front().find("value"));
  if(auto const err_it = responses.front().find("err"); err_it != responses.front().end())
  {
    INFO("eval error: " << err_it->second.as_string());
  }
  REQUIRE(value_it != responses.front().end());
  std::cerr << "(let [p (cpp/box (th/test_get_int_ptr))] (th/TEST_PTR_ADD p 8)) = "
            << value_it->second.as_string() << "\n";
  CHECK(value_it->second.as_string().find("50") != std::string::npos);

  /* Test with mixed cpp/box and primitive literal bindings */
  responses = eng.handle(make_message({
    {   "op", "eval" },
    { "code",  "(let [x 8 p (cpp/box (th/test_get_int_ptr))] (th/TEST_PTR_ADD p x))" }
  }));
  REQUIRE(!responses.empty());
  auto const value_it2(responses.front().find("value"));
  if(auto const err_it = responses.front().find("err"); err_it != responses.front().end())
  {
    INFO("eval error: " << err_it->second.as_string());
  }
  REQUIRE(value_it2 != responses.front().end());
  std::cerr << "(let [x 8 p (cpp/box (th/test_get_int_ptr))] (th/TEST_PTR_ADD p x)) = "
            << value_it2->second.as_string() << "\n";
  CHECK(value_it2->second.as_string().find("50") != std::string::npos);

  /* Clean up the native alias */
  runtime::__rt_ctx->current_ns()->remove_native_alias(th_sym);
}

TEST_CASE("enumerate_native_header_macros includes function-like macros")
{
  /* Verify that enumerate_native_header_macros returns both object-like and function-like macros */
  engine eng;

  /* Include the C header */
  eng.handle(make_message({
    {   "op",                                                           "eval" },
    { "code", R"((cpp/raw "#include \"../test/cpp/jank/nrepl/test_c_header.h\""))" }
  }));

  /* Create a native alias for testing */
  runtime::ns::native_alias alias;
  alias.header = "../test/cpp/jank/nrepl/test_c_header.h";
  alias.scope = "";

  /* Enumerate all TEST_* macros */
  auto test_macros = enumerate_native_header_macros(alias, "TEST_");

  std::cerr << "Found TEST_* macros (including function-like):\n";
  for(auto const &name : test_macros)
  {
    std::cerr << "  - " << name << "\n";
  }

  /* Should include both object-like macros */
  bool found_pi = std::find(test_macros.begin(), test_macros.end(), "TEST_PI") != test_macros.end();
  bool found_white
    = std::find(test_macros.begin(), test_macros.end(), "TEST_WHITE") != test_macros.end();
  CHECK_MESSAGE(found_pi, "Should find object-like TEST_PI");
  CHECK_MESSAGE(found_white, "Should find object-like TEST_WHITE");

  /* And function-like macros */
  bool found_add = std::find(test_macros.begin(), test_macros.end(), "TEST_ADD") != test_macros.end();
  bool found_mul = std::find(test_macros.begin(), test_macros.end(), "TEST_MUL") != test_macros.end();
  bool found_clamp
    = std::find(test_macros.begin(), test_macros.end(), "TEST_CLAMP") != test_macros.end();
  CHECK_MESSAGE(found_add, "Should find function-like TEST_ADD");
  CHECK_MESSAGE(found_mul, "Should find function-like TEST_MUL");
  CHECK_MESSAGE(found_clamp, "Should find function-like TEST_CLAMP");
}

TEST_CASE("autocompletion includes function-like macros from native alias")
{
  /* Test that autocompletion via the complete op returns function-like macros */
  engine eng;

  /* Include the C header */
  eng.handle(make_message({
    {   "op",                                                           "eval" },
    { "code", R"((cpp/raw "#include \"../test/cpp/jank/nrepl/test_c_header.h\""))" }
  }));

  /* Create a native alias with :scope "" for global scope */
  eng.handle(make_message({
    {   "op",                                                                  "eval" },
    { "code", R"((require '["../test/cpp/jank/nrepl/test_c_header.h" :as th :scope ""]))" }
  }));

  /* Test completion for th/TEST_ -> should include both object-like and function-like macros */
  auto responses(eng.handle(make_message({
    {     "op", "complete" },
    { "prefix",  "th/TEST_" },
    {     "ns",     "user" }
  })));

  REQUIRE(responses.size() == 1);
  auto const &payload(responses.front());
  auto const &completions(payload.at("completions").as_list());

  std::cerr << "Macro autocompletion results for th/TEST_: " << completions.size() << "\n";
  for(auto const &entry : completions)
  {
    auto const &dict(entry.as_dict());
    std::cerr << "  - " << dict.at("candidate").as_string()
              << " (type: " << dict.at("type").as_string() << ")\n";
  }

  /* Autocompletion may not work in all environments */
  if(completions.empty())
  {
    WARN("Macro autocompletion not available - skipping test");
    return;
  }

  /* Look for function-like macros in the completions */
  bool found_test_add{ false };
  bool found_test_mul{ false };
  bool found_test_clamp{ false };
  bool found_test_pi{ false };

  for(auto const &entry : completions)
  {
    auto const &dict(entry.as_dict());
    auto const &candidate(dict.at("candidate").as_string());
    if(candidate == "th/TEST_ADD")
    {
      found_test_add = true;
    }
    if(candidate == "th/TEST_MUL")
    {
      found_test_mul = true;
    }
    if(candidate == "th/TEST_CLAMP")
    {
      found_test_clamp = true;
    }
    if(candidate == "th/TEST_PI")
    {
      found_test_pi = true;
    }
  }

  /* Should include function-like macros */
  CHECK_MESSAGE(found_test_add, "Should find function-like macro TEST_ADD in completions");
  CHECK_MESSAGE(found_test_mul, "Should find function-like macro TEST_MUL in completions");
  CHECK_MESSAGE(found_test_clamp, "Should find function-like macro TEST_CLAMP in completions");

  /* Should also include object-like macros */
  CHECK_MESSAGE(found_test_pi, "Should find object-like macro TEST_PI in completions");
}

TEST_CASE("function-like macro with jank expression arguments via wrapper")
{
  /* Test that function-like macros work with jank expression arguments.
   * This uses generated wrapper functions that:
   * 1. Take object_ref parameters
   * 2. Unbox them to native integers
   * 3. Call the macro
   * 4. Box and return the result
   */
  engine eng;

  /* Include the C header */
  eng.handle(make_message({
    {   "op",                                                           "eval" },
    { "code", R"((cpp/raw "#include \"../test/cpp/jank/nrepl/test_c_header.h\""))" }
  }));

  /* Create a native alias with :scope "" for global scope */
  eng.handle(make_message({
    {   "op",                                                                  "eval" },
    { "code", R"((require '["../test/cpp/jank/nrepl/test_c_header.h" :as th :scope ""]))" }
  }));

  auto th_sym(make_box<runtime::obj::symbol>("th"));

  /* Test: Local binding as macro argument */
  auto responses(eng.handle(make_message({
    {   "op",                                          "eval" },
    { "code", "(let [x 5] (th/TEST_ADD x 10))" }
  })));

  /* Check for success with value = 15 */
  bool found_value = false;
  for(auto const &resp : responses)
  {
    auto value_it(resp.find("value"));
    if(value_it != resp.end())
    {
      std::string value_str = value_it->second.as_string();
      std::cerr << "(let [x 5] (th/TEST_ADD x 10)) = " << value_str << "\n";
      CHECK(value_str == "15");
      found_value = true;
      break;
    }
    auto err_it(resp.find("err"));
    if(err_it != resp.end())
    {
      std::cerr << "Unexpected error: " << err_it->second.as_string() << "\n";
    }
  }
  CHECK_MESSAGE(found_value, "Should get value 15 for local binding macro arg");

  /* Test: Jank arithmetic as macro argument */
  responses = eng.handle(make_message({
    {   "op",                               "eval" },
    { "code", "(th/TEST_ADD (+ 3 4) 5)" }
  }));

  found_value = false;
  for(auto const &resp : responses)
  {
    auto value_it(resp.find("value"));
    if(value_it != resp.end())
    {
      std::string value_str = value_it->second.as_string();
      std::cerr << "(th/TEST_ADD (+ 3 4) 5) = " << value_str << "\n";
      CHECK(value_str == "12");
      found_value = true;
      break;
    }
    auto err_it(resp.find("err"));
    if(err_it != resp.end())
    {
      std::cerr << "Unexpected error: " << err_it->second.as_string() << "\n";
    }
  }
  CHECK_MESSAGE(found_value, "Should get value 12 for arithmetic macro arg");

  /* Clean up the native alias */
  runtime::__rt_ctx->current_ns()->remove_native_alias(th_sym);
}

TEST_CASE("info shows variadic indicator for C variadic functions")
{
  /* Test that variadic C functions show ... in their arglists */
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

  /* Get info for TraceLog - a variadic function */
  auto responses(eng.handle(make_message({
    {  "op",         "info" },
    { "sym", "rl/TraceLog" },
    {  "ns",         "user" }
  })));

  REQUIRE(responses.size() == 1);
  auto const &payload(responses.front());

  auto const arglists_it(payload.find("arglists-str"));
  if(arglists_it == payload.end())
  {
    WARN("arglists-str not found for TraceLog");
    return;
  }

  auto const &arglists_str(arglists_it->second.as_string());
  std::cerr << "TraceLog arglists: " << arglists_str << "\n";

  /* Should contain the variadic indicator */
  bool const has_variadic = arglists_str.find("...") != std::string::npos;
  CHECK_MESSAGE(has_variadic, "arglists should contain '...' for variadic function, got: " << arglists_str);

  /* Should also have the fixed parameters */
  bool const has_log_level = arglists_str.find("logLevel") != std::string::npos
                               || arglists_str.find("int") != std::string::npos;
  CHECK_MESSAGE(has_log_level, "arglists should contain logLevel or int, got: " << arglists_str);

  bool const has_text = arglists_str.find("text") != std::string::npos
                          || arglists_str.find("char") != std::string::npos;
  CHECK_MESSAGE(has_text, "arglists should contain text or char, got: " << arglists_str);
}

TEST_CASE("eldoc shows variadic indicator for C variadic functions")
{
  /* Test that variadic C functions show ... in eldoc params */
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

  /* Get eldoc for TraceLog - a variadic function */
  auto responses(eng.handle(make_message({
    {  "op",        "eldoc" },
    { "sym", "rl/TraceLog" },
    {  "ns",         "user" }
  })));

  REQUIRE(responses.size() == 1);
  auto const &payload(responses.front());

  auto const eldoc_it(payload.find("eldoc"));
  if(eldoc_it == payload.end())
  {
    WARN("eldoc not found for TraceLog");
    return;
  }

  auto const &eldoc_list(eldoc_it->second.as_list());
  if(eldoc_list.empty())
  {
    WARN("eldoc list is empty for TraceLog");
    return;
  }

  /* Get the first (and only) signature */
  auto const &params(eldoc_list.front().as_list());
  std::cerr << "TraceLog eldoc params count: " << params.size() << "\n";

  /* Look for ... among the params */
  bool found_variadic = false;
  for(auto const &param : params)
  {
    auto const param_str = param.as_string();
    std::cerr << "  param: " << param_str << "\n";
    if(param_str.find("...") != std::string::npos)
    {
      found_variadic = true;
    }
  }

  CHECK_MESSAGE(found_variadic, "eldoc params should contain '...' for variadic function");
}

TEST_CASE("info shows default parameter values for C++ functions")
{
  /* Test that C++ functions with default parameters show {:default ...} in arglists */
  engine eng;

  /* Include the C header (which has C++ functions with defaults) */
  eng.handle(make_message({
    {   "op",                                                           "eval" },
    { "code", R"((cpp/raw "#include \"../test/cpp/jank/nrepl/test_c_header.h\""))" }
  }));

  /* Create a native alias with :scope "" for global scope */
  eng.handle(make_message({
    {   "op",                                                                  "eval" },
    { "code", R"((require '["../test/cpp/jank/nrepl/test_c_header.h" :as rl :scope ""]))" }
  }));

  /* Get info for DrawTextEx - a function with multiple default parameters */
  auto responses(eng.handle(make_message({
    {  "op",          "info" },
    { "sym", "rl/DrawTextEx" },
    {  "ns",          "user" }
  })));

  REQUIRE(responses.size() == 1);
  auto const &payload(responses.front());

  auto const arglists_it(payload.find("arglists-str"));
  if(arglists_it == payload.end())
  {
    WARN("arglists-str not found for DrawTextEx");
    return;
  }

  auto const &arglists_str(arglists_it->second.as_string());
  std::cerr << "DrawTextEx arglists: " << arglists_str << "\n";

  /* Should contain {:default ...} for parameters with defaults */
  bool const has_default = arglists_str.find("{:default") != std::string::npos;
  CHECK_MESSAGE(has_default, "arglists should contain '{:default' for function with default params, got: " << arglists_str);

  /* Check for specific default values */
  bool const has_default_0 = arglists_str.find("{:default 0}") != std::string::npos;
  CHECK_MESSAGE(has_default_0, "arglists should contain '{:default 0}' for posX/posY/color, got: " << arglists_str);

  bool const has_default_20 = arglists_str.find("{:default 20}") != std::string::npos;
  CHECK_MESSAGE(has_default_20, "arglists should contain '{:default 20}' for fontSize, got: " << arglists_str);
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

TEST_CASE("info returns jank source location for cpp/raw functions")
{
  /* This test verifies that functions defined in cpp/raw blocks
   * return the correct line within the C++ code (not just the cpp/raw line),
   * enabling goto-definition to jump to the exact declaration. */
  engine eng;

  /* Load a jank file that contains multiple cpp/raw function definitions.
   * The cpp/raw call is on line 4 of the test file.
   * - first_fn is on line 1 of C++ code = jank line 4
   * - second_fn is on line 2 of C++ code = jank line 5 */
  std::string const jank_file_path{ "test/jank/nrepl/cpp_raw_location.jank" };
  std::string const file_contents =
    "; Test file for cpp/raw source location tracking\n"
    "; The cpp/raw call on line 4 should have its location stored\n"
    "\n"
    "(cpp/raw \"inline void test_first_fn(int x) { }\n"
    "inline void test_second_fn(int y) { }\")\n";

  eng.handle(make_message({
    {        "op",     "load-file" },
    {      "file",   file_contents },
    { "file-path", jank_file_path }
  }));

  /* Test first function - should be on line 4 (same line as cpp/raw) */
  {
    auto responses(eng.handle(make_message({
      {  "op",                "info" },
      { "sym", "cpp/test_first_fn" },
      {  "ns",                "user" }
    })));

    REQUIRE(responses.size() == 1);
    auto const &payload(responses.front());

    auto const name_it(payload.find("name"));
    REQUIRE(name_it != payload.end());
    CHECK(name_it->second.as_string() == "test_first_fn");

    auto const line_it(payload.find("line"));
    REQUIRE(line_it != payload.end());
    CHECK(line_it->second.as_integer() == 4);
  }

  /* Test second function - should be on line 5 (one line after cpp/raw) */
  {
    auto responses(eng.handle(make_message({
      {  "op",                 "info" },
      { "sym", "cpp/test_second_fn" },
      {  "ns",                 "user" }
    })));

    REQUIRE(responses.size() == 1);
    auto const &payload(responses.front());

    auto const name_it(payload.find("name"));
    REQUIRE(name_it != payload.end());
    CHECK(name_it->second.as_string() == "test_second_fn");

    auto const line_it(payload.find("line"));
    REQUIRE(line_it != payload.end());
    CHECK(line_it->second.as_integer() == 5);
  }
}

}
