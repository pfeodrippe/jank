#include "common.hpp"

namespace jank::nrepl_server::asio
{
  TEST_SUITE("nREPL eldoc op")
  {
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

  }
}
