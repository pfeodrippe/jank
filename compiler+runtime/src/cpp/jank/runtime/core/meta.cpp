#include <vector>
#include <exception>

#include <jank/runtime/visit.hpp>
#include <jank/runtime/core/meta.hpp>
#include <jank/runtime/core/math.hpp>
#include <jank/runtime/core/seq.hpp>
#include <jank/runtime/core/make_box.hpp>
#include <jank/runtime/context.hpp>
#include <jank/runtime/behavior/metadatable.hpp>
#include <jank/util/fmt.hpp>
#include <jank/error/runtime.hpp>

namespace jank::runtime
{
  namespace
  {
    auto &source_hint_stack()
    {
      thread_local std::vector<read::source> stack;
      return stack;
    }

    obj::persistent_hash_map_ref build_source_meta(read::source const &source)
    {
      auto const source_map{ obj::persistent_array_map::empty()->to_transient() };

      if(source.module != read::no_source_path)
      {
        source_map->assoc_in_place(__rt_ctx->intern_keyword("module").expect_ok(),
                                   make_box(source.module));
      }

      if(source.file != read::no_source_path)
      {
        source_map->assoc_in_place(__rt_ctx->intern_keyword("file").expect_ok(),
                                   make_box(source.file));
      }

      auto const start_map{ obj::persistent_array_map::create_unique(
        __rt_ctx->intern_keyword("offset").expect_ok(),
        make_box(source.start.offset),
        __rt_ctx->intern_keyword("line").expect_ok(),
        make_box(source.start.line),
        __rt_ctx->intern_keyword("col").expect_ok(),
        make_box(source.start.col)) };
      auto const end_map{ obj::persistent_array_map::create_unique(
        __rt_ctx->intern_keyword("offset").expect_ok(),
        make_box(source.end.offset),
        __rt_ctx->intern_keyword("line").expect_ok(),
        make_box(source.end.line),
        __rt_ctx->intern_keyword("col").expect_ok(),
        make_box(source.end.col)) };
      source_map->assoc_in_place(__rt_ctx->intern_keyword("start").expect_ok(), start_map);
      source_map->assoc_in_place(__rt_ctx->intern_keyword("end").expect_ok(), end_map);

      if(!source.macro_expansion.is_nil())
      {
        source_map->assoc_in_place(__rt_ctx->intern_keyword("jank/macro-expansion").expect_ok(),
                                   source.macro_expansion);
      }

      auto const key{ __rt_ctx->intern_keyword("jank/source").expect_ok() };
      return obj::persistent_hash_map::create_unique(
        std::make_pair(key, source_map->to_persistent()));
    }

    obj::keyword_ref source_hint_keyword()
    {
      static obj::keyword_ref const key{ __rt_ctx->intern_keyword("jank/source-hint").expect_ok() };
      return key;
    }
  }

  object_ref meta(object_ref const m)
  {
    if(m.is_nil())
    {
      return m;
    }

    return visit_object(
      [](auto const typed_m) -> object_ref {
        using T = typename jtl::decay_t<decltype(typed_m)>::value_type;

        if constexpr(behavior::metadatable<T>)
        {
          return typed_m->meta.unwrap_or(jank_nil());
        }
        else
        {
          return jank_nil();
        }
      },
      m);
  }

  object_ref with_meta(object_ref const o, object_ref const m)
  {
    return visit_object(
      [&o](auto const typed_o, object_ref const m) -> object_ref {
        using T = typename jtl::decay_t<decltype(typed_o)>::value_type;

        if constexpr(behavior::metadatable<T>)
        {
          return typed_o->with_meta(m);
        }
        else
        {
          throw error::runtime_non_metadatable_value(
            util::format("{} [{}] can't hold any metadata.",
                         typed_o->to_code_string(),
                         object_type_str(o->type)),
            object_source(o));
        }
      },
      o,
      m);
  }

