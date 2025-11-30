#pragma once

#include <functional>
#include <regex>
#include <string>
#include <string_view>

/* TODO: Remove these so that people include only what they need. */
#include <jank/runtime/core/make_box.hpp>
#include <jank/runtime/core/to_string.hpp>
#include <jank/runtime/core/seq.hpp>
#include <jank/runtime/core/truthy.hpp>
#include <jank/runtime/core/munge.hpp>
#include <jank/runtime/core/math.hpp>

namespace jank::runtime
{
  jtl::immutable_string type(object_ref o);
  bool is_nil(object_ref o);
  bool is_true(object_ref o);
  bool is_false(object_ref o);
  bool is_some(object_ref o);
  bool is_string(object_ref o);
  bool is_char(object_ref o);

  bool is_symbol(object_ref o);
  bool is_simple_symbol(object_ref o);
  bool is_qualified_symbol(object_ref o);

  object_ref to_unqualified_symbol(object_ref o);
  object_ref to_qualified_symbol(object_ref ns, object_ref name);

  object_ref print(object_ref args);
  object_ref println(object_ref args);
  object_ref pr(object_ref args);
  object_ref prn(object_ref args);

  void forward_output(std::string_view text);
  void forward_error(std::string_view text);

  void push_output_redirect(std::function<void(std::string)> sink);
  void pop_output_redirect();

  struct scoped_output_redirect
  {
    explicit scoped_output_redirect(std::function<void(std::string)> sink);
    scoped_output_redirect(scoped_output_redirect const &) = delete;
    scoped_output_redirect(scoped_output_redirect &&) = delete;
    scoped_output_redirect &operator=(scoped_output_redirect const &) = delete;
    scoped_output_redirect &operator=(scoped_output_redirect &&) = delete;
    ~scoped_output_redirect();
  };

  /* A scoped guard that captures stderr output during C++ compilation and forwards it
   * to the same output redirect as stdout. This allows C++ compilation errors to appear
   * in the IDE REPL. */
  struct scoped_stderr_redirect
  {
    scoped_stderr_redirect();
    scoped_stderr_redirect(scoped_stderr_redirect const &) = delete;
    scoped_stderr_redirect(scoped_stderr_redirect &&) = delete;
    scoped_stderr_redirect &operator=(scoped_stderr_redirect const &) = delete;
    scoped_stderr_redirect &operator=(scoped_stderr_redirect &&) = delete;
    ~scoped_stderr_redirect();

    struct impl;
    std::unique_ptr<impl> pimpl;
  };

  obj::persistent_string_ref subs(object_ref s, object_ref start);
  obj::persistent_string_ref subs(object_ref s, object_ref start, object_ref end);
  i64 first_index_of(object_ref s, object_ref m);
  i64 last_index_of(object_ref s, object_ref m);

  bool is_named(object_ref o);
  jtl::immutable_string name(object_ref o);
  object_ref namespace_(object_ref o);

  object_ref keyword(object_ref ns, object_ref name);
  bool is_keyword(object_ref o);
  bool is_simple_keyword(object_ref o);
  bool is_qualified_keyword(object_ref o);

  bool is_callable(object_ref o);

  uhash to_hash(object_ref o);

  object_ref macroexpand1(object_ref o);
  object_ref macroexpand(object_ref o);

  object_ref gensym(object_ref o);

  object_ref atom(object_ref o);
  object_ref deref(object_ref o);
  object_ref swap_atom(object_ref atom, object_ref fn);
  object_ref swap_atom(object_ref atom, object_ref fn, object_ref a1);
  object_ref swap_atom(object_ref atom, object_ref fn, object_ref a1, object_ref a2);
  object_ref
  swap_atom(object_ref atom, object_ref fn, object_ref a1, object_ref a2, object_ref rest);
  object_ref swap_vals(object_ref atom, object_ref fn);
  object_ref swap_vals(object_ref atom, object_ref fn, object_ref a1);
  object_ref swap_vals(object_ref atom, object_ref fn, object_ref a1, object_ref a2);
  object_ref
  swap_vals(object_ref atom, object_ref fn, object_ref a1, object_ref a2, object_ref rest);
  object_ref compare_and_set(object_ref atom, object_ref old_val, object_ref new_val);
  object_ref reset(object_ref atom, object_ref new_val);
  object_ref reset_vals(object_ref atom, object_ref new_val);

  object_ref volatile_(object_ref o);
  bool is_volatile(object_ref o);
  object_ref vswap(object_ref v, object_ref fn);
  object_ref vswap(object_ref v, object_ref fn, object_ref args);
  object_ref vreset(object_ref v, object_ref new_val);

  void push_thread_bindings(object_ref o);
  void pop_thread_bindings();
  object_ref get_thread_bindings();

  object_ref force(object_ref o);

  object_ref tagged_literal(object_ref tag, object_ref form);
  bool is_tagged_literal(object_ref o);

  object_ref parse_uuid(object_ref o);
  bool is_uuid(object_ref o);
  object_ref random_uuid();

  bool is_inst(object_ref o);
  i64 inst_ms(object_ref o);

  object_ref re_pattern(object_ref o);
  object_ref re_matcher(object_ref re, object_ref s);
  object_ref re_find(object_ref m);
  object_ref re_groups(object_ref m);
  object_ref re_matches(object_ref re, object_ref s);
  object_ref smatch_to_vector(std::smatch const &match_results);

  object_ref get_global_cpp_functions();
  object_ref get_global_cpp_types();

  object_ref add_watch(object_ref reference, object_ref key, object_ref fn);
  object_ref remove_watch(object_ref reference, object_ref key);
}
