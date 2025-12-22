#include <exception>
#include <string_view>
#include <utility>
#include <vector>
#include <cstdio>
#include <unistd.h>

#include <jank/runtime/core.hpp>
#include <jank/runtime/visit.hpp>
#include <jank/runtime/obj/user_type.hpp>
#include <jank/runtime/behavior/nameable.hpp>
#include <jank/runtime/behavior/derefable.hpp>
#include <jank/runtime/behavior/ref_like.hpp>
#include <jank/runtime/context.hpp>
#include <jank/runtime/sequence_range.hpp>
#include <jank/util/fmt.hpp>

namespace
{
  std::vector<std::function<void(std::string)>> &output_redirects()
  {
    thread_local std::vector<std::function<void(std::string)>> redirects;
    return redirects;
  }

  void write_to_output(std::string_view const text)
  {
    if(text.empty())
    {
      return;
    }

    auto &redirects(output_redirects());
    if(redirects.empty())
    {
      std::fwrite(text.data(), 1, text.size(), stdout);
      return;
    }

    redirects.back()(std::string{ text });
  }
}

namespace jank::runtime
{
  void push_output_redirect(std::function<void(std::string)> sink)
  {
    output_redirects().emplace_back(std::move(sink));
  }

  void pop_output_redirect()
  {
    auto &redirects(output_redirects());
    if(redirects.empty())
    {
      throw std::runtime_error{ "no output redirect to pop" };
    }

    redirects.pop_back();
  }

  scoped_output_redirect::scoped_output_redirect(std::function<void(std::string)> sink)
  {
    push_output_redirect(std::move(sink));
  }

  scoped_output_redirect::~scoped_output_redirect()
  {
    try
    {
      pop_output_redirect();
    }
    catch(...)
    {
      std::terminate();
    }
  }

  void forward_output(std::string_view const text)
  {
    write_to_output(text);
  }

  void forward_error(std::string_view const text)
  {
    write_to_output(text);
  }

  jtl::immutable_string type(object_ref const o)
  {
    return object_type_str(o->type);
  }

  bool is_nil(object_ref const o)
  {
    return o == jank_nil;
  }

  bool is_true(object_ref const o)
  {
    return o == jank_true;
  }

  bool is_false(object_ref const o)
  {
    return o == jank_false;
  }

  bool is_some(object_ref const o)
  {
    return o != jank_nil;
  }

  bool is_string(object_ref const o)
  {
    return o->type == object_type::persistent_string;
  }

  bool is_char(object_ref const o)
  {
    return o->type == object_type::character;
  }

  bool is_symbol(object_ref const o)
  {
    return o->type == object_type::symbol;
  }

  bool is_simple_symbol(object_ref const o)
  {
    return o->type == object_type::symbol && expect_object<obj::symbol>(o)->ns.empty();
  }

  bool is_qualified_symbol(object_ref const o)
  {
    return o->type == object_type::symbol && !expect_object<obj::symbol>(o)->ns.empty();
  }

  object_ref to_unqualified_symbol(object_ref const o)
  {
    return runtime::visit_object(
      [&](auto const typed_o) -> object_ref {
        using T = typename decltype(typed_o)::value_type;

        if constexpr(std::same_as<T, obj::symbol>)
        {
          return typed_o;
        }
        else if constexpr(std::same_as<T, obj::persistent_string>)
        {
          return make_box<obj::symbol>(typed_o->data);
        }
        else if constexpr(std::same_as<T, var>)
        {
          return make_box<obj::symbol>(typed_o->n->name->name, typed_o->name->name);
        }
        else if constexpr(std::same_as<T, obj::keyword>)
        {
          return typed_o->sym;
        }
        else
        {
          throw std::runtime_error{ util::format("can't convert {} to a symbol",
                                                 typed_o->to_code_string()) };
        }
      },
      o);
  }

  object_ref to_qualified_symbol(object_ref const ns, object_ref const name)
  {
    return make_box<obj::symbol>(ns, name);
  }

