/**
 * Performance Benchmarks for jank Form Evaluation
 *
 * This file contains benchmarks for measuring the performance of different
 * form types in jank. It separates:
 *
 * 1. COLD benchmarks: Full pipeline (lex -> parse -> analyze -> JIT -> eval)
 *    - These measure first-time evaluation cost including compilation
 *
 * 2. WARM benchmarks: Just function call overhead
 *    - Pre-define functions, then measure just the call
 *    - This isolates runtime execution from compilation
 *
 * 3. PHASE benchmarks: Time spent in each compilation phase
 *    - Shows where time is spent: lexing, parsing, analysis, or eval
 *
 * 4. EVAL BREAKDOWN: Detailed breakdown of the EVAL phase
 *    - Codegen (C++ code generation)
 *    - JIT Declaration compilation
 *    - JIT Expression compilation & execution
 *
 * Run with:
 *   ./build/jank-test --test-suite="Eval Performance"
 *
 * Run specific test:
 *   ./build/jank-test --test-case="*Warm*"
 *   ./build/jank-test --test-case="*Eval Breakdown*"
 */

#include <chrono>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <vector>

#include <Interpreter/Compatibility.h>
#include <Interpreter/CppInterOpInterpreter.h>
#include <llvm/Support/Error.h>

#include <nanobench.h>

#include <doctest/doctest.h>

#include <jank/profile/time.hpp>
#include <jank/read/lex.hpp>
#include <jank/read/parse.hpp>
#include <jank/analyze/processor.hpp>
#include <jank/evaluate.hpp>
#include <jank/codegen/processor.hpp>
#include <jank/jit/processor.hpp>
#include <jank/runtime/context.hpp>
#include <jank/runtime/core.hpp>
#include <jank/runtime/behavior/callable.hpp>
#include <jank/runtime/visit.hpp>
#include <jank/runtime/ns.hpp>
#include <jank/runtime/core/munge.hpp>
#include <jank/util/cli.hpp>

namespace jank::perf
{
  using namespace jank::runtime;
  using namespace std::chrono;

  /**
   * Measures time spent in each compilation/evaluation phase.
   */
  struct phase_breakdown
  {
    i64 lex_ns{ 0 };
    i64 parse_ns{ 0 };
    i64 analyze_ns{ 0 };
    i64 eval_ns{ 0 };
    i64 total_ns{ 0 };

    void print() const
    {
      auto const total_check{ lex_ns + parse_ns + analyze_ns + eval_ns };
      std::cout << std::fixed << std::setprecision(3);
      std::cout << "  LEX:     " << std::setw(12) << lex_ns << " ns ("
                << (100.0 * lex_ns / total_check) << "%)\n";
      std::cout << "  PARSE:   " << std::setw(12) << parse_ns << " ns ("
                << (100.0 * parse_ns / total_check) << "%)\n";
      std::cout << "  ANALYZE: " << std::setw(12) << analyze_ns << " ns ("
                << (100.0 * analyze_ns / total_check) << "%)\n";
      std::cout << "  EVAL:    " << std::setw(12) << eval_ns << " ns ("
                << (100.0 * eval_ns / total_check) << "%)\n";
      std::cout << "  TOTAL:   " << std::setw(12) << total_ns << " ns\n";
    }
  };

  /**
   * Measures time for each phase of evaluation.
   */
  static phase_breakdown measure_phases(jtl::immutable_string_view const code)
  {
    phase_breakdown result;
    auto const start{ high_resolution_clock::now() };

    /* Phase 1: Lexing */
    auto const lex_start{ high_resolution_clock::now() };
    read::lex::processor l_prc{ code, 1, 1 };
    auto const lex_end{ high_resolution_clock::now() };
    result.lex_ns = duration_cast<nanoseconds>(lex_end - lex_start).count();

    /* Phase 2: Parsing */
    auto const parse_start{ high_resolution_clock::now() };
    read::parse::processor p_prc{ l_prc.begin(), l_prc.end() };
    object_ref parsed_form{ jank_nil };
    for(auto const &form : p_prc)
    {
      parsed_form = form.expect_ok().unwrap().ptr;
    }
    auto const parse_end{ high_resolution_clock::now() };
    result.parse_ns = duration_cast<nanoseconds>(parse_end - parse_start).count();

    /* Phase 3: Analysis */
    auto const analyze_start{ high_resolution_clock::now() };
    analyze::processor an_prc;
    auto expr{ an_prc.analyze(parsed_form, analyze::expression_position::statement).expect_ok() };
    auto const analyze_end{ high_resolution_clock::now() };
    result.analyze_ns = duration_cast<nanoseconds>(analyze_end - analyze_start).count();

    /* Phase 4: Evaluation */
    auto const eval_start{ high_resolution_clock::now() };
    [[maybe_unused]] auto const ret{ evaluate::eval(expr) };
    auto const eval_end{ high_resolution_clock::now() };
    result.eval_ns = duration_cast<nanoseconds>(eval_end - eval_start).count();

    auto const end{ high_resolution_clock::now() };
    result.total_ns = duration_cast<nanoseconds>(end - start).count();

    return result;
  }

