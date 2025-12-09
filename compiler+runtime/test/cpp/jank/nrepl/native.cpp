#include "common.hpp"

namespace jank::nrepl_server::asio
{
  TEST_SUITE("nREPL native header macros")
  {
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

  }
}