  object_ref print(object_ref const args)
  {
    visit_object(
      [](auto const typed_args) {
        using T = typename decltype(typed_args)::value_type;

        if constexpr(behavior::sequenceable<T>)
        {
          jtl::string_builder buff;
          runtime::to_string(typed_args->first().erase(), buff);
          for(auto const e : make_sequence_range(typed_args).skip(1))
          {
            buff(' ');
            runtime::to_string(e.erase(), buff);
          }
          write_to_output(std::string_view{ buff.data(), buff.size() });
        }
        else
        {
          throw std::runtime_error{ util::format("expected a sequence: {}",
                                                 typed_args->to_string()) };
        }
      },
      args);
    return jank_nil;
  }

  object_ref println(object_ref const args)
  {
    visit_object(
      [](auto const typed_more) {
        using T = typename decltype(typed_more)::value_type;

        if constexpr(std::same_as<T, obj::nil>)
        {
          write_to_output(std::string_view{ "\n", 1 });
        }
        else if constexpr(behavior::sequenceable<T>)
        {
          jtl::string_builder buff;
          runtime::to_string(typed_more->first().erase(), buff);
          for(auto const e : make_sequence_range(typed_more).skip(1))
          {
            buff(' ');
            runtime::to_string(e.erase(), buff);
          }
          write_to_output(std::string_view{ buff.data(), buff.size() });
          write_to_output(std::string_view{ "\n", 1 });
        }
        else
        {
          throw std::runtime_error{ util::format("expected a sequence: {}",
                                                 typed_more->to_string()) };
        }
      },
      args);
    return jank_nil;
  }

  object_ref pr(object_ref const args)
  {
    visit_object(
      [](auto const typed_args) {
        using T = typename decltype(typed_args)::value_type;

        if constexpr(behavior::sequenceable<T>)
        {
          jtl::string_builder buff;
          runtime::to_code_string(typed_args->first().erase(), buff);
          for(auto const e : make_sequence_range(typed_args).skip(1))
          {
            buff(' ');
            runtime::to_code_string(e.erase(), buff);
          }
          write_to_output(std::string_view{ buff.data(), buff.size() });
        }
        else
        {
          throw std::runtime_error{ util::format("expected a sequence: {}",
                                                 typed_args->to_string()) };
        }
      },
      args);
    return jank_nil;
  }

  object_ref prn(object_ref const args)
  {
    visit_object(
      [](auto const typed_args) {
        using T = typename decltype(typed_args)::value_type;

        if constexpr(std::same_as<T, obj::nil>)
        {
          write_to_output(std::string_view{ "\n", 1 });
        }
        else if constexpr(behavior::sequenceable<T>)
        {
          jtl::string_builder buff;
          runtime::to_code_string(typed_args->first().erase(), buff);
          for(auto const e : make_sequence_range(typed_args).skip(1))
          {
            buff(' ');
            runtime::to_code_string(e.erase(), buff);
          }
          write_to_output(std::string_view{ buff.data(), buff.size() });
          write_to_output(std::string_view{ "\n", 1 });
        }
        else
        {
          throw std::runtime_error{ util::format("expected a sequence: {}",
                                                 typed_args->to_string()) };
        }
      },
      args);
    return jank_nil;
  }

  f64 to_real(object_ref const o)
  {
    return visit_number_like(
      [](auto const typed_o) -> f64 { return typed_o->to_real(); },
      [=]() -> f64 { throw std::runtime_error{ util::format("not a number: {}", to_string(o)) }; },
      o);
  }

  object_ref to_float(object_ref const o)
  {
    auto const value(static_cast<f32>(to_real(o)));
    return make_box<obj::real>(static_cast<f64>(value), true);
  }

  obj::persistent_string_ref subs(object_ref const s, object_ref const start)
  {
    return visit_type<obj::persistent_string>(
      [](auto const typed_s, i64 const start) -> obj::persistent_string_ref {
        return typed_s->substring(start).expect_ok();
      },
      s,
      to_int(start));
  }

  obj::persistent_string_ref subs(object_ref const s, object_ref const start, object_ref const end)
  {
    return visit_type<obj::persistent_string>(
      [](auto const typed_s, i64 const start, i64 const end) -> obj::persistent_string_ref {
        return typed_s->substring(start, end).expect_ok();
      },
      s,
      to_int(start),
      to_int(end));
  }

  i64 first_index_of(object_ref const s, object_ref const m)
  {
    return visit_type<obj::persistent_string>(
      [](auto const typed_s, object_ref const m) -> i64 { return typed_s->first_index_of(m); },
      s,
      m);
  }

