#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

  /* NOLINTNEXTLINE(modernize-use-using) */
  typedef void *jank_object_ref;

  /* NOLINTNEXTLINE(modernize-use-using) */
  typedef char jank_bool;

  /* NOLINTNEXTLINE(modernize-use-using) */
  typedef signed char jank_i8;
  /* NOLINTNEXTLINE(modernize-use-using) */
  typedef unsigned char jank_u8;

  /* NOLINTNEXTLINE(modernize-use-using) */
  typedef short jank_i16;
  /* NOLINTNEXTLINE(modernize-use-using) */
  typedef unsigned short jank_u16;

  /* NOLINTNEXTLINE(modernize-use-using) */
  typedef int jank_i32;
  /* NOLINTNEXTLINE(modernize-use-using) */
  typedef unsigned int jank_u32;

  /* NOLINTNEXTLINE(modernize-use-using) */
  typedef long long jank_i64;
  /* NOLINTNEXTLINE(modernize-use-using) */
  typedef unsigned long long jank_u64;

  /* NOLINTNEXTLINE(modernize-use-using) */
  typedef float jank_f32;
  /* NOLINTNEXTLINE(modernize-use-using) */
  typedef double jank_f64;

  /* NOLINTNEXTLINE(modernize-use-using) */
  typedef unsigned long long jank_uptr;
  /* NOLINTNEXTLINE(modernize-use-using) */
  typedef jank_uptr jank_usize;
  /* NOLINTNEXTLINE(modernize-use-using) */
  typedef jank_u32 jank_uhash;
  /* NOLINTNEXTLINE(modernize-use-using) */
  typedef jank_u8 jank_arity_flags;
  /* NOLINTNEXTLINE(modernize-use-using) */
  typedef jank_object_ref (*jank_native_callback_invoke_fn)(void *callback,
                                                            void *context,
                                                            jank_object_ref const *args,
                                                            jank_usize arg_count);

  jank_object_ref jank_eval(jank_object_ref s);
  jank_object_ref jank_read_string(jank_object_ref s);
  jank_object_ref jank_read_string_c(char const * const s);

  jank_object_ref jank_ns_intern(jank_object_ref sym);
  jank_object_ref jank_ns_intern_c(char const * const sym);
  void jank_ns_set_symbol_counter(char const * const ns, jank_u64 const count);

  jank_object_ref jank_var_intern(jank_object_ref ns, jank_object_ref name);
  jank_object_ref jank_var_intern_c(char const * const ns, char const * const name);
  jank_object_ref jank_var_bind_root(jank_object_ref var, jank_object_ref val);
  jank_object_ref jank_var_set_dynamic(jank_object_ref var, jank_object_ref dynamic);

  jank_object_ref jank_keyword_intern(jank_object_ref ns, jank_object_ref name);

  jank_object_ref jank_deref(jank_object_ref o);

  jank_object_ref jank_call0(jank_object_ref f);
  jank_object_ref jank_call1(jank_object_ref f, jank_object_ref a1);
  jank_object_ref jank_call2(jank_object_ref f, jank_object_ref a1, jank_object_ref a2);
  jank_object_ref
  jank_call3(jank_object_ref f, jank_object_ref a1, jank_object_ref a2, jank_object_ref a3);
  jank_object_ref jank_call4(jank_object_ref f,
                             jank_object_ref a1,
                             jank_object_ref a2,
                             jank_object_ref a3,
                             jank_object_ref a4);
  jank_object_ref jank_call5(jank_object_ref f,
                             jank_object_ref a1,
                             jank_object_ref a2,
                             jank_object_ref a3,
                             jank_object_ref a4,
                             jank_object_ref a5);
  jank_object_ref jank_call6(jank_object_ref f,
                             jank_object_ref a1,
                             jank_object_ref a2,
                             jank_object_ref a3,
                             jank_object_ref a4,
                             jank_object_ref a5,
                             jank_object_ref a6);
  jank_object_ref jank_call7(jank_object_ref f,
                             jank_object_ref a1,
                             jank_object_ref a2,
                             jank_object_ref a3,
                             jank_object_ref a4,
                             jank_object_ref a5,
                             jank_object_ref a6,
                             jank_object_ref a7);
  jank_object_ref jank_call8(jank_object_ref f,
                             jank_object_ref a1,
                             jank_object_ref a2,
                             jank_object_ref a3,
                             jank_object_ref a4,
                             jank_object_ref a5,
                             jank_object_ref a6,
                             jank_object_ref a7,
                             jank_object_ref a8);
  jank_object_ref jank_call9(jank_object_ref f,
                             jank_object_ref a1,
                             jank_object_ref a2,
                             jank_object_ref a3,
                             jank_object_ref a4,
                             jank_object_ref a5,
                             jank_object_ref a6,
                             jank_object_ref a7,
                             jank_object_ref a8,
                             jank_object_ref a9);
  jank_object_ref jank_call10(jank_object_ref f,
                              jank_object_ref a1,
                              jank_object_ref a2,
                              jank_object_ref a3,
                              jank_object_ref a4,
                              jank_object_ref a5,
                              jank_object_ref a6,
                              jank_object_ref a7,
                              jank_object_ref a8,
                              jank_object_ref a9,
                              jank_object_ref a10);
  jank_object_ref jank_call11(jank_object_ref f,
                              jank_object_ref a1,
                              jank_object_ref a2,
                              jank_object_ref a3,
                              jank_object_ref a4,
                              jank_object_ref a5,
                              jank_object_ref a6,
                              jank_object_ref a7,
                              jank_object_ref a8,
                              jank_object_ref a9,
                              jank_object_ref a10,
                              jank_object_ref rest);

  jank_object_ref jank_const_nil();
  jank_object_ref jank_const_true();
  jank_object_ref jank_const_false();
  jank_object_ref jank_integer_create(jank_i64 i);
  jank_object_ref jank_big_integer_create(char const * const s);
  jank_object_ref jank_big_decimal_create(char const * const s);
  jank_object_ref jank_real_create(jank_f64 r);
  jank_object_ref jank_ratio_create(jank_object_ref numerator, jank_object_ref denominator);
  jank_object_ref jank_string_create(char const *s);
  jank_object_ref jank_symbol_create(jank_object_ref ns, jank_object_ref name);
  jank_object_ref jank_character_create(char const *s);
  jank_object_ref jank_regex_create(char const *s);
  jank_object_ref jank_uuid_create(char const *s);
  jank_object_ref jank_inst_create(char const *s);

  jank_object_ref jank_list_create(jank_u64 size, ...);
  jank_object_ref jank_vector_create(jank_u64 size, ...);
  jank_object_ref jank_map_create(jank_u64 pairs, ...);
  jank_object_ref jank_set_create(jank_u64 size, ...);
  jank_object_ref jank_pointer_create(void *ptr);
  void *jank_to_pointer(jank_object_ref o);
  void *jank_native_function_wrapper_get_pointer(jank_object_ref wrapper);

  jank_object_ref jank_box(char const *type, void const *o);
  void *jank_unbox(char const *type, jank_object_ref o);
  void *jank_unbox_with_source(char const *type, jank_object_ref o, jank_object_ref source);
  void *jank_unbox_lazy_source(char const *type, jank_object_ref o, char const *source_str);
  jank_object_ref jank_native_function_wrapper_create(void *callback,
                                                      void *context,
                                                      jank_native_callback_invoke_fn invoke,
                                                      jank_u8 arg_count);

  jank_arity_flags jank_function_build_arity_flags(jank_u8 highest_fixed_arity,
                                                   jank_bool is_variadic,
                                                   jank_bool is_variadic_ambiguous);
  jank_object_ref jank_function_create(jank_arity_flags arity_flags);
  void jank_function_set_arity0(jank_object_ref fn, jank_object_ref (*f)(jank_object_ref));
  void jank_function_set_arity1(jank_object_ref fn,
                                jank_object_ref (*f)(jank_object_ref, jank_object_ref));
  void
  jank_function_set_arity2(jank_object_ref fn,
                           jank_object_ref (*f)(jank_object_ref, jank_object_ref, jank_object_ref));
  void jank_function_set_arity3(
    jank_object_ref fn,
    jank_object_ref (*f)(jank_object_ref, jank_object_ref, jank_object_ref, jank_object_ref));
  void jank_function_set_arity4(jank_object_ref fn,
                                jank_object_ref (*f)(jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref));
  void jank_function_set_arity5(jank_object_ref fn,
                                jank_object_ref (*f)(jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref));
  void jank_function_set_arity6(jank_object_ref fn,
                                jank_object_ref (*f)(jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref));
  void jank_function_set_arity7(jank_object_ref fn,
                                jank_object_ref (*f)(jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref));
  void jank_function_set_arity8(jank_object_ref fn,
                                jank_object_ref (*f)(jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref));
  void jank_function_set_arity9(jank_object_ref fn,
                                jank_object_ref (*f)(jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref));
  void jank_function_set_arity10(jank_object_ref fn,
                                 jank_object_ref (*f)(jank_object_ref,
                                                      jank_object_ref,
                                                      jank_object_ref,
                                                      jank_object_ref,
                                                      jank_object_ref,
                                                      jank_object_ref,
                                                      jank_object_ref,
                                                      jank_object_ref,
                                                      jank_object_ref,
                                                      jank_object_ref,
                                                      jank_object_ref));

  jank_object_ref jank_closure_create(jank_arity_flags arity_flags, void *context);
  void jank_closure_set_arity0(jank_object_ref fn, jank_object_ref (*f)(jank_object_ref));
  void jank_closure_set_arity1(jank_object_ref fn,
                               jank_object_ref (*f)(jank_object_ref, jank_object_ref));
  void
  jank_closure_set_arity2(jank_object_ref fn,
                          jank_object_ref (*f)(jank_object_ref, jank_object_ref, jank_object_ref));
  void jank_closure_set_arity3(
    jank_object_ref fn,
    jank_object_ref (*f)(jank_object_ref, jank_object_ref, jank_object_ref, jank_object_ref));
  void jank_closure_set_arity4(jank_object_ref fn,
                               jank_object_ref (*f)(jank_object_ref,
                                                    jank_object_ref,
                                                    jank_object_ref,
                                                    jank_object_ref,
                                                    jank_object_ref));
  void jank_closure_set_arity5(jank_object_ref fn,
                               jank_object_ref (*f)(jank_object_ref,
                                                    jank_object_ref,
                                                    jank_object_ref,
                                                    jank_object_ref,
                                                    jank_object_ref,
                                                    jank_object_ref));
  void jank_closure_set_arity6(jank_object_ref fn,
                               jank_object_ref (*f)(jank_object_ref,
                                                    jank_object_ref,
                                                    jank_object_ref,
                                                    jank_object_ref,
                                                    jank_object_ref,
                                                    jank_object_ref,
                                                    jank_object_ref));
  void jank_closure_set_arity7(jank_object_ref fn,
                               jank_object_ref (*f)(jank_object_ref,
                                                    jank_object_ref,
                                                    jank_object_ref,
                                                    jank_object_ref,
                                                    jank_object_ref,
                                                    jank_object_ref,
                                                    jank_object_ref,
                                                    jank_object_ref));
  void jank_closure_set_arity8(jank_object_ref fn,
                               jank_object_ref (*f)(jank_object_ref,
                                                    jank_object_ref,
                                                    jank_object_ref,
                                                    jank_object_ref,
                                                    jank_object_ref,
                                                    jank_object_ref,
                                                    jank_object_ref,
                                                    jank_object_ref,
                                                    jank_object_ref));
  void jank_closure_set_arity9(jank_object_ref fn,
                               jank_object_ref (*f)(jank_object_ref,
                                                    jank_object_ref,
                                                    jank_object_ref,
                                                    jank_object_ref,
                                                    jank_object_ref,
                                                    jank_object_ref,
                                                    jank_object_ref,
                                                    jank_object_ref,
                                                    jank_object_ref,
                                                    jank_object_ref));
  void jank_closure_set_arity10(jank_object_ref fn,
                                jank_object_ref (*f)(jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref,
                                                     jank_object_ref));

  jank_bool jank_truthy(jank_object_ref o);
  jank_bool jank_equal(jank_object_ref l, jank_object_ref r);
  jank_uhash jank_to_hash(jank_object_ref o);
  jank_i64 jank_to_integer(jank_object_ref o);
  jank_f64 jank_to_real(jank_object_ref o);
  char const *jank_to_string(jank_object_ref o);
  jank_i64 jank_shift_mask_case_integer(jank_object_ref o, jank_i64 shift, jank_i64 mask);

  void jank_set_meta(jank_object_ref o, jank_object_ref meta);

  void jank_throw(jank_object_ref o);
  void jank_profile_enter(char const *label);
  void jank_profile_exit(char const *label);
  void jank_profile_report(char const *label);

  void jank_resource_register(char const *name, char const *data, jank_usize size);
  void jank_module_set_loaded(char const *module);

  int jank_init(int const argc,
                char const ** const argv,
                jank_bool const init_default_ctx,
                int (*fn)(int const, char const ** const));
  int jank_init_with_pch(int const argc,
                         char const ** const argv,
                         jank_bool const init_default_ctx,
                         char const * const pch_data,
                         jank_usize pch_size,
                         int (*fn)(int const, char const ** const));

  jank_object_ref jank_parse_command_line_args(int const argc, char const **argv);

  /* iOS Remote Eval Server API
   * These functions start an eval server on iOS that can optionally delegate
   * compilation to a macOS compile server for better performance. */

  /* Start eval server on iOS without remote compilation.
   * The server will compile code locally using CppInterOp. */
  void jank_ios_start_eval_server(jank_u16 port);

  /* Start eval server on iOS with remote compilation enabled.
   * Code will be sent to the macOS compile server for cross-compilation.
   * @param eval_port Port for iOS eval server (clients connect here)
   * @param compile_host macOS compile server hostname/IP
   * @param compile_port macOS compile server port (default: 5559) */
  void jank_ios_start_eval_server_remote(jank_u16 eval_port,
                                         char const *compile_host,
                                         jank_u16 compile_port);

  /* Stop the iOS eval server. */
  void jank_ios_stop_eval_server(void);

  /* Enable remote compilation on an already-running eval server. */
  void jank_ios_enable_remote_compile(char const *compile_host, jank_u16 compile_port);

  /* ---- Remote Compilation (for iOS nREPL server using full eval_string) ---- */
  /* These functions enable remote JIT compilation where iOS sends jank source code
   * to a macOS compile-server, which cross-compiles to ARM64 object files that
   * iOS loads and executes. This allows full JIT on iOS without CppInterOp. */

  /* Configure remote compilation host and port (call before connect).
   * @param host macOS compile-server hostname/IP (e.g., "192.168.1.100")
   * @param port macOS compile-server port (default: 5570) */
  void jank_remote_compile_configure(char const *host, jank_u16 port);

  /* Connect to the remote compile server.
   * @return 1 if connected successfully, 0 if connection failed */
  jank_bool jank_remote_compile_connect(void);

  /* Disconnect from the remote compile server. */
  void jank_remote_compile_disconnect(void);

  /* Check if remote compilation is enabled and connected.
   * @return 1 if remote compilation is active, 0 otherwise */
  jank_bool jank_remote_compile_is_enabled(void);

#ifdef __cplusplus
}
#endif