  /* This is the same as with_meta, but it gracefully handles the target
   * not supporting metadata. In that case, the target is returned and nothing
   * is done with the meta. */
  object_ref with_meta_graceful(object_ref const o, object_ref const m)
  {
    return visit_object(
      [](auto const typed_o, object_ref const m) -> object_ref {
        using T = typename jtl::decay_t<decltype(typed_o)>::value_type;

        if constexpr(behavior::metadatable<T>)
        {
          return typed_o->with_meta(m);
        }
        else
        {
          return typed_o;
        }
      },
      o,
      m);
  }

  object_ref reset_meta(object_ref const o, object_ref const m)
  {
    return visit_object(
      [](auto const typed_o, object_ref const m) -> object_ref {
        using T = typename jtl::decay_t<decltype(typed_o)>::value_type;

        if constexpr(behavior::metadatable<T>)
        {
          auto const meta(behavior::detail::validate_meta(m));
          typed_o->meta = meta;
          return m;
        }
        else
        {
          throw std::runtime_error{ util::format("not metadatable: {} [{}]",
                                                 typed_o->to_code_string(),
                                                 object_type_str(typed_o->base.type)) };
        }
      },
      o,
      m);
  }

  read::source meta_source(jtl::option<runtime::object_ref> const &o)
  {
    using namespace jank::runtime;

    auto const meta(o.unwrap_or(jank_nil()));
    auto const source(get(meta, __rt_ctx->intern_keyword("jank/source").expect_ok()));
    if(source == jank_nil())
    {
      return read::source::unknown();
    }

    auto const file(get(source, __rt_ctx->intern_keyword("file").expect_ok()));
    if(file == jank_nil())
    {
      return read::source::unknown();
    }
    jtl::immutable_string const file_str{ runtime::to_string(file) };

    auto const module(get(source, __rt_ctx->intern_keyword("module").expect_ok()));
    jtl::immutable_string module_str{ read::no_source_path };
    if(module != jank_nil())
    {
      module_str = runtime::to_string(module);
    }

    auto const start(get(source, __rt_ctx->intern_keyword("start").expect_ok()));
    auto const end(get(source, __rt_ctx->intern_keyword("end").expect_ok()));

    auto const start_offset(get(start, __rt_ctx->intern_keyword("offset").expect_ok()));
    auto const start_line(get(start, __rt_ctx->intern_keyword("line").expect_ok()));
    auto const start_col(get(start, __rt_ctx->intern_keyword("col").expect_ok()));

    auto const end_offset(get(end, __rt_ctx->intern_keyword("offset").expect_ok()));
    auto const end_line(get(end, __rt_ctx->intern_keyword("line").expect_ok()));
    auto const end_col(get(end, __rt_ctx->intern_keyword("col").expect_ok()));

    auto const macro_expansion(
      get(meta, __rt_ctx->intern_keyword("jank/macro-expansion").expect_ok()));

    return {
      file_str,
      module_str,
      { static_cast<size_t>(to_int(start_offset)),
        static_cast<size_t>(to_int(start_line)),
        static_cast<size_t>(to_int(start_col)) },
      {   static_cast<size_t>(to_int(end_offset)),
        static_cast<size_t>(to_int(end_line)),
        static_cast<size_t>(to_int(end_col))  },
      macro_expansion
    };
  }

  read::source object_source(object_ref const o)
  {
    auto const meta(runtime::meta(o));
    if(meta == jank_nil())
    {
      return read::source::unknown();
    }
    auto source(meta_source(meta));
    if(source == read::source::unknown() || source.file == read::no_source_path)
    {
      auto const hint(meta_source_hint(meta));
      if(hint != read::source::unknown())
      {
        return hint;
      }
    }
    return source;
  }

  read::source meta_source_hint(object_ref const meta)
  {
    if(meta == jank_nil())
    {
      return read::source::unknown();
    }

    auto const hint(get(meta, __rt_ctx->intern_keyword("jank/source-hint").expect_ok()));
    if(hint == jank_nil())
    {
      return read::source::unknown();
    }

    return meta_source(hint);
  }

  read::source object_source_hint(object_ref const o)
  {
    auto const meta(runtime::meta(o));
    return meta_source_hint(meta);
  }