  i64 last_index_of(object_ref const s, object_ref const m)
  {
    return visit_type<obj::persistent_string>(
      [](auto const typed_s, object_ref const m) -> i64 { return typed_s->last_index_of(m); },
      s,
      m);
  }

  bool is_named(object_ref const o)
  {
    return visit_object(
      [](auto const typed_o) {
        using T = typename decltype(typed_o)::value_type;

        return behavior::nameable<T>;
      },
      o);
  }

  jtl::immutable_string name(object_ref const o)
  {
    return visit_object(
      [](auto const typed_o) -> jtl::immutable_string {
        using T = typename decltype(typed_o)::value_type;

        if constexpr(std::same_as<T, obj::persistent_string>)
        {
          return typed_o->data;
        }
        else if constexpr(behavior::nameable<T>)
        {
          return typed_o->get_name();
        }
        else
        {
          throw std::runtime_error{ util::format("not nameable: {}", typed_o->to_string()) };
        }
      },
      o);
  }

  object_ref namespace_(object_ref const o)
  {
    return visit_object(
      [](auto const typed_o) -> object_ref {
        using T = typename decltype(typed_o)::value_type;

        if constexpr(behavior::nameable<T>)
        {
          auto const ns(typed_o->get_namespace());
          if(ns.empty())
          {
            return jank_nil;
          }
          return make_box<obj::persistent_string>(ns);
        }
        else
        {
          throw std::runtime_error{ util::format("not nameable: {}", typed_o->to_string()) };
        }
      },
      o);
  }

  object_ref keyword(object_ref const ns, object_ref const name)
  {
    if(!ns.is_nil() && ns->type != object_type::persistent_string)
    {
      throw std::runtime_error{ util::format(
        "The 'keyword' function expects a namespace to be 'nil' or a 'string', got {} instead.",
        runtime::to_code_string(ns)) };
    }
    if(name->type != object_type::persistent_string)
    {
      throw std::runtime_error{ util::format(
        "The 'keyword' function expects the name to be a 'string', got {} instead.",
        runtime::to_code_string(name)) };
    }

    if(ns.is_nil())
    {
      return __rt_ctx->intern_keyword(runtime::to_string(name)).expect_ok();
    }

    return __rt_ctx->intern_keyword(runtime::to_string(ns), runtime::to_string(name)).expect_ok();
  }

  bool is_keyword(object_ref const o)
  {
    return o->type == object_type::keyword;
  }

  bool is_simple_keyword(object_ref const o)
  {
    return o->type == object_type::keyword && expect_object<obj::keyword>(o)->sym->ns.empty();
  }

  bool is_qualified_keyword(object_ref const o)
  {
    return o->type == object_type::keyword && !expect_object<obj::keyword>(o)->sym->ns.empty();
  }

  bool is_callable(object_ref const o)
  {
    return visit_object(
      [=](auto const typed_o) -> bool {
        using T = typename decltype(typed_o)::value_type;

        return std::is_base_of_v<behavior::callable, T>;
      },
      o);
  }

  uhash to_hash(object_ref const o)
  {
    return visit_object([=](auto const typed_o) -> uhash { return typed_o->to_hash(); }, o);
  }

  object_ref macroexpand1(object_ref const o)
  {
    return __rt_ctx->macroexpand1(o);
  }

  object_ref macroexpand(object_ref const o)
  {
    return __rt_ctx->macroexpand(o);
  }

  object_ref gensym(object_ref const o)
  {
    return make_box<obj::symbol>(__rt_ctx->unique_symbol(to_string(o)));
  }

  object_ref atom(object_ref const o)
  {
    return make_box<obj::atom>(o);
  }

  object_ref swap_atom(object_ref const atom, object_ref const fn)
  {
    return try_object<obj::atom>(atom)->swap(fn);
  }

  object_ref swap_atom(object_ref const atom, object_ref const fn, object_ref const a1)
  {
    return try_object<obj::atom>(atom)->swap(fn, a1);
  }

  object_ref
  swap_atom(object_ref const atom, object_ref const fn, object_ref const a1, object_ref const a2)
  {
    return try_object<obj::atom>(atom)->swap(fn, a1, a2);
  }

  object_ref swap_atom(object_ref const atom,
                       object_ref const fn,
                       object_ref const a1,
                       object_ref const a2,
                       object_ref const rest)
  {
    return try_object<obj::atom>(atom)->swap(fn, a1, a2, rest);
  }

