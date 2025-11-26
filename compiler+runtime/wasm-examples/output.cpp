namespace clojure::set
{
  struct clojure_set_fn_8 : jank::runtime::obj::jit_function
  {
    jank::runtime::var_ref const clojure_set_identical_QMARK__9;
    jank::runtime::object_ref const clojure_set_max_5;

    clojure_set_fn_8(jank::runtime::object_ref clojure_set_max_5)
      : jank::runtime::obj::jit_function{ jank::runtime::__rt_ctx->read_string(
          "{:name \"clojure.set/clojure_set-fn-8\"}") }
      , clojure_set_identical_QMARK__9{ jank::runtime::__rt_ctx
                                          ->intern_var("clojure.set", "identical?")
                                          .expect_ok() }
      , clojure_set_max_5{ clojure_set_max_5 }
    {
    }

    jank::runtime::object_ref call(jank::runtime::object_ref const _PERC_1_SHARP_) final
    {
      using namespace jank;
      using namespace jank::runtime;
      object_ref const clojure_set_fn_8{ this };
      auto const clojure_set_call_20(
        jank::runtime::dynamic_call(clojure_set_identical_QMARK__9->deref(),
                                    clojure_set_max_5,
                                    _PERC_1_SHARP_));
      return clojure_set_call_20;
    }
  };
}

namespace clojure::set
{
  struct clojure_set_bubble_max_key_2 : jank::runtime::obj::jit_function
  {
    jank::runtime::var_ref const clojure_set_remove_7;
    jank::runtime::var_ref const clojure_set_max_key_4;
    jank::runtime::var_ref const clojure_set_cons_6;
    jank::runtime::var_ref const clojure_set_apply_3;

    clojure_set_bubble_max_key_2()
      : jank::runtime::obj::jit_function{ jank::runtime::__rt_ctx->read_string(
          "{:name \"clojure.set/bubble-max-key\"}") }
      , clojure_set_remove_7{ jank::runtime::__rt_ctx->intern_var("clojure.set", "remove")
                                .expect_ok() }
      , clojure_set_max_key_4{ jank::runtime::__rt_ctx->intern_var("clojure.set", "max-key")
                                 .expect_ok() }
      , clojure_set_cons_6{ jank::runtime::__rt_ctx->intern_var("clojure.set", "cons").expect_ok() }
      , clojure_set_apply_3{
        jank::runtime::__rt_ctx->intern_var("clojure.set", "apply").expect_ok()
      }
    {
    }

    jank::runtime::object_ref
    call(jank::runtime::object_ref const k, jank::runtime::object_ref const coll) final
    {
      using namespace jank;
      using namespace jank::runtime;
      object_ref const bubble_max_key{ this };
      object_ref clojure_set_let_16{};
      {
        auto const clojure_set_call_17(jank::runtime::dynamic_call(clojure_set_apply_3->deref(),
                                                                   clojure_set_max_key_4->deref(),
                                                                   k,
                                                                   coll));
        {
          auto &&clojure_set_max_5(clojure_set_call_17);
          auto const clojure_set_call_19(jank::runtime::dynamic_call(
            clojure_set_remove_7->deref(),
            jank::runtime::make_box<::clojure::set::clojure_set_fn_8>(clojure_set_max_5),
            coll));
          auto const clojure_set_call_18(jank::runtime::dynamic_call(clojure_set_cons_6->deref(),
                                                                     clojure_set_max_5,
                                                                     clojure_set_call_19));
          return clojure_set_call_18;
        }
      }
      return clojure_set_let_16;
    }
  };
}

// WASM AOT module registration for clojure.set$clojure_set_bubble_max_key_2
extern "C"
{
  // Module load function (compatible with standard module loading)
  void *jank_load_clojure_set_clojure_set_bubble_max_key_2()
  {
    return clojure::set::clojure_set_bubble_max_key_2{}.call().erase();
  }

  // WASM-specific init function with result capture
  jank::runtime::object_ref jank_wasm_init_clojure_set_clojure_set_bubble_max_key_2()
  {
    return clojure::set::clojure_set_bubble_max_key_2{}.call();
  }
}