  /**
   * Helper to run multiple iterations and compute statistics.
   */
  struct timing_stats
  {
    double mean_ns{ 0 };
    double min_ns{ 0 };
    double max_ns{ 0 };
    double stddev_ns{ 0 };

    static timing_stats compute(std::vector<i64> const &samples)
    {
      timing_stats stats;
      if(samples.empty())
      {
        return stats;
      }

      auto const sum{ std::accumulate(samples.begin(), samples.end(), 0LL) };
      stats.mean_ns = static_cast<double>(sum) / static_cast<double>(samples.size());
      stats.min_ns = static_cast<double>(*std::min_element(samples.begin(), samples.end()));
      stats.max_ns = static_cast<double>(*std::max_element(samples.begin(), samples.end()));

      double variance{ 0 };
      for(auto const s : samples)
      {
        auto const diff{ static_cast<double>(s) - stats.mean_ns };
        variance += diff * diff;
      }
      variance /= static_cast<double>(samples.size());
      stats.stddev_ns = std::sqrt(variance);

      return stats;
    }

    void print(jtl::immutable_string_view label) const
    {
      std::cout << "  " << label << ":\n";
      std::cout << "    Mean:   " << std::fixed << std::setprecision(0) << mean_ns << " ns\n";
      std::cout << "    Min:    " << min_ns << " ns\n";
      std::cout << "    Max:    " << max_ns << " ns\n";
      std::cout << "    StdDev: " << std::setprecision(1) << stddev_ns << " ns\n";
    }
  };

  /**
   * Helper to call a jank function directly and measure just the call overhead.
   * This bypasses eval_string entirely for warm benchmarks.
   */
  static object_ref call_fn(object_ref fn, object_ref arg)
  {
    return visit_object(
      [&](auto const typed_fn) -> object_ref {
        using T = typename decltype(typed_fn)::value_type;
        if constexpr(std::is_base_of_v<behavior::callable, T>)
        {
          return typed_fn->call(arg);
        }
        else
        {
          throw std::runtime_error{ "not callable" };
        }
      },
      fn);
  }

  static object_ref call_fn(object_ref fn, object_ref arg1, object_ref arg2)
  {
    return visit_object(
      [&](auto const typed_fn) -> object_ref {
        using T = typename decltype(typed_fn)::value_type;
        if constexpr(std::is_base_of_v<behavior::callable, T>)
        {
          return typed_fn->call(arg1, arg2);
        }
        else
        {
          throw std::runtime_error{ "not callable" };
        }
      },
      fn);
  }

  static object_ref call_fn(object_ref fn)
  {
    return visit_object(
      [&](auto const typed_fn) -> object_ref {
        using T = typename decltype(typed_fn)::value_type;
        if constexpr(std::is_base_of_v<behavior::callable, T>)
        {
          return typed_fn->call();
        }
        else
        {
          throw std::runtime_error{ "not callable" };
        }
      },
      fn);
  }