  object_ref swap_vals(object_ref const atom, object_ref const fn)
  {
    return try_object<obj::atom>(atom)->swap_vals(fn);
  }

  object_ref swap_vals(object_ref const atom, object_ref const fn, object_ref const a1)
  {
    return try_object<obj::atom>(atom)->swap_vals(fn, a1);
  }

  object_ref
  swap_vals(object_ref const atom, object_ref const fn, object_ref const a1, object_ref const a2)
  {
    return try_object<obj::atom>(atom)->swap_vals(fn, a1, a2);
  }

  object_ref swap_vals(object_ref const atom,
                       object_ref const fn,
                       object_ref const a1,
                       object_ref const a2,
                       object_ref const rest)
  {
    return try_object<obj::atom>(atom)->swap_vals(fn, a1, a2, rest);
  }

  object_ref
  compare_and_set(object_ref const atom, object_ref const old_val, object_ref const new_val)
  {
    return try_object<obj::atom>(atom)->compare_and_set(old_val, new_val);
  }

  object_ref reset(object_ref const atom, object_ref const new_val)
  {
    return try_object<obj::atom>(atom)->reset(new_val);
  }

  object_ref reset_vals(object_ref const atom, object_ref const new_val)
  {
    return try_object<obj::atom>(atom)->reset_vals(new_val);
  }

  object_ref deref(object_ref const o)
  {
    return visit_object(
      [=](auto const typed_o) -> object_ref {
        using T = typename decltype(typed_o)::value_type;

        if constexpr(behavior::derefable<T>)
        {
          return typed_o->deref();
        }
        else
        {
          throw std::runtime_error{ util::format("not derefable: {}", typed_o->to_string()) };
        }
      },
      o);
  }

  object_ref volatile_(object_ref const o)
  {
    return make_box<obj::volatile_>(o);
  }

  bool is_volatile(object_ref const o)
  {
    return o->type == object_type::volatile_;
  }

  object_ref vswap(object_ref const v, object_ref const fn)
  {
    auto const v_obj(try_object<obj::volatile_>(v));
    return v_obj->reset(dynamic_call(fn, v_obj->deref()));
  }

  object_ref vswap(object_ref const v, object_ref const fn, object_ref const args)
  {
    auto const v_obj(try_object<obj::volatile_>(v));
    return v_obj->reset(apply_to(fn, make_box<obj::cons>(v_obj->deref(), args)));
  }

  object_ref vreset(object_ref const v, object_ref const new_val)
  {
    return try_object<obj::volatile_>(v)->reset(new_val);
  }

  void push_thread_bindings(object_ref const o)
  {
    __rt_ctx->push_thread_bindings(o).expect_ok();
  }

  void pop_thread_bindings()
  {
    __rt_ctx->pop_thread_bindings().expect_ok();
  }

  object_ref get_thread_bindings()
  {
    return __rt_ctx->get_thread_bindings();
  }

  object_ref force(object_ref const o)
  {
    if(o->type == object_type::delay)
    {
      return expect_object<obj::delay>(o)->deref();
    }
    return o;
  }

  object_ref tagged_literal(object_ref const tag, object_ref const form)
  {
    return make_box<obj::tagged_literal>(tag, form);
  }

  bool is_tagged_literal(object_ref const o)
  {
    return o->type == object_type::tagged_literal;
  }

  object_ref re_pattern(object_ref const o)
  {
    return make_box<obj::re_pattern>(try_object<obj::persistent_string>(o)->data);
  }

  object_ref re_matcher(object_ref const re, object_ref const s)
  {
    return make_box<obj::re_matcher>(try_object<obj::re_pattern>(re),
                                     try_object<obj::persistent_string>(s)->data);
  }

  object_ref smatch_to_vector(std::smatch const &match_results)
  {
    auto const size(match_results.size());
    switch(size)
    {
      case 0:
        return jank_nil;
      case 1:
        {
          return make_box<obj::persistent_string>(match_results[0].str());
        }
      default:
        {
          native_vector<object_ref> vec;
          vec.reserve(size);

          for(auto const &s : match_results)
          {
            vec.emplace_back(make_box<obj::persistent_string>(s.str()));
          }

          return make_box<obj::persistent_vector>(
            runtime::detail::native_persistent_vector{ vec.begin(), vec.end() });
        }
    }
  }

