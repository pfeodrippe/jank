#include <algorithm>
#include <array>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <string_view>
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

    constexpr std::array<std::string_view, 18> expected_ops{
      "clone",     "describe",      "ls-sessions",    "close",
      "eval",      "load-file",     "completions",    "complete",
      "lookup",    "info",          "eldoc",          "forward-system-output",
      "interrupt", "ls-middleware", "add-middleware", "swap-middleware",
      "stdin",     "caught"
    };

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
      auto const &info(payload.at("info").as_dict());
      CHECK(info.at("name").as_string() == "sample-fn");
      CHECK(info.at("ns").as_string() == "user");
      auto const doc_it(info.find("doc"));
      REQUIRE(doc_it != info.end());
      CHECK(doc_it->second.as_string().find("demo doc") != std::string::npos);
      auto const &arglists(info.at("arglists").as_list());
      REQUIRE(arglists.size() == 2);
      auto const statuses(extract_status(payload));
      auto const done(std::ranges::find(statuses, "done"));
      CHECK(done != statuses.end());
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
      CHECK(eldoc.front().as_string().find("sample-fn") != std::string::npos);
      CHECK(payload.at("ns").as_string() == "user");
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
      auto const &info_dict(info_payload.at("info").as_dict());
      CHECK(info_dict.at("name").as_string() == "sample-fn");
      CHECK(info_dict.at("ns").as_string() == "cpp_raw_inline.core");
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
      CHECK(eldoc_list.front().as_string().find("sample-fn") != std::string::npos);
      auto const eldoc_status(extract_status(eldoc_payload));
      CHECK(std::ranges::find(eldoc_status, "done") != eldoc_status.end());
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