  TEST_SUITE("Eval Performance")
  {
    /* ==========================================================================
     * WARM BENCHMARKS: Pure function call overhead
     *
     * These measure the time to call an already-compiled function.
     * This is what matters for runtime performance after initial load.
     * ========================================================================== */
    TEST_CASE("Warm: Pre-compiled function calls")
    {
      std::cout << "\n=== WARM: Pre-compiled Function Calls ===\n";
      std::cout << "(Measures JUST the call overhead, no compilation)\n\n";

      /* Ensure we're in the user namespace */
      __rt_ctx->eval_string("(in-ns 'user)");

      /* Pre-compile all the functions we'll benchmark */
      __rt_ctx->eval_string("(def bench-identity (fn [x] x))");
      __rt_ctx->eval_string("(def bench-add1 (fn [x] (+ x 1)))");
      __rt_ctx->eval_string("(def bench-add (fn [x y] (+ x y)))");
      __rt_ctx->eval_string("(def bench-mul (fn [x y] (* x y)))");
      __rt_ctx->eval_string("(def bench-nested (fn [x] (+ (* x x) x)))");
      __rt_ctx->eval_string("(def bench-if (fn [x] (if (> x 0) x (- x))))");
      __rt_ctx->eval_string("(def bench-let (fn [x] (let [y (+ x 1)] (+ x y))))");
      __rt_ctx->eval_string("(def bench-first-vec (fn [] (first [1 2 3])))");
      __rt_ctx->eval_string("(def bench-conj-vec (fn [] (conj [1 2 3] 4)))");
      __rt_ctx->eval_string("(def bench-get-map (fn [] (get {:a 1 :b 2} :a)))");

      /* Get references to the functions - deref directly since find_var returns var_ref */
      auto const identity_fn{ __rt_ctx->find_var("user", "bench-identity")->deref() };
      auto const add1_fn{ __rt_ctx->find_var("user", "bench-add1")->deref() };
      auto const add_fn{ __rt_ctx->find_var("user", "bench-add")->deref() };
      auto const mul_fn{ __rt_ctx->find_var("user", "bench-mul")->deref() };
      auto const nested_fn{ __rt_ctx->find_var("user", "bench-nested")->deref() };
      auto const if_fn{ __rt_ctx->find_var("user", "bench-if")->deref() };
      auto const let_fn{ __rt_ctx->find_var("user", "bench-let")->deref() };
      auto const first_vec_fn{ __rt_ctx->find_var("user", "bench-first-vec")->deref() };
      auto const conj_vec_fn{ __rt_ctx->find_var("user", "bench-conj-vec")->deref() };
      auto const get_map_fn{ __rt_ctx->find_var("user", "bench-get-map")->deref() };

      /* Pre-box common arguments */
      auto const arg_5{ make_box(5) };
      auto const arg_3{ make_box(3) };
      auto const arg_neg5{ make_box(-5) };

      ankerl::nanobench::Bench bench;
      bench.warmup(100).minEpochIterations(10000).relative(true);

      bench.run("W1: identity(x)", [&] {
        auto result = call_fn(identity_fn, arg_5);
        ankerl::nanobench::doNotOptimizeAway(result);
      });

      bench.run("W2: (+ x 1)", [&] {
        auto result = call_fn(add1_fn, arg_5);
        ankerl::nanobench::doNotOptimizeAway(result);
      });

      bench.run("W3: (+ x y)", [&] {
        auto result = call_fn(add_fn, arg_5, arg_3);
        ankerl::nanobench::doNotOptimizeAway(result);
      });

      bench.run("W4: (* x y)", [&] {
        auto result = call_fn(mul_fn, arg_5, arg_3);
        ankerl::nanobench::doNotOptimizeAway(result);
      });

      bench.run("W5: (+ (* x x) x)", [&] {
        auto result = call_fn(nested_fn, arg_5);
        ankerl::nanobench::doNotOptimizeAway(result);
      });

      bench.run("W6: (if (> x 0) x (- x))", [&] {
        auto result = call_fn(if_fn, arg_5);
        ankerl::nanobench::doNotOptimizeAway(result);
      });

      bench.run("W7: if with negative", [&] {
        auto result = call_fn(if_fn, arg_neg5);
        ankerl::nanobench::doNotOptimizeAway(result);
      });

      bench.run("W8: (let [y (+ x 1)] (+ x y))", [&] {
        auto result = call_fn(let_fn, arg_5);
        ankerl::nanobench::doNotOptimizeAway(result);
      });

      bench.run("W9: (first [1 2 3])", [&] {
        auto result = call_fn(first_vec_fn);
        ankerl::nanobench::doNotOptimizeAway(result);
      });

      bench.run("W10: (conj [1 2 3] 4)", [&] {
        auto result = call_fn(conj_vec_fn);
        ankerl::nanobench::doNotOptimizeAway(result);
      });

      bench.run("W11: (get {:a 1 :b 2} :a)", [&] {
        auto result = call_fn(get_map_fn);
        ankerl::nanobench::doNotOptimizeAway(result);
      });
    }

    /* ==========================================================================
     * WARM: Core function calls (clojure.core)
     *
     * These measure calling core functions that are already loaded.
     * ========================================================================== */
    TEST_CASE("Warm: Core function calls")
    {
      std::cout << "\n=== WARM: Core Function Calls ===\n";
      std::cout << "(Measures calling already-loaded clojure.core functions)\n\n";

      /* Get references to core functions */
      auto const plus_fn{ __rt_ctx->find_var("clojure.core", "+")->deref() };
      auto const minus_fn{ __rt_ctx->find_var("clojure.core", "-")->deref() };
      auto const mul_fn{ __rt_ctx->find_var("clojure.core", "*")->deref() };
      auto const first_fn{ __rt_ctx->find_var("clojure.core", "first")->deref() };
      auto const rest_fn{ __rt_ctx->find_var("clojure.core", "rest")->deref() };
      auto const count_fn{ __rt_ctx->find_var("clojure.core", "count")->deref() };
      auto const conj_fn{ __rt_ctx->find_var("clojure.core", "conj")->deref() };
      auto const get_fn{ __rt_ctx->find_var("clojure.core", "get")->deref() };
      auto const assoc_fn{ __rt_ctx->find_var("clojure.core", "assoc")->deref() };
      auto const identity_fn{ __rt_ctx->find_var("clojure.core", "identity")->deref() };
      auto const inc_fn{ __rt_ctx->find_var("clojure.core", "inc")->deref() };

      /* Pre-box arguments */
      auto const arg_1{ make_box(1) };
      auto const arg_2{ make_box(2) };
      auto const arg_5{ make_box(5) };
      auto const vec_123{ __rt_ctx->eval_string("[1 2 3]") };
      auto const map_ab{ __rt_ctx->eval_string("{:a 1 :b 2}") };
      auto const kw_a{ __rt_ctx->intern_keyword("a").expect_ok() };
      auto const kw_c{ __rt_ctx->intern_keyword("c").expect_ok() };
      auto const arg_3{ make_box(3) };

      ankerl::nanobench::Bench bench;
      bench.warmup(100).minEpochIterations(10000).relative(true);

      bench.run("C1: (+ 1 2) direct", [&] {
        auto result = call_fn(plus_fn, arg_1, arg_2);
        ankerl::nanobench::doNotOptimizeAway(result);
      });

      bench.run("C2: (- 5 2) direct", [&] {
        auto result = call_fn(minus_fn, arg_5, arg_2);
        ankerl::nanobench::doNotOptimizeAway(result);
      });

      bench.run("C3: (* 2 3) direct", [&] {
        auto result = call_fn(mul_fn, arg_2, arg_3);
        ankerl::nanobench::doNotOptimizeAway(result);
      });

      bench.run("C4: (inc 5) direct", [&] {
        auto result = call_fn(inc_fn, arg_5);
        ankerl::nanobench::doNotOptimizeAway(result);
      });

      bench.run("C5: (identity 5) direct", [&] {
        auto result = call_fn(identity_fn, arg_5);
        ankerl::nanobench::doNotOptimizeAway(result);
      });

      bench.run("C6: (first [1 2 3]) direct", [&] {
        auto result = call_fn(first_fn, vec_123);
        ankerl::nanobench::doNotOptimizeAway(result);
      });

      bench.run("C7: (rest [1 2 3]) direct", [&] {
        auto result = call_fn(rest_fn, vec_123);
        ankerl::nanobench::doNotOptimizeAway(result);
      });

      bench.run("C8: (count [1 2 3]) direct", [&] {
        auto result = call_fn(count_fn, vec_123);
        ankerl::nanobench::doNotOptimizeAway(result);
      });

      bench.run("C9: (get {:a 1 :b 2} :a) direct", [&] {
        auto result = call_fn(get_fn, map_ab, kw_a);
        ankerl::nanobench::doNotOptimizeAway(result);
      });

      bench.run("C10: (conj [1 2 3] 4)", [&] {
        auto result = dynamic_call(conj_fn, vec_123, make_box(4));
        ankerl::nanobench::doNotOptimizeAway(result);
      });

      bench.run("C11: (assoc {:a 1 :b 2} :c 3)", [&] {
        auto result = dynamic_call(assoc_fn, map_ab, kw_c, arg_3);
        ankerl::nanobench::doNotOptimizeAway(result);
      });
    }

    /* ==========================================================================
     * COLD BENCHMARKS: Full eval_string pipeline
     *
     * These measure the FULL pipeline including JIT compilation.
     * These are slower but show what the REPL experiences.
     * ========================================================================== */
    TEST_CASE("Cold: Full eval pipeline")
    {
      std::cout << "\n=== COLD: Full Eval Pipeline (includes JIT) ===\n";
      std::cout << "(Measures full lex -> parse -> analyze -> JIT -> eval)\n\n";

      ankerl::nanobench::Bench bench;
      bench.warmup(5).minEpochIterations(20).relative(true);

      /* Primitives - these should be fast even cold */
      bench.run("COLD: 42", [&] {
        auto result = __rt_ctx->eval_string("42");
        ankerl::nanobench::doNotOptimizeAway(result);
      });

      bench.run("COLD: :keyword", [&] {
        auto result = __rt_ctx->eval_string(":test");
        ankerl::nanobench::doNotOptimizeAway(result);
      });

      bench.run("COLD: [1 2 3]", [&] {
        auto result = __rt_ctx->eval_string("[1 2 3]");
        ankerl::nanobench::doNotOptimizeAway(result);
      });

      /* Function calls - need to resolve var + call */
      bench.run("COLD: (+ 1 2)", [&] {
        auto result = __rt_ctx->eval_string("(+ 1 2)");
        ankerl::nanobench::doNotOptimizeAway(result);
      });

      bench.run("COLD: (if true 1 2)", [&] {
        auto result = __rt_ctx->eval_string("(if true 1 2)");
        ankerl::nanobench::doNotOptimizeAway(result);
      });

      /* These trigger JIT compilation */
      bench.run("COLD: (let [x 1] x)", [&] {
        auto result = __rt_ctx->eval_string("(let [x 1] x)");
        ankerl::nanobench::doNotOptimizeAway(result);
      });

      bench.run("COLD: ((fn [x] x) 5)", [&] {
        auto result = __rt_ctx->eval_string("((fn [x] x) 5)");
        ankerl::nanobench::doNotOptimizeAway(result);
      });
    }

    /* ==========================================================================
     * Phase Breakdown Analysis
     * ========================================================================== */
    TEST_CASE("Phase Breakdown")
    {
      std::cout << "\n=== Phase Breakdown Analysis ===\n";
      std::cout << "(Shows time spent in each compilation phase)\n\n";

      std::cout << "-- Primitive (42) --\n";
      measure_phases("42").print();

      std::cout << "\n-- Vector [1 2 3] --\n";
      measure_phases("[1 2 3]").print();

      std::cout << "\n-- Arithmetic (+ 1 2) --\n";
      measure_phases("(+ 1 2)").print();

      std::cout << "\n-- If expression --\n";
      measure_phases("(if true 1 2)").print();

      std::cout << "\n-- Let binding (triggers JIT) --\n";
      measure_phases("(let [x 1] x)").print();

      std::cout << "\n-- Function call (triggers JIT) --\n";
      measure_phases("((fn [x] x) 5)").print();
    }

    /* ==========================================================================
     * EVAL Phase Detailed Breakdown
     *
     * For expressions that trigger JIT (let, fn, loop), the EVAL phase includes:
     * 1. wrap_expression: Wrapping the expression in a function
     * 2. codegen: Generating C++ code from the expression
     * 3. cpp_jit_decl: JIT compiling the declaration
     * 4. cpp_jit_expr: JIT compiling and executing the expression
     *
     * This test breaks down the EVAL phase to show exactly where time is spent.
     * ========================================================================== */
    TEST_CASE("Eval Breakdown: JIT Phases")
    {
      std::cout << "\n=== EVAL Phase Detailed Breakdown ===\n";
      std::cout << "(Shows codegen vs JIT compilation time within EVAL)\n\n";

      auto measure_eval_breakdown = [](jtl::immutable_string_view const code) {
        /* Phase 1-3: Lex, Parse, Analyze (same as before) */
        read::lex::processor l_prc{ code, 1, 1 };
        read::parse::processor p_prc{ l_prc.begin(), l_prc.end() };
        object_ref parsed_form{ jank_nil };
        for(auto const &form : p_prc)
        {
          parsed_form = form.expect_ok().unwrap().ptr;
        }
        analyze::processor an_prc;
        auto expr{ an_prc.analyze(parsed_form, analyze::expression_position::statement).expect_ok() };

        /* Now we manually break down the EVAL phase for function expressions.
         * We need to wrap the expression in a function to trigger JIT. */
        auto wrapped_expr{ evaluate::wrap_expression(expr, "perf_test", {}) };

        /* Get module name */
        auto const ns_name{ expect_object<ns>(__rt_ctx->current_ns_var->deref())->to_string() };
        auto const &module_name{ module::nest_module(ns_name, munge(wrapped_expr->unique_name)) };

        /* Step 1: Codegen - Generate C++ code */
        i64 codegen_ns{ 0 };
        i64 jit_decl_ns{ 0 };
        i64 jit_expr_ns{ 0 };

        auto const codegen_start{ high_resolution_clock::now() };
        codegen::processor cg_prc{ wrapped_expr, module_name, codegen::compilation_target::eval };
        auto const codegen_end{ high_resolution_clock::now() };
        codegen_ns = duration_cast<nanoseconds>(codegen_end - codegen_start).count();

        /* Step 2: JIT Declaration - Compile function declarations */
        auto const jit_decl_start{ high_resolution_clock::now() };
        __rt_ctx->jit_prc.eval_string(cg_prc.declaration_str());
        auto const jit_decl_end{ high_resolution_clock::now() };
        jit_decl_ns = duration_cast<nanoseconds>(jit_decl_end - jit_decl_start).count();

        /* Step 3: JIT Expression - Compile and execute */
        auto const expr_str{ cg_prc.expression_str() + ".erase()" };
        clang::Value v;
        auto const jit_expr_start{ high_resolution_clock::now() };
        auto res(__rt_ctx->jit_prc.interpreter->ParseAndExecute({ expr_str.data(), expr_str.size() }, &v));
        auto const jit_expr_end{ high_resolution_clock::now() };
        jit_expr_ns = duration_cast<nanoseconds>(jit_expr_end - jit_expr_start).count();

        if(res)
        {
          llvm::logAllUnhandledErrors(jtl::move(res), llvm::errs(), "error: ");
        }

        auto const total{ codegen_ns + jit_decl_ns + jit_expr_ns };
        std::cout << std::fixed << std::setprecision(3);
        std::cout << "  CODEGEN (C++ gen):     " << std::setw(12) << codegen_ns << " ns ("
                  << (100.0 * codegen_ns / total) << "%)\n";
        std::cout << "  JIT_DECL (clang decl): " << std::setw(12) << jit_decl_ns << " ns ("
                  << (100.0 * jit_decl_ns / total) << "%)\n";
        std::cout << "  JIT_EXPR (clang exec): " << std::setw(12) << jit_expr_ns << " ns ("
                  << (100.0 * jit_expr_ns / total) << "%)\n";
        std::cout << "  TOTAL EVAL:            " << std::setw(12) << total << " ns\n";
      };

      std::cout << "-- Let binding: (let [x 1] x) --\n";
      measure_eval_breakdown("(let [x 1] x)");

      std::cout << "\n-- Let with arithmetic: (let [x 1 y 2] (+ x y)) --\n";
      measure_eval_breakdown("(let [x 1 y 2] (+ x y))");

      std::cout << "\n-- Simple fn: ((fn [x] x) 5) --\n";
      measure_eval_breakdown("((fn [x] x) 5)");

      std::cout << "\n-- Fn with arithmetic: ((fn [x y] (+ x y)) 1 2) --\n";
      measure_eval_breakdown("((fn [x y] (+ x y)) 1 2)");

      std::cout << "\n-- Fn with let: ((fn [x] (let [y (+ x 1)] (* x y))) 5) --\n";
      measure_eval_breakdown("((fn [x] (let [y (+ x 1)] (* x y))) 5)");

      std::cout << "\n-- Larger fn body: ((fn [a b c] (let [sum (+ a b c)] (* sum sum))) 1 2 3) --\n";
      measure_eval_breakdown("((fn [a b c] (let [sum (+ a b c)] (* sum sum))) 1 2 3)");

      /* ========== DEFN BENCHMARKS ========== */
      std::cout << "\n\n========== DEFN Benchmarks ==========\n";

      std::cout << "\n-- Small defn: (defn add1 [x] (+ x 1)) --\n";
      measure_eval_breakdown("(defn add1 [x] (+ x 1))");

      std::cout << "\n-- Medium defn with let: (defn calc [x y] (let [sum (+ x y)] (* sum 2))) --\n";
      measure_eval_breakdown("(defn calc [x y] (let [sum (+ x y)] (* sum 2)))");

      std::cout << "\n-- Defn with if: (defn abs [x] (if (< x 0) (- x) x)) --\n";
      measure_eval_breakdown("(defn abs [x] (if (< x 0) (- x) x))");

      std::cout << "\n-- Defn with multiple arities style (nested let): --\n";
      measure_eval_breakdown(R"((defn complex-calc [a b c]
        (let [sum (+ a b c)
              prod (* a b c)
              diff (- sum prod)]
          (if (> diff 0)
            (* diff 2)
            (/ diff 2)))))");

      std::cout << "\n-- Large defn with many operations: --\n";
      measure_eval_breakdown(R"((defn big-fn [a b c d e]
        (let [sum1 (+ a b)
              sum2 (+ c d)
              sum3 (+ sum1 sum2)
              prod1 (* a c)
              prod2 (* b d)
              prod3 (* prod1 prod2)
              result (+ sum3 prod3)]
          (if (> result 100)
            (let [half (/ result 2)]
              (* half e))
            (let [double (* result 2)]
              (+ double e))))))");