  object_ref re_find(object_ref const m)
  {
    std::smatch match_results{};
    auto const matcher(try_object<obj::re_matcher>(m));
    std::regex_search(matcher->match_input, match_results, matcher->re->regex);

    // Copy out the match result substrings before mutating the source
    // match_input string below.
    matcher->groups = smatch_to_vector(match_results);

    if(!match_results.empty())
    {
      matcher->match_input = match_results.suffix().str();
    }

    return matcher->groups;
  }

  object_ref re_groups(object_ref const m)
  {
    auto const matcher(try_object<obj::re_matcher>(m));

    if(matcher->groups.is_nil())
    {
      throw std::runtime_error{ "No match found" };
    }

    return matcher->groups;
  }

  object_ref re_matches(object_ref const re, object_ref const s)
  {
    std::smatch match_results{};
    std::string const search_str{ try_object<obj::persistent_string>(s)->data.c_str() };

    std::regex_search(search_str,
                      match_results,
                      try_object<obj::re_pattern>(re)->regex,
                      std::regex_constants::match_continuous);

    if(!match_results.suffix().str().empty())
    {
      return jank_nil;
    }

    return smatch_to_vector(match_results);
  }

  object_ref parse_uuid(object_ref const o)
  {
    if(o->type == object_type::persistent_string)
    {
      try
      {
        return make_box<obj::uuid>(expect_object<obj::persistent_string>(o)->data);
      }
      catch(...)
      {
        return jank_nil;
      }
    }
    else
    {
      throw std::runtime_error{ util::format("expected string, got {}", object_type_str(o->type)) };
    }
  }

  bool is_uuid(object_ref const o)
  {
    return o->type == object_type::uuid;
  }

  object_ref random_uuid()
  {
    return make_box<obj::uuid>();
  }

  bool is_inst(object_ref const o)
  {
    return o->type == object_type::inst;
  }

  i64 inst_ms(object_ref const o)
  {
    if(o->type != object_type::inst)
    {
      throw std::runtime_error{ util::format("The function 'inst-ms' expects an inst, got {}",
                                             object_type_str(o->type)) };
    }

    return std::chrono::duration_cast<std::chrono::milliseconds>(
             expect_object<obj::inst>(o)->value.time_since_epoch())
      .count();
  }

  object_ref get_global_cpp_functions()
  {
    auto const locked_globals{ __rt_ctx->global_cpp_functions.rlock() };
    usize total_functions{};
    for(auto const &entry : *locked_globals)
    {
      total_functions += entry.second.size();
    }

    auto const name_kw(__rt_ctx->intern_keyword("name").expect_ok());
    auto const args_kw(__rt_ctx->intern_keyword("args").expect_ok());
    auto const type_kw(__rt_ctx->intern_keyword("type").expect_ok());
    auto const return_type_kw(__rt_ctx->intern_keyword("return-type").expect_ok());
    auto const origin_kw(__rt_ctx->intern_keyword("origin").expect_ok());

    native_vector<object_ref> vec;
    vec.reserve(total_functions);
    for(auto const &[fn_name, overloads] : *locked_globals)
    {
      for(auto const &metadata : overloads)
      {
        native_vector<object_ref> args;
        args.reserve(metadata.arguments.size());
        for(auto const &argument : metadata.arguments)
        {
          auto const arg_map(obj::persistent_hash_map::create_unique(
            std::make_pair(name_kw, make_box<obj::persistent_string>(argument.name)),
            std::make_pair(type_kw, make_box<obj::persistent_string>(argument.type))));
          args.emplace_back(arg_map);
        }

        auto const args_vector(make_box<obj::persistent_vector>(
          runtime::detail::native_persistent_vector{ args.begin(), args.end() }));

        auto function_map(obj::persistent_hash_map::empty());
        function_map
          = function_map->assoc(name_kw, make_box<obj::persistent_string>(metadata.name));
        function_map = function_map->assoc(return_type_kw,
                                           make_box<obj::persistent_string>(metadata.return_type));
        function_map = function_map->assoc(args_kw, args_vector);
        if(metadata.origin.is_some())
        {
          function_map
            = function_map->assoc(origin_kw,
                                  make_box<obj::persistent_string>(metadata.origin.unwrap()));
        }

        vec.emplace_back(function_map);
      }
    }

    return make_box<obj::persistent_vector>(
      runtime::detail::native_persistent_vector{ vec.begin(), vec.end() });
  }

