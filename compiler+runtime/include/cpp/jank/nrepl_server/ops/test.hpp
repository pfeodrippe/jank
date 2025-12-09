#pragma once

#include <chrono>

namespace jank::nrepl_server::asio
{
  inline std::vector<bencode::value::dict> engine::handle_test(message const &msg)
  {
    auto const ns_name(msg.get("ns"));
    if(ns_name.empty())
    {
      return handle_unsupported(msg, "missing-ns");
    }

    auto &session(ensure_session(msg.session()));

    /* Get the list of test names to run */
    std::vector<std::string> test_names;
    if(auto const *tests_val = msg.find_value("tests"))
    {
      if(tests_val->is_list())
      {
        for(auto const &entry : tests_val->as_list())
        {
          if(entry.is_string())
          {
            test_names.push_back(entry.as_string());
          }
        }
      }
    }

    /* Check if we should load the namespace first */
    auto const load_ns(msg.get("load?", "true") == "true");

    /* Check for fail-fast mode */
    auto const fail_fast(msg.get("fail-fast", "false") == "true");

    auto const start_time(std::chrono::steady_clock::now());

    /* Ensure clojure.test is loaded */
    try
    {
      __rt_ctx->eval_string("(require 'clojure.test)");
    }
    catch(...)
    {
      /* clojure.test might already be loaded, ignore errors */
    }

    /* Install custom assert-expr for = that captures actual values directly.
     * This is similar to what CIDER-nrepl does to provide better test reporting.
     * Instead of reporting :actual as (not (= expected actual)), we capture
     * the individual values so editors can display them properly.
     * Note: The 4-arg signature [msg form file line] matches the updated
     * clojure.test/assert-expr dispatch function. */
    try
    {
      __rt_ctx->eval_string(R"(
        (defmethod clojure.test/assert-expr '= [msg form file line]
          (let [[_ expected & actuals] form
                file-val (or file *file*)
                line-val (or line 1)]
            `(let [expected# ~expected
                   actuals# (vector ~@actuals)
                   result# (apply = expected# actuals#)]
               (if result#
                 (clojure.test/do-report {:type :pass
                                          :message ~msg
                                          :expected expected#
                                          :actual (first actuals#)
                                          :file ~file-val
                                          :line ~line-val})
                 (clojure.test/do-report {:type :fail
                                          :message ~msg
                                          :expected expected#
                                          :actual (first actuals#)
                                          :file ~file-val
                                          :line ~line-val}))
               result#)))
        (defmethod clojure.test/assert-expr 'clojure.core/= [msg form file line]
          (clojure.test/assert-expr msg (cons '= (rest form)) file line))
      )");
    }
    catch(...)
    {
      /* If this fails, tests will still work but with standard clojure.test behavior */
    }

    /* Load the target namespace if requested */
    if(load_ns)
    {
      try
      {
        std::string require_code = "(require '" + ns_name + ")";
        __rt_ctx->eval_string(jtl::immutable_string_view{ require_code.data(), require_code.size() });
      }
      catch(std::exception const &ex)
      {
        bencode::value::dict error_payload;
        if(!msg.id().empty())
        {
          error_payload.emplace("id", msg.id());
        }
        error_payload.emplace("session", session.id);
        error_payload.emplace("err", std::string{ "Failed to load namespace: " } + ex.what());
        error_payload.emplace("status", bencode::list_of_strings({ "done", "error" }));
        return { std::move(error_payload) };
      }
      catch(...)
      {
        bencode::value::dict error_payload;
        if(!msg.id().empty())
        {
          error_payload.emplace("id", msg.id());
        }
        error_payload.emplace("session", session.id);
        error_payload.emplace("err", std::string{ "Failed to load namespace" });
        error_payload.emplace("status", bencode::list_of_strings({ "done", "error" }));
        return { std::move(error_payload) };
      }
    }

    /* Build the results structure: ns -> var -> [results] */
    bencode::value::dict ns_results;
    bencode::value::dict var_results;

    std::int64_t total_pass{ 0 };
    std::int64_t total_fail{ 0 };
    std::int64_t total_error{ 0 };
    std::int64_t total_test{ 0 };

    /* If no specific tests provided, find all test vars in the namespace */
    if(test_names.empty())
    {
      std::string find_tests_code = R"(
        (let [ns-obj (the-ns ')" + ns_name + R"()
              mappings (ns-interns ns-obj)]
          (->> mappings
               vals
               (filter #(contains? (meta %) :test))
               (map #(-> % symbol name))
               vec)))";

      try
      {
        auto const test_vars(__rt_ctx->eval_string(
          jtl::immutable_string_view{ find_tests_code.data(), find_tests_code.size() }));

        if(!test_vars.is_nil())
        {
          for(auto it = runtime::fresh_seq(test_vars); !it.is_nil(); it = runtime::next_in_place(it))
          {
            auto const test_name_obj(runtime::first(it));
            if(!test_name_obj.is_nil())
            {
              test_names.push_back(to_std_string(runtime::to_string(test_name_obj)));
            }
          }
        }
      }
      catch(...)
      {
        /* Failed to enumerate tests */
      }
    }

    /* Run each specified test */
    for(auto const &test_name : test_names)
    {
      auto const var_start_time(std::chrono::steady_clock::now());

      /* Build the fully qualified symbol */
      std::string fq_var = ns_name + "/" + test_name;

      bencode::value::list test_results_list;

      try
      {
        /* Resolve the var */
        std::string resolve_code = "(resolve '" + fq_var + ")";
        auto const var_obj(__rt_ctx->eval_string(
          jtl::immutable_string_view{ resolve_code.data(), resolve_code.size() }));

        if(var_obj.is_nil())
        {
          /* Var not found - create an error result */
          bencode::value::dict result;
          result.emplace("type", "error");
          result.emplace("message", "Test var not found: " + fq_var);
          result.emplace("ns", ns_name);
          result.emplace("var", test_name);
          result.emplace("index", static_cast<std::int64_t>(0));
          test_results_list.push_back(bencode::value{ std::move(result) });
          total_error++;
          total_test++;
        }
        else
        {
          /* Run the test using clojure.test/run-test-var and capture results */
          /* First, bind a custom report function to capture results */
          std::string run_code = R"(
            (let [results (atom [])
                  old-report clojure.test/report]
              (binding [clojure.test/*report-counters* (atom clojure.test/*initial-report-counters*)
                        clojure.test/report (fn [m]
                                              (when (#{:pass :fail :error} (:type m))
                                                (swap! results conj m))
                                              (old-report m))]
                (clojure.test/test-var (resolve ')" + fq_var + R"())
                {:results @results
                 :counters @clojure.test/*report-counters*})))";

          auto const test_result(__rt_ctx->eval_string(
            jtl::immutable_string_view{ run_code.data(), run_code.size() }));

          /* Extract results from the returned map */
          auto const results_kw(__rt_ctx->intern_keyword("results").expect_ok());
          auto const counters_kw(__rt_ctx->intern_keyword("counters").expect_ok());
          auto const type_kw(__rt_ctx->intern_keyword("type").expect_ok());
          auto const message_kw(__rt_ctx->intern_keyword("message").expect_ok());
          auto const expected_kw(__rt_ctx->intern_keyword("expected").expect_ok());
          auto const actual_kw(__rt_ctx->intern_keyword("actual").expect_ok());
          auto const pass_kw(__rt_ctx->intern_keyword("pass").expect_ok());
          auto const fail_kw(__rt_ctx->intern_keyword("fail").expect_ok());
          auto const error_kw(__rt_ctx->intern_keyword("error").expect_ok());
          auto const test_kw(__rt_ctx->intern_keyword("test").expect_ok());
          auto const file_kw(__rt_ctx->intern_keyword("file").expect_ok());
          auto const line_kw(__rt_ctx->intern_keyword("line").expect_ok());

          auto const results_vec(runtime::get(test_result, results_kw));
          auto const counters(runtime::get(test_result, counters_kw));

          /* Extract counter values */
          auto const pass_count(runtime::get(counters, pass_kw));
          auto const fail_count(runtime::get(counters, fail_kw));
          auto const error_count(runtime::get(counters, error_kw));
          auto const test_count(runtime::get(counters, test_kw));

          std::int64_t var_pass = (!pass_count.is_nil()) ? runtime::to_int(pass_count) : 0;
          std::int64_t var_fail = (!fail_count.is_nil()) ? runtime::to_int(fail_count) : 0;
          std::int64_t var_error = (!error_count.is_nil()) ? runtime::to_int(error_count) : 0;
          std::int64_t var_test = (!test_count.is_nil()) ? runtime::to_int(test_count) : 0;

          total_pass += var_pass;
          total_fail += var_fail;
          total_error += var_error;
          total_test += var_test;

          /* Build result entries */
          std::int64_t index = 0;
          if(!results_vec.is_nil())
          {
            for(auto it = runtime::fresh_seq(results_vec); !it.is_nil(); it = runtime::next_in_place(it))
            {
              auto const result_map(runtime::first(it));
              bencode::value::dict result;

              auto const type_val(runtime::get(result_map, type_kw));
              std::string type_str = "pass";
              if(!type_val.is_nil())
              {
                auto const type_name(to_std_string(runtime::to_string(type_val)));
                if(type_name == ":fail")
                {
                  type_str = "fail";
                }
                else if(type_name == ":error")
                {
                  type_str = "error";
                }
              }

              result.emplace("type", type_str);
              result.emplace("ns", ns_name);
              result.emplace("var", test_name);
              result.emplace("index", index++);

              /* Add context - use "nil" string for CIDER compatibility */
              result.emplace("context", "nil");

              /* Add file if present in report */
              auto const file_val(runtime::get(result_map, file_kw));
              if(!file_val.is_nil())
              {
                result.emplace("file", to_std_string(runtime::to_string(file_val)));
              }
              else
              {
                result.emplace("file", "NO_SOURCE_FILE");
              }

              /* Add line if present in report */
              auto const line_val(runtime::get(result_map, line_kw));
              if(!line_val.is_nil())
              {
                result.emplace("line", runtime::to_int(line_val));
              }
              else
              {
                result.emplace("line", static_cast<std::int64_t>(1));
              }

              /* Add message if present */
              auto const msg_val(runtime::get(result_map, message_kw));
              if(!msg_val.is_nil())
              {
                result.emplace("message", to_std_string(runtime::to_string(msg_val)));
              }
              else
              {
                result.emplace("message", "");
              }

              /* Add expected/actual for failures */
              if(type_str == "fail" || type_str == "error")
              {
                auto const expected_val(runtime::get(result_map, expected_kw));
                auto const actual_val(runtime::get(result_map, actual_kw));
                if(!expected_val.is_nil())
                {
                  result.emplace("expected",
                                 to_std_string(runtime::to_code_string(expected_val)) + "\n");
                }
                if(!actual_val.is_nil())
                {
                  result.emplace("actual",
                                 to_std_string(runtime::to_code_string(actual_val)) + "\n");
                }
              }

              auto const var_end_time(std::chrono::steady_clock::now());
              auto const var_elapsed_ms(std::chrono::duration_cast<std::chrono::milliseconds>(
                var_end_time - var_start_time).count());

              bencode::value::dict elapsed_time;
              elapsed_time.emplace("ms", var_elapsed_ms);
              elapsed_time.emplace("humanized", "Completed in " + std::to_string(var_elapsed_ms) + " ms");
              result.emplace("elapsed-time", bencode::value{ std::move(elapsed_time) });

              test_results_list.push_back(bencode::value{ std::move(result) });
            }
          }

          /* If no individual results but test ran, add a pass result */
          if(test_results_list.empty() && var_test > 0)
          {
            bencode::value::dict result;
            result.emplace("type", "pass");
            result.emplace("ns", ns_name);
            result.emplace("var", test_name);
            result.emplace("index", static_cast<std::int64_t>(0));
            result.emplace("message", "");
            result.emplace("context", "nil");
            result.emplace("file", "NO_SOURCE_FILE");
            result.emplace("line", static_cast<std::int64_t>(1));

            auto const var_end_time(std::chrono::steady_clock::now());
            auto const var_elapsed_ms(std::chrono::duration_cast<std::chrono::milliseconds>(
              var_end_time - var_start_time).count());

            bencode::value::dict elapsed_time;
            elapsed_time.emplace("ms", var_elapsed_ms);
            elapsed_time.emplace("humanized", "Completed in " + std::to_string(var_elapsed_ms) + " ms");
            result.emplace("elapsed-time", bencode::value{ std::move(elapsed_time) });

            test_results_list.push_back(bencode::value{ std::move(result) });
          }
        }
      }
      catch(runtime::object_ref const &ex_obj)
      {
        bencode::value::dict result;
        result.emplace("type", "error");
        result.emplace("message", to_std_string(runtime::to_code_string(ex_obj)));
        result.emplace("ns", ns_name);
        result.emplace("var", test_name);
        result.emplace("index", static_cast<std::int64_t>(0));
        test_results_list.push_back(bencode::value{ std::move(result) });
        total_error++;
        total_test++;
      }
      catch(std::exception const &ex)
      {
        bencode::value::dict result;
        result.emplace("type", "error");
        result.emplace("message", std::string{ ex.what() });
        result.emplace("ns", ns_name);
        result.emplace("var", test_name);
        result.emplace("index", static_cast<std::int64_t>(0));
        test_results_list.push_back(bencode::value{ std::move(result) });
        total_error++;
        total_test++;
      }
      catch(...)
      {
        bencode::value::dict result;
        result.emplace("type", "error");
        result.emplace("message", "Unknown error");
        result.emplace("ns", ns_name);
        result.emplace("var", test_name);
        result.emplace("index", static_cast<std::int64_t>(0));
        test_results_list.push_back(bencode::value{ std::move(result) });
        total_error++;
        total_test++;
      }

      var_results.emplace(test_name, bencode::value{ std::move(test_results_list) });

      /* Check fail-fast */
      if(fail_fast && (total_fail > 0 || total_error > 0))
      {
        break;
      }
    }

    ns_results.emplace(ns_name, bencode::value{ std::move(var_results) });

    auto const end_time(std::chrono::steady_clock::now());
    auto const total_elapsed_ms(std::chrono::duration_cast<std::chrono::milliseconds>(
      end_time - start_time).count());

    /* Build the summary */
    bencode::value::dict summary;
    summary.emplace("ns", static_cast<std::int64_t>(1));
    summary.emplace("var", static_cast<std::int64_t>(test_names.size()));
    summary.emplace("test", total_test);
    summary.emplace("pass", total_pass);
    summary.emplace("fail", total_fail);
    summary.emplace("error", total_error);

    /* Build elapsed-time dict */
    bencode::value::dict elapsed_time;
    elapsed_time.emplace("ms", total_elapsed_ms);
    elapsed_time.emplace("humanized", "Completed in " + std::to_string(total_elapsed_ms) + " ms");

    /* Build the response payload */
    bencode::value::dict payload;
    if(!msg.id().empty())
    {
      payload.emplace("id", msg.id());
    }
    payload.emplace("session", session.id);
    payload.emplace("results", bencode::value{ std::move(ns_results) });
    payload.emplace("summary", bencode::value{ std::move(summary) });
    payload.emplace("elapsed-time", bencode::value{ std::move(elapsed_time) });
    payload.emplace("testing-ns", ns_name);
    payload.emplace("gen-input", bencode::value{});

    /* Add empty dicts for timing breakdowns that CIDER expects */
    payload.emplace("ns-elapsed-time", bencode::value{ bencode::value::dict{} });
    payload.emplace("var-elapsed-time", bencode::value{ bencode::value::dict{} });

    payload.emplace("status", bencode::list_of_strings({ "done" }));

    return { std::move(payload) };
  }
}
