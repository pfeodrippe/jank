namespace hello
{
  struct hello__main_2 : jank::runtime::obj::jit_function
  {
    jank::runtime::var_ref const hello_println_3;
    jank::runtime::obj::persistent_string_ref const hello_const_4;

    hello__main_2()
      : jank::runtime::obj::jit_function{ jank::runtime::__rt_ctx->read_string(
          "{:name \"hello/-main\"}") }
      , hello_println_3{ jank::runtime::__rt_ctx->intern_var("hello", "println").expect_ok() }
      , hello_const_4{ jank::runtime::make_box<jank::runtime::obj::persistent_string>(
          "Hello World") }
    {
    }

    jank::runtime::object_ref call() final
    {
      using namespace jank;
      using namespace jank::runtime;
      object_ref const _main{ this };
      auto const hello_call_6(jank::runtime::dynamic_call(hello_println_3->deref(), hello_const_4));
      return hello_call_6;
    }
  };
}