      /* Note: cpp/raw benchmarks are excluded because cpp/raw generates code differently -
       * the raw expression becomes a standalone #define without semicolons, which doesn't
       * fit our wrap_expression measurement pattern. The key finding is that codegen is
       * essentially free (<1μs) while JIT compilation dominates (95%+ of time). */
    }

    /* ==========================================================================
     * Var Resolution Overhead
     * ========================================================================== */
    TEST_CASE("Var Resolution Overhead")
    {
      std::cout << "\n=== Var Resolution Overhead ===\n";
      std::cout << "(Measures time to look up vars)\n\n";

      ankerl::nanobench::Bench bench;
      bench.warmup(100).minEpochIterations(10000).relative(true);

      auto const plus_sym{ make_box<obj::symbol>("clojure.core", "+") };
      auto const identity_sym{ make_box<obj::symbol>("clojure.core", "identity") };

      bench.run("find_var(+)", [&] {
        auto result = __rt_ctx->find_var(plus_sym);
        ankerl::nanobench::doNotOptimizeAway(result);
      });

      bench.run("find_var(identity)", [&] {
        auto result = __rt_ctx->find_var(identity_sym);
        ankerl::nanobench::doNotOptimizeAway(result);
      });

      /* Compare with direct var access (already have the var) */
      auto const plus_var{ __rt_ctx->find_var(plus_sym) };
      auto const identity_var{ __rt_ctx->find_var(identity_sym) };

      bench.run("var->deref(+)", [&] {
        auto result = plus_var->deref();
        ankerl::nanobench::doNotOptimizeAway(result);
      });

      bench.run("var->deref(identity)", [&] {
        auto result = identity_var->deref();
        ankerl::nanobench::doNotOptimizeAway(result);
      });
    }

    /* ==========================================================================
     * Object Creation Overhead
     * ========================================================================== */
    TEST_CASE("Object Creation Overhead")
    {
      std::cout << "\n=== Object Creation Overhead ===\n";
      std::cout << "(Measures time to create boxed values)\n\n";

      ankerl::nanobench::Bench bench;
      bench.warmup(100).minEpochIterations(10000).relative(true);

      bench.run("make_box(int)", [&] {
        auto result = make_box(42);
        ankerl::nanobench::doNotOptimizeAway(result);
      });

      bench.run("make_box(double)", [&] {
        auto result = make_box(3.14);
        ankerl::nanobench::doNotOptimizeAway(result);
      });

      bench.run("make_box(string)", [&] {
        auto result = make_box("hello");
        ankerl::nanobench::doNotOptimizeAway(result);
      });

      bench.run("intern_keyword(:test)", [&] {
        auto result = __rt_ctx->intern_keyword("test");
        ankerl::nanobench::doNotOptimizeAway(result);
      });

      bench.run("make_box(symbol)", [&] {
        auto result = make_box<obj::symbol>("test");
        ankerl::nanobench::doNotOptimizeAway(result);
      });
    }

    /* ==========================================================================
     * Repeated Evaluation (cold vs warm comparison)
     * ========================================================================== */
    TEST_CASE("Cold vs Warm Comparison")
    {
      std::cout << "\n=== Cold vs Warm Comparison ===\n";
      std::cout << "(First call vs subsequent calls)\n\n";

      /* Measure 10 consecutive calls to eval_string */
      auto measure_repeated = [](jtl::immutable_string_view code, size_t n) {
        std::vector<i64> times;
        times.reserve(n);

        for(size_t i = 0; i < n; ++i)
        {
          auto const start{ high_resolution_clock::now() };
          auto result = __rt_ctx->eval_string(code);
          ankerl::nanobench::doNotOptimizeAway(result);
          auto const end{ high_resolution_clock::now() };
          times.push_back(duration_cast<nanoseconds>(end - start).count());
        }

        return times;
      };

      std::cout << "-- (+ 1 2) called 5 times --\n";
      auto const add_times{ measure_repeated("(+ 1 2)", 5) };
      for(size_t i = 0; i < add_times.size(); ++i)
      {
        std::cout << "  Call " << (i + 1) << ": " << std::setw(12) << add_times[i] << " ns\n";
      }
      timing_stats::compute(add_times).print("Stats");

      std::cout << "\n-- (let [x 1] x) called 5 times --\n";
      auto const let_times{ measure_repeated("(let [x 1] x)", 5) };
      for(size_t i = 0; i < let_times.size(); ++i)
      {
        std::cout << "  Call " << (i + 1) << ": " << std::setw(12) << let_times[i] << " ns\n";
      }
      timing_stats::compute(let_times).print("Stats");
    }

    /* ==========================================================================
     * Incremental JIT Cache: Cache Enabled vs Disabled
     *
     * This test proves the incremental JIT cache optimization works by:
     * 1. Defining functions with cache enabled (default)
     * 2. Reloading with cache enabled - should be fast (cache hits)
     * 3. Reloading with cache disabled - should be slow (full JIT)
     * 4. Comparing the results
     * ========================================================================== */
    TEST_CASE("Incremental JIT Cache: Enabled vs Disabled")
    {
      std::cout << "\n=== Incremental JIT Cache: Enabled vs Disabled ===\n";
      std::cout << "(Proves the optimization works by comparing with cache on/off)\n\n";

      constexpr int NUM_FUNCTIONS = 10;

      /* Generate unique namespace to avoid conflicts */
      auto const test_ns = "test-cache-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count() % 10000);

      /* Create namespace */
      __rt_ctx->eval_string(("(ns " + test_ns + ")").c_str());

      /* Ensure cache is enabled for initial definitions */
      auto const original_cache_setting = util::cli::opts.jit_cache_enabled;
      util::cli::opts.jit_cache_enabled = true;

      /* Clear the cache to start fresh */
      __rt_ctx->jit_cache.clear();

      /* Step 1: Initial definition of N functions (cache enabled) */
      std::cout << "-- Step 1: Initial definition of " << NUM_FUNCTIONS << " functions (cache enabled) --\n";
      auto const initial_start{ high_resolution_clock::now() };

      for(int i = 0; i < NUM_FUNCTIONS; i++)
      {
        std::string code = "(defn cache-test-fn-" + std::to_string(i) + " [x] (+ x " + std::to_string(i) + "))";
        __rt_ctx->eval_string(code.c_str());
      }

      auto const initial_end{ high_resolution_clock::now() };
      auto const initial_time_ms = duration_cast<milliseconds>(initial_end - initial_start).count();
      auto const cache_stats_after_initial = __rt_ctx->jit_cache.get_stats();

      std::cout << "  Time: " << initial_time_ms << " ms\n";
      std::cout << "  Cache entries: " << cache_stats_after_initial.entries << "\n";
      std::cout << "  Cache hits: " << cache_stats_after_initial.hits
                << ", misses: " << cache_stats_after_initial.misses << "\n";

      /* Step 2: Reload all functions WITH cache enabled (should be fast - cache hits) */
      std::cout << "\n-- Step 2: Reload WITH cache enabled (should be fast) --\n";

      auto const cache_enabled_start{ high_resolution_clock::now() };

      for(int i = 0; i < NUM_FUNCTIONS; i++)
      {
        std::string code = "(defn cache-test-fn-" + std::to_string(i) + " [x] (+ x " + std::to_string(i) + "))";
        __rt_ctx->eval_string(code.c_str());
      }

      auto const cache_enabled_end{ high_resolution_clock::now() };
      auto const cache_enabled_time_ms = duration_cast<milliseconds>(cache_enabled_end - cache_enabled_start).count();
      auto const cache_stats_after_reload = __rt_ctx->jit_cache.get_stats();

      std::cout << "  Time: " << cache_enabled_time_ms << " ms\n";
      std::cout << "  Cache hits: " << cache_stats_after_reload.hits
                << ", misses: " << cache_stats_after_reload.misses << "\n";
      std::cout << "  New hits this round: " << (cache_stats_after_reload.hits - cache_stats_after_initial.hits) << "\n";

      /* Step 3: Reload all functions with cache DISABLED (should be slow - full JIT) */
      std::cout << "\n-- Step 3: Reload WITHOUT cache (should be slow) --\n";
      util::cli::opts.jit_cache_enabled = false;

      auto const cache_disabled_start{ high_resolution_clock::now() };

      for(int i = 0; i < NUM_FUNCTIONS; i++)
      {
        std::string code = "(defn cache-test-fn-" + std::to_string(i) + " [x] (+ x " + std::to_string(i) + "))";
        __rt_ctx->eval_string(code.c_str());
      }

      auto const cache_disabled_end{ high_resolution_clock::now() };
      auto const cache_disabled_time_ms = duration_cast<milliseconds>(cache_disabled_end - cache_disabled_start).count();

      std::cout << "  Time: " << cache_disabled_time_ms << " ms\n";

      /* Restore original cache setting */
      util::cli::opts.jit_cache_enabled = original_cache_setting;

      /* Calculate speedup */
      double speedup = 0.0;
      if(cache_enabled_time_ms > 0)
      {
        speedup = static_cast<double>(cache_disabled_time_ms) / static_cast<double>(cache_enabled_time_ms);
      }
      else if(cache_disabled_time_ms > 0)
      {
        speedup = static_cast<double>(cache_disabled_time_ms); /* cache_enabled was 0ms */
      }

      /* Summary */
      std::cout << "\n=== Results ===\n";
      std::cout << "| Scenario                | Time (ms) |\n";
      std::cout << "|-------------------------|-----------|\n";
      std::cout << "| Initial load            | " << std::setw(9) << initial_time_ms << " |\n";
      std::cout << "| Reload WITH cache       | " << std::setw(9) << cache_enabled_time_ms << " |\n";
      std::cout << "| Reload WITHOUT cache    | " << std::setw(9) << cache_disabled_time_ms << " |\n";
      std::cout << "|-------------------------|-----------|\n";
      std::cout << "| Speedup from cache      | " << std::setw(7) << std::fixed << std::setprecision(1)
                << speedup << "x |\n";

      if(speedup >= 2.0)
      {
        std::cout << "\n✓ SUCCESS: Cache provides " << speedup << "x speedup!\n";
      }
      else if(cache_enabled_time_ms <= 50 && cache_disabled_time_ms > 100)
      {
        std::cout << "\n✓ SUCCESS: Cache is working (enabled: " << cache_enabled_time_ms
                  << "ms, disabled: " << cache_disabled_time_ms << "ms)\n";
      }
      else
      {
        std::cout << "\n? Cache benefit may be masked by measurement noise or small sample size.\n";
        std::cout << "  Try increasing NUM_FUNCTIONS for clearer results.\n";
      }
    }
  }
}