  obj::persistent_hash_map_ref source_to_meta(read::source const &source)
  {
    auto meta(build_source_meta(source));

    auto &hint_stack(source_hint_stack());
    if(!hint_stack.empty())
    {
      auto const hint_meta(build_source_meta(hint_stack.back()));
      auto const transient(meta->to_transient());
      transient->assoc_in_place(source_hint_keyword(), hint_meta);
      meta = transient->to_persistent();
    }

    return meta;
  }

  void push_source_hint(read::source const &source)
  {
    source_hint_stack().emplace_back(source);
  }

  void pop_source_hint()
  {
    auto &hint_stack(source_hint_stack());
    if(!hint_stack.empty())
    {
      hint_stack.pop_back();
    }
  }

  read::source current_source_hint()
  {
    auto &hint_stack(source_hint_stack());
    if(!hint_stack.empty())
    {
      return hint_stack.back();
    }
    return read::source::unknown();
  }

  namespace
  {
    constexpr size_t debug_trace_max_entries{ 32 };

    struct debug_trace_entry
    {
      char const *location{ nullptr };
      char const *file{ nullptr };
      int line{ 0 };
    };

    auto &debug_trace_stack()
    {
      thread_local std::vector<debug_trace_entry> stack;
      return stack;
    }
  }

  void debug_trace_push(char const *location, char const *file, int line)
  {
    auto &stack{ debug_trace_stack() };

    /* If no file provided, try to get it from the runtime context */
    char const *effective_file = file;
    int effective_line = line;

    if(!file && __rt_ctx)
    {
      static thread_local std::string file_buffer;
      auto const file_obj{ __rt_ctx->current_file_var->deref() };
      if(!file_obj.is_nil())
      {
        file_buffer = to_string(file_obj);
        effective_file = file_buffer.c_str();
      }
    }

    stack.push_back({ location, effective_file, effective_line });
    /* Keep only the last N entries to avoid memory growth */
    if(stack.size() > debug_trace_max_entries)
    {
      stack.erase(stack.begin());
    }
  }

  void debug_trace_pop()
  {
    /* Don't pop during exception unwinding - we want to preserve the trace! */
    if(std::uncaught_exceptions() > 0)
    {
      return;
    }
    auto &stack{ debug_trace_stack() };
    if(!stack.empty())
    {
      stack.pop_back();
    }
  }

  std::string debug_trace_dump()
  {
    auto &stack{ debug_trace_stack() };
    std::string result;
    result += "\n=== jank Execution Trace (most recent last) ===\n";
    for(size_t i = 0; i < stack.size(); ++i)
    {
      result += "  ";
      result += std::to_string(i);
      result += ": ";
      result += stack[i].location ? stack[i].location : "(null)";
      if(stack[i].file && stack[i].line > 0)
      {
        result += " @ ";
        result += stack[i].file;
        result += ":";
        result += std::to_string(stack[i].line);
      }
      result += "\n";
    }
    result += "=== End Execution Trace ===\n";
    return result;
  }

  obj::persistent_hash_map_ref
  source_to_meta(read::source_position const &start, read::source_position const &end)
  {
    read::source source{ start, end };

    auto const module{ runtime::to_code_string(runtime::__rt_ctx->current_ns_var->deref()) };
    source.module = module;

    auto const file{ runtime::__rt_ctx->current_file_var->deref() };
    source.file = runtime::to_string(file);

    return source_to_meta(source);
  }

  object_ref strip_source_from_meta(object_ref const meta)
  {
    auto const kw{ __rt_ctx->intern_keyword("jank/source").expect_ok() };
    return dissoc(meta, kw);
  }

  jtl::option<object_ref> strip_source_from_meta_opt(jtl::option<object_ref> const &meta)
  {
    if(meta.is_none())
    {
      return meta;
    }

    auto stripped{ strip_source_from_meta(meta.unwrap()) };
    if(is_empty(stripped))
    {
      return none;
    }
    return stripped;
  }
}