  object_ref get_global_cpp_types()
  {
    auto const locked_types{ __rt_ctx->global_cpp_types.rlock() };
    auto const name_kw(__rt_ctx->intern_keyword("name").expect_ok());
    auto const type_kw(__rt_ctx->intern_keyword("type").expect_ok());
    auto const fields_kw(__rt_ctx->intern_keyword("fields").expect_ok());
    auto const kind_kw(__rt_ctx->intern_keyword("kind").expect_ok());
    auto const qualified_kw(__rt_ctx->intern_keyword("qualified-name").expect_ok());
    auto const constructors_kw(__rt_ctx->intern_keyword("constructors").expect_ok());
    auto const args_kw(__rt_ctx->intern_keyword("args").expect_ok());

    native_vector<object_ref> vec;
    vec.reserve(locked_types->size());
    for(auto const &[_, metadata] : *locked_types)
    {
      native_vector<object_ref> field_entries;
      field_entries.reserve(metadata.fields.size());
      for(auto const &field : metadata.fields)
      {
        auto field_map(obj::persistent_hash_map::empty());
        field_map = field_map->assoc(name_kw, make_box<obj::persistent_string>(field.name));
        field_map = field_map->assoc(type_kw, make_box<obj::persistent_string>(field.type));
        field_entries.emplace_back(field_map);
      }

      auto const field_vector(make_box<obj::persistent_vector>(
        runtime::detail::native_persistent_vector{ field_entries.begin(), field_entries.end() }));

      auto type_map(obj::persistent_hash_map::empty());
      type_map = type_map->assoc(name_kw, make_box<obj::persistent_string>(metadata.name));
      type_map = type_map->assoc(qualified_kw,
                                 make_box<obj::persistent_string>(metadata.qualified_cpp_name));

      std::string kind_token;
      switch(metadata.kind)
      {
        case context::cpp_record_kind::Class:
          kind_token = "class";
          break;
        case context::cpp_record_kind::Union:
          kind_token = "union";
          break;
        case context::cpp_record_kind::Struct:
        default:
          kind_token = "struct";
          break;
      }
      auto const kind_string(
        jtl::immutable_string{ kind_token.data(), static_cast<size_t>(kind_token.size()) });
      type_map = type_map->assoc(kind_kw, make_box<obj::persistent_string>(kind_string));
      type_map = type_map->assoc(fields_kw, field_vector);

      native_vector<object_ref> ctor_entries;
      ctor_entries.reserve(metadata.constructors.size());
      for(auto const &ctor : metadata.constructors)
      {
        native_vector<object_ref> arg_entries;
        arg_entries.reserve(ctor.arguments.size());
        for(auto const &arg : ctor.arguments)
        {
          auto arg_map(obj::persistent_hash_map::empty());
          arg_map = arg_map->assoc(name_kw, make_box<obj::persistent_string>(arg.name));
          arg_map = arg_map->assoc(type_kw, make_box<obj::persistent_string>(arg.type));
          arg_entries.emplace_back(arg_map);
        }
        auto const arg_vector(make_box<obj::persistent_vector>(
          runtime::detail::native_persistent_vector{ arg_entries.begin(), arg_entries.end() }));
        auto ctor_map(obj::persistent_hash_map::empty());
        ctor_map = ctor_map->assoc(args_kw, arg_vector);
        ctor_entries.emplace_back(ctor_map);
      }
      auto const ctor_vector(make_box<obj::persistent_vector>(
        runtime::detail::native_persistent_vector{ ctor_entries.begin(), ctor_entries.end() }));
      type_map = type_map->assoc(constructors_kw, ctor_vector);

      vec.emplace_back(type_map);
    }

    return make_box<obj::persistent_vector>(
      runtime::detail::native_persistent_vector{ vec.begin(), vec.end() });
  }

