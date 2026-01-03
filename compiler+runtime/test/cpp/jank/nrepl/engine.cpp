#include "common.hpp"

namespace jank::nrepl_server::asio
{
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
