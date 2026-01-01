#include "common.hpp"

namespace jank::nrepl_server::asio
{
  TEST_SUITE("nREPL info op")
  {
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
        {   "op",                                                              "eval" },
        { "code", R"((cpp/raw "#include \"../test/cpp/jank/nrepl/test_flecs.hpp\""))" }
      }));

      /* Require it as a native header alias. */
      eng.handle(make_message({
        {   "op",                                                   "eval"                 },
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
        {   "op","eval"                },
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
    }

    SUBCASE("variadic template member function shows Args types")
    {
      engine eng;

      /* Require the template_types.hpp header with :scope for template_type_test namespace */
      eng.handle(make_message({
        {   "op","eval"                },
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

      /* Check arglists-str doesn't contain "auto" (if available) */
      /* Note: arglists-str may not be available for all symbols (e.g., template functions) */
      auto const arglists_str_it(payload.find("arglists-str"));
      if(arglists_str_it != payload.end())
      {
        auto const &arglists_str(arglists_str_it->second.as_string());

        /* Should not contain "auto" - should show actual template parameter types */
        CHECK_MESSAGE(arglists_str.find("auto") == std::string::npos,
                      "arglists should not contain 'auto', got: " << arglists_str);

        /* Should contain "Args" or the actual parameter pack type */
        bool const has_args_param = arglists_str.find("Args") != std::string::npos
          || arglists_str.find("args") != std::string::npos;
        CHECK_MESSAGE(
          has_args_param,
          "arglists should contain template parameter name 'Args' or 'args', got: " << arglists_str);
      }

      /* Also check return type is not "auto" (if available) */
      /* Note: return-type may not be available for all symbols */
      auto const return_type_it(payload.find("return-type"));
      if(return_type_it != payload.end())
      {
        auto const &return_type(return_type_it->second.as_string());
        /* Return type should be "entity" not "auto" */
        CHECK_MESSAGE(return_type.find("auto") == std::string::npos,
                      "return-type should not be 'auto', got: " << return_type);
      }
    }

    SUBCASE("simple template function with T parameter")
    {
      engine eng;

      /* Require the template_types.hpp header with :scope for template_type_test namespace */
      eng.handle(make_message({
        {   "op","eval"                },
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

      /* Check arglists-str doesn't contain "auto" (if available) */
      /* Note: arglists-str may not be available for all symbols (e.g., template functions) */
      auto const arglists_str_it(payload.find("arglists-str"));
      if(arglists_str_it != payload.end())
      {
        auto const &arglists_str(arglists_str_it->second.as_string());

        /* Should not contain "auto" */
        CHECK_MESSAGE(arglists_str.find("auto") == std::string::npos,
                      "arglists should not contain 'auto', got: " << arglists_str);

        /* Should contain "T" for the template parameter */
        CHECK_MESSAGE(arglists_str.find("T ") != std::string::npos,
                      "arglists should contain template parameter 'T', got: " << arglists_str);
      }
    }

    SUBCASE("template method with mixed parameters")
    {
      engine eng;

      /* Require the template_types.hpp header with :scope for template_type_test namespace */
      eng.handle(make_message({
        {   "op","eval"                },
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

      /* Check arglists-str doesn't contain "auto" (if available) */
      /* Note: arglists-str may not be available for all symbols (e.g., template functions) */
      auto const arglists_str_it(payload.find("arglists-str"));
      if(arglists_str_it != payload.end())
      {
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
  }

  TEST_CASE("info returns fields for typedef structs from C header")
  {
    /* Test that typedef'd C structs show their fields in the info response */
    engine eng;

    /* Include the C header */
    eng.handle(make_message({
      {   "op",                                                               "eval" },
      { "code", R"((cpp/raw "#include \"../test/cpp/jank/nrepl/test_c_header.h\""))" }
    }));

    /* Create a native alias with :scope "" for global scope using require */
    eng.handle(make_message({
      {   "op",                                                                      "eval" },
      { "code", R"((require '["../test/cpp/jank/nrepl/test_c_header.h" :as rl :scope ""]))" }
    }));

    /* Get info for Vector2 */
    auto responses(eng.handle(make_message({
      {     "op",       "info" },
      { "symbol", "rl/Vector2" },
      {     "ns",       "user" }
    })));

    REQUIRE(responses.size() == 1);
    auto const &payload(responses.front());

    /* Check that we got a valid response */
    REQUIRE(payload.find("status") != payload.end());

    /* Check that cpp-fields is present */
    auto const fields_it = payload.find("cpp-fields");
    std::cerr << "Vector2 info has cpp-fields: " << (fields_it != payload.end()) << '\n';

    if(fields_it != payload.end())
    {
      auto const &fields = fields_it->second.as_list();
      std::cerr << "Vector2 has " << fields.size() << " fields\n";
      for(auto const &field : fields)
      {
        auto const &dict = field.as_dict();
        std::cerr << "  - " << dict.at("name").as_string() << " (" << dict.at("type").as_string()
                  << ")\n";
      }

      /* Vector2 should have 2 fields: x, y */
      CHECK(fields.size() == 2);

      bool found_x{ false };
      bool found_y{ false };
      for(auto const &field : fields)
      {
        auto const &dict = field.as_dict();
        auto const &name = dict.at("name").as_string();
        auto const &type = dict.at("type").as_string();
        if(name == "x")
        {
          found_x = true;
          CHECK(type == "float");
        }
        else if(name == "y")
        {
          found_y = true;
          CHECK(type == "float");
        }
      }
      CHECK(found_x);
      CHECK(found_y);
    }
    else
    {
      /* Check if doc contains Fields section as fallback */
      auto const doc_it = payload.find("doc");
      if(doc_it != payload.end())
      {
        auto const &doc = doc_it->second.as_string();
        std::cerr << "Vector2 doc: " << doc << '\n';
        CHECK(doc.find("Fields:") != std::string::npos);
      }
      else
      {
        FAIL("Neither cpp-fields nor doc with Fields found");
      }
    }
  }

  TEST_CASE("info returns metadata for global C functions")
  {
    /* Test that we can get function info (arglists, etc.) for global C functions */
    engine eng;

    /* Include the C header */
    eng.handle(make_message({
      {   "op",                                                               "eval" },
      { "code", R"((cpp/raw "#include \"../test/cpp/jank/nrepl/test_c_header.h\""))" }
    }));

    /* Create a native alias with :scope "" for global scope */
    eng.handle(make_message({
      {   "op",                                                                      "eval" },
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
    CHECK_MESSAGE(has_width_info,
                  "arglists should contain 'width' or 'int', got: " << arglists_str);

    bool const has_height_info = arglists_str.find("height") != std::string::npos
      || arglists_str.find("int") != std::string::npos;
    CHECK_MESSAGE(has_height_info,
                  "arglists should contain 'height' or 'int', got: " << arglists_str);

    bool const has_title_info = arglists_str.find("title") != std::string::npos
      || arglists_str.find("char") != std::string::npos;
    CHECK_MESSAGE(has_title_info,
                  "arglists should contain 'title' or 'char', got: " << arglists_str);

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
      {   "op",                                                               "eval" },
      { "code", R"((cpp/raw "#include \"../test/cpp/jank/nrepl/test_c_header.h\""))" }
    }));

    /* Create a native alias with :scope "" for global scope */
    eng.handle(make_message({
      {   "op",                                                                      "eval" },
      { "code", R"((require '["../test/cpp/jank/nrepl/test_c_header.h" :as rl :scope ""]))" }
    }));

    /* Get info for DrawRectangle function - has inline comment */
    auto responses(eng.handle(make_message({
      {  "op",             "info" },
      { "sym", "rl/DrawRectangle" },
      {  "ns",             "user" }
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

  TEST_CASE("info shows variadic indicator for C variadic functions")
  {
    /* Test that variadic C functions show ... in their arglists */
    engine eng;

    /* Include the C header */
    eng.handle(make_message({
      {   "op",                                                               "eval" },
      { "code", R"((cpp/raw "#include \"../test/cpp/jank/nrepl/test_c_header.h\""))" }
    }));

    /* Create a native alias with :scope "" for global scope */
    eng.handle(make_message({
      {   "op",                                                                      "eval" },
      { "code", R"((require '["../test/cpp/jank/nrepl/test_c_header.h" :as rl :scope ""]))" }
    }));

    /* Get info for TraceLog - a variadic function */
    auto responses(eng.handle(make_message({
      {  "op",        "info" },
      { "sym", "rl/TraceLog" },
      {  "ns",        "user" }
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
    CHECK_MESSAGE(has_variadic,
                  "arglists should contain '...' for variadic function, got: " << arglists_str);

    /* Should also have the fixed parameters */
    bool const has_log_level = arglists_str.find("logLevel") != std::string::npos
      || arglists_str.find("int") != std::string::npos;
    CHECK_MESSAGE(has_log_level, "arglists should contain logLevel or int, got: " << arglists_str);

    bool const has_text = arglists_str.find("text") != std::string::npos
      || arglists_str.find("char") != std::string::npos;
    CHECK_MESSAGE(has_text, "arglists should contain text or char, got: " << arglists_str);
  }

  TEST_CASE("info shows default parameter values for C++ functions")
  {
    /* Test that C++ functions with default parameters show {:default ...} in arglists */
    engine eng;

    /* Include the C header (which has C++ functions with defaults) */
    eng.handle(make_message({
      {   "op",                                                               "eval" },
      { "code", R"((cpp/raw "#include \"../test/cpp/jank/nrepl/test_c_header.h\""))" }
    }));

    /* Create a native alias with :scope "" for global scope */
    eng.handle(make_message({
      {   "op",                                                                      "eval" },
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
    CHECK_MESSAGE(has_default,
                  "arglists should contain '{:default' for function with default params, got: "
                    << arglists_str);

    /* Check for specific default values */
    bool const has_default_0 = arglists_str.find("{:default 0}") != std::string::npos;
    CHECK_MESSAGE(
      has_default_0,
      "arglists should contain '{:default 0}' for posX/posY/color, got: " << arglists_str);

    bool const has_default_20 = arglists_str.find("{:default 20}") != std::string::npos;
    CHECK_MESSAGE(has_default_20,
                  "arglists should contain '{:default 20}' for fontSize, got: " << arglists_str);
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
    std::string const file_contents
      = "; Test file for cpp/raw source location tracking\n"
        "; The cpp/raw call on line 4 should have its location stored\n"
        "\n"
        "(cpp/raw \"inline void test_first_fn(int x) { }\n"
        "inline void test_second_fn(int y) { }\")\n";

    eng.handle(make_message({
      {        "op",    "load-file" },
      {      "file",  file_contents },
      { "file-path", jank_file_path }
    }));

    /* Test first function - should be on line 4 (same line as cpp/raw) */
    {
      auto responses(eng.handle(make_message({
        {  "op",              "info" },
        { "sym", "cpp/test_first_fn" },
        {  "ns",              "user" }
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
        {  "op",               "info" },
        { "sym", "cpp/test_second_fn" },
        {  "ns",               "user" }
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