  object_ref add_watch(object_ref const reference, object_ref const key, object_ref const fn)
  {
    visit_object(
      [=](auto const typed_reference) -> void {
        using T = typename decltype(typed_reference)::value_type;

        if constexpr(behavior::ref_like<T>)
        {
          typed_reference->add_watch(key, fn);
        }
        else
        {
          throw std::runtime_error{ util::format(
            "Value does not support 'add-watch' because it is not ref_like: {}",
            typed_reference->to_code_string()) };
        }
      },
      reference);

    return reference;
  }

  object_ref remove_watch(object_ref const reference, object_ref const key)
  {
    visit_object(
      [=](auto const typed_reference) -> void {
        using T = typename decltype(typed_reference)::value_type;

        if constexpr(behavior::ref_like<T>)
        {
          typed_reference->remove_watch(key);
        }
        else
        {
          throw std::runtime_error{ util::format(
            "Value does not support 'remove-watch' because it is not ref_like: {}",
            typed_reference->to_code_string()) };
        }
      },
      reference);

    return reference;
  }

  object_ref make_user_type(object_ref const type_name, object_ref const constructor_fn)
  {
    return obj::make_user_type(type_name, constructor_fn);
  }

}

/* Implementation of scoped_stderr_redirect using file descriptor redirection. */
namespace jank::runtime
{
  struct scoped_stderr_redirect::impl
  {
    impl()
      : temp_file{ tmpfile() }
    {
      if(!temp_file)
      {
        return;
      }

      /* Save the original stderr file descriptor. */
      original_stderr_fd = dup(STDERR_FILENO);
      if(original_stderr_fd < 0)
      {
        fclose(temp_file);
        temp_file = nullptr;
        return;
      }

      /* Flush stderr before redirecting. */
      fflush(stderr);

      /* Redirect stderr to the temporary file. */
      if(dup2(fileno(temp_file), STDERR_FILENO) < 0)
      {
        close(original_stderr_fd);
        original_stderr_fd = -1;
        fclose(temp_file);
        temp_file = nullptr;
      }
    }

    ~impl() noexcept
    {
      if(!temp_file || original_stderr_fd < 0)
      {
        return;
      }

      /* Flush and forward any remaining content. */
      try
      {
        flush_impl();
      }
      catch(...) // NOLINT(bugprone-empty-catch)
      {
        /* Silently ignore errors during destructor cleanup to maintain noexcept guarantee. */
      }

      /* Restore the original stderr. */
      dup2(original_stderr_fd, STDERR_FILENO);
      close(original_stderr_fd);

      fclose(temp_file);
    }

    void flush_impl()
    {
      if(!temp_file || original_stderr_fd < 0)
      {
        return;
      }

      /* Flush any remaining data to the temporary file. */
      fflush(stderr);

      /* Read the captured content from the temporary file (only new content since last read). */
      fseek(temp_file, 0L, SEEK_END);
      long const file_size{ ftell(temp_file) };
      if(file_size <= static_cast<long>(bytes_already_read))
      {
        return;
      }

      fseek(temp_file, static_cast<long>(bytes_already_read), SEEK_SET);

      /* Use a fixed-size buffer. Read in chunks if the content is larger than the buffer. */
      constexpr size_t buffer_size{ 4096 };
      char buffer[buffer_size];

      bool const has_redirects{ !output_redirects().empty() };

      size_t remaining{ static_cast<size_t>(file_size) - bytes_already_read };
      while(remaining > 0)
      {
        size_t const to_read{ remaining < buffer_size ? remaining : buffer_size };
        size_t const bytes_read{ fread(buffer, sizeof(char), to_read, temp_file) };
        if(bytes_read == 0)
        {
          break;
        }

        bytes_already_read += bytes_read;

        /* Forward the captured stderr content.
         * If redirects are active, use them; otherwise write directly to stderr. */
        if(has_redirects)
        {
          forward_error(std::string_view{ buffer, bytes_read });
        }
        else
        {
          /* No redirects active, write directly to stderr. */
          fwrite(buffer, sizeof(char), bytes_read, stderr);
        }

        remaining -= bytes_read;
      }
    }

    FILE *temp_file{};
    int original_stderr_fd{ -1 };
    size_t bytes_already_read{};
  };

  scoped_stderr_redirect::scoped_stderr_redirect()
    : pimpl{ std::make_unique<impl>() }
  {
  }

  scoped_stderr_redirect::~scoped_stderr_redirect() = default;

  void scoped_stderr_redirect::flush()
  {
    pimpl->flush_impl();
  }
}
