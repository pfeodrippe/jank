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
      REQUIRE_FALSE(completions.empty());

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
      REQUIRE_FALSE(completions.empty());

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
          CHECK(dict.at("ns").as_string() == "str-native");

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
          CHECK(signature.find("s") != std::string::npos);
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
      CHECK(first_param.find("[") != std::string::npos);
      CHECK(first_param.find("s") != std::string::npos);
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
        if(ns == "str-native")
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

      CHECK(payload.at("name").as_string() == "reverse");
      CHECK(payload.at("ns").as_string() == "str-native");

      auto const arglists_it(payload.find("arglists"));
      REQUIRE(arglists_it != payload.end());
      auto const &arglists(arglists_it->second.as_list());
      REQUIRE_FALSE(arglists.empty());
      auto const &signature(arglists.front().as_string());
      std::cerr << "Info arglists signature: '" << signature << "'\n";
      // Should NOT be just "coll" - should have type and parameter info
      CHECK(signature != "coll");
      CHECK(signature.find("s") != std::string::npos);
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
      CHECK(payload.at("return-type").as_string() == "i32");

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
      // Should be [[i32 lhs] [i32 rhs]] format
      CHECK(signature.find("[[i32 lhs] [i32 rhs]]") != std::string::npos);
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
      CHECK(cpp_signature.at("return-type").as_string() == "i32");
      auto const &args(cpp_signature.at("args").as_list());
      REQUIRE(args.size() == 2);
      auto const &first_arg(args.front().as_dict());
      CHECK(first_arg.at("index").as_integer() == 0);
      CHECK(first_arg.at("type").as_string() == "i32");
      auto const first_arg_name(first_arg.at("name").as_string());
      INFO("first_arg_name: " << first_arg_name);
      CHECK(first_arg_name == "lhs");
      auto const &second_arg(args.back().as_dict());
      CHECK(second_arg.at("index").as_integer() == 1);
      CHECK(second_arg.at("type").as_string() == "i32");
      auto const second_arg_name(second_arg.at("name").as_string());
      INFO("second_arg_name: " << second_arg_name);
      CHECK(second_arg_name == "rhs");

      auto const statuses(extract_status(payload));
      CHECK(std::ranges::find(statuses, "done") != statuses.end());

      CHECK(payload.at("docstring").as_string() == "i32");
      CHECK(payload.at("doc").as_string() == payload.at("docstring").as_string());
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
      CHECK(payload.at("name").as_string() == "cpp_info_struct");
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
  }
}
