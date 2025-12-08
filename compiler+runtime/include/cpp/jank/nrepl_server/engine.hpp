#pragma once

#include <algorithm>
#include <charconv>
#include <cctype>
#include <compare>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <typeinfo>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <jtl/immutable_string.hpp>
#include <jtl/immutable_string_view.hpp>

#include <jank/error.hpp>
#include <jank/runtime/behavior/callable.hpp>
#include <jank/runtime/context.hpp>
#include <jank/runtime/core.hpp>
#include <jank/runtime/core/meta.hpp>
#include <jank/runtime/core/to_string.hpp>
#include <jank/runtime/module/loader.hpp>
#include <jank/runtime/ns.hpp>
#include <jank/runtime/object.hpp>
#include <jank/runtime/obj/keyword.hpp>
#include <jank/runtime/obj/native_function_wrapper.hpp>
#include <jank/runtime/obj/nil.hpp>
#include <jank/runtime/obj/persistent_hash_map.hpp>
#include <jank/runtime/obj/persistent_string.hpp>
#include <jank/runtime/obj/symbol.hpp>
#include <jank/read/source.hpp>
#include <jank/runtime/rtti.hpp>
#include <jank/runtime/var.hpp>
#include <jank/util/scope_exit.hpp>
#include <jank/util/string.hpp>
#include <jank/analyze/cpp_util.hpp>
#include <jank/nrepl_server/native_header_index.hpp>
#include <jank/nrepl_server/native_header_completion.hpp>

#include <clang/AST/DeclBase.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/RawCommentList.h>
#include <clang/AST/Type.h>
#include <clang/Basic/SourceManager.h>
#include <llvm/Support/Casting.h>
#include <llvm/Demangle/Demangle.h>

#include <jank/jit/processor.hpp>

namespace Cpp
{
  class Interpreter;
}

namespace jank::nrepl_server::asio
{
  using namespace jank;
  using namespace jank::runtime;

  inline std::string to_std_string(jtl::immutable_string const &s)
  {
    return std::string{ s.begin(), s.end() };
  }

  inline std::string to_std_string(jtl::immutable_string_view const &s)
  {
    return std::string{ s.begin(), s.end() };
  }

  inline jtl::immutable_string make_immutable_string(std::string const &value)
  {
    return { value.data(), value.size() };
  }

  inline bool starts_with(std::string_view const value, std::string_view const prefix)
  {
    if(prefix.size() > value.size())
    {
      return false;
    }
    return std::equal(prefix.begin(), prefix.end(), value.begin());
  }

  inline void bootstrap_runtime_once()
  {
    static bool const bootstrapped = [] {
      __rt_ctx->load_module("/clojure.core", module::origin::latest).expect_ok();
      dynamic_call(__rt_ctx->in_ns_var->deref(), make_box<obj::symbol>("user"));
      dynamic_call(__rt_ctx->intern_var("clojure.core", "refer").expect_ok(),
                   make_box<obj::symbol>("clojure.core"));
      return true;
    }();
    (void)bootstrapped;
  }

  struct parsed_symbol
  {
    std::string ns;
    std::string name;
  };

  inline parsed_symbol parse_symbol(std::string const &raw)
  {
    parsed_symbol parts;
    auto const slash(raw.find('/'));
    if(slash == std::string::npos || slash == 0)
    {
      parts.name = raw;
      return parts;
    }

    parts.ns = raw.substr(0, slash);
    parts.name = raw.substr(slash + 1);
    return parts;
  }

  inline std::string strip_text_properties(std::string const &raw)
  {
    if(raw.size() > 3 && raw[0] == '#' && raw[1] == '(')
    {
      auto const open_quote(raw.find('"', 2));
      if(open_quote != std::string::npos)
      {
        auto const close_quote(raw.find('"', open_quote + 1));
        if(close_quote != std::string::npos && close_quote > open_quote)
        {
          return raw.substr(open_quote + 1, close_quote - open_quote - 1);
        }
      }
    }
    return raw;
  }

  namespace bencode
  {
    struct value
    {
      using list = std::vector<value>;
      using dict = std::map<std::string, value>;

      std::variant<std::int64_t, std::string, list, dict> data;

      value() = default;

      value(std::int64_t v)
        : data{ v }
      {
      }

      value(std::string v)
        : data{ std::move(v) }
      {
      }

      value(list v)
        : data{ std::move(v) }
      {
      }

      value(dict v)
        : data{ std::move(v) }
      {
      }

      bool is_string() const
      {
        return std::holds_alternative<std::string>(data);
      }

      bool is_list() const
      {
        return std::holds_alternative<list>(data);
      }

      bool is_dict() const
      {
        return std::holds_alternative<dict>(data);
      }

      bool is_integer() const
      {
        return std::holds_alternative<std::int64_t>(data);
      }

      std::string const &as_string() const
      {
        return std::get<std::string>(data);
      }

      std::int64_t as_integer() const
      {
        return std::get<std::int64_t>(data);
      }

      list const &as_list() const
      {
        return std::get<list>(data);
      }

      dict const &as_dict() const
      {
        return std::get<dict>(data);
      }
    };

    enum class parse_state : std::uint8_t
    {
      ok,
      need_more,
      error
    };

    struct decode_result
    {
      parse_state state{ parse_state::error };
      size_t consumed{};
      value data{};
      std::string error{};
    };

    inline parse_state
    decode_value(std::string_view input, size_t &offset, value &out, std::string &err)
    {
      if(offset >= input.size())
      {
        return parse_state::need_more;
      }

      auto const current(input[offset]);
      if(current == 'i')
      {
        ++offset;
        auto const end(input.find('e', offset));
        if(end == std::string_view::npos)
        {
          return parse_state::need_more;
        }

        std::int64_t value_out{};
        auto const res(std::from_chars(input.data() + offset, input.data() + end, value_out));
        if(res.ec != std::errc{} || res.ptr != input.data() + end)
        {
          err = "invalid integer";
          return parse_state::error;
        }
        out = value{ value_out };
        offset = end + 1;
        return parse_state::ok;
      }

      if(current == 'l')
      {
        ++offset;
        value::list list;
        while(true)
        {
          if(offset >= input.size())
          {
            return parse_state::need_more;
          }
          if(input[offset] == 'e')
          {
            ++offset;
            out = value{ std::move(list) };
            return parse_state::ok;
          }

          value element;
          auto const state(decode_value(input, offset, element, err));
          if(state != parse_state::ok)
          {
            return state;
          }
          list.emplace_back(std::move(element));
        }
      }

      if(current == 'd')
      {
        ++offset;
        value::dict dict;
        while(true)
        {
          if(offset >= input.size())
          {
            return parse_state::need_more;
          }
          if(input[offset] == 'e')
          {
            ++offset;
            out = value{ std::move(dict) };
            return parse_state::ok;
          }

          value key;
          auto state(decode_value(input, offset, key, err));
          if(state != parse_state::ok)
          {
            return state;
          }
          if(!key.is_string())
          {
            err = "dictionary key must be string";
            return parse_state::error;
          }

          value value_entry;
          state = decode_value(input, offset, value_entry, err);
          if(state != parse_state::ok)
          {
            return state;
          }
          dict.emplace(key.as_string(), std::move(value_entry));
        }
      }

      if(!std::isdigit(static_cast<unsigned char>(current)))
      {
        err = "unsupported token";
        return parse_state::error;
      }

      auto const colon(input.find(':', offset));
      if(colon == std::string_view::npos)
      {
        return parse_state::need_more;
      }

      std::int64_t size{};
      auto const size_res(std::from_chars(input.data() + offset, input.data() + colon, size));
      if(size_res.ec != std::errc{} || size_res.ptr != input.data() + colon || size < 0)
      {
        err = "invalid string length";
        return parse_state::error;
      }

      auto const remaining(input.size() - (colon + 1));
      auto const size_as_size(static_cast<std::size_t>(size));
      if(size_as_size > remaining)
      {
        return parse_state::need_more;
      }

      auto const start(colon + 1);
      out = value{ std::string{ input.substr(start, size_as_size) } };
      offset = start + size_as_size;
      return parse_state::ok;
    }

    inline decode_result decode(std::string_view input)
    {
      decode_result res;
      size_t offset{};
      auto const state(decode_value(input, offset, res.data, res.error));
      res.state = state;
      if(state == parse_state::ok)
      {
        res.consumed = offset;
      }
      return res;
    }

    inline void encode_string(std::string const &value, std::string &out)
    {
      out += std::to_string(value.size());
      out.push_back(':');
      out += value;
    }

    inline void encode_value(value const &val, std::string &out)
    {
      if(val.is_string())
      {
        encode_string(val.as_string(), out);
        return;
      }

      if(val.is_integer())
      {
        out.push_back('i');
        out += std::to_string(std::get<std::int64_t>(val.data));
        out.push_back('e');
        return;
      }

      if(val.is_list())
      {
        out.push_back('l');
        for(auto const &entry : val.as_list())
        {
          encode_value(entry, out);
        }
        out.push_back('e');
        return;
      }

      out.push_back('d');
      for(auto const &[key, entry] : val.as_dict())
      {
        encode_string(key, out);
        encode_value(entry, out);
      }
      out.push_back('e');
    }

    inline value list_of_strings(std::vector<std::string> const &items)
    {
      value::list list;
      list.reserve(items.size());
      for(auto const &item : items)
      {
        list.emplace_back(item);
      }
      return value{ std::move(list) };
    }

    inline value make_doc_value(std::string doc)
    {
      value::dict dict;
      dict.emplace("doc", value{ std::move(doc) });
      return value{ std::move(dict) };
    }
  }

  struct message
  {
    bencode::value::dict data;

    std::string get(std::string const &key, std::string default_value = {}) const
    {
      auto const found(data.find(key));
      if(found == data.end() || !found->second.is_string())
      {
        return default_value;
      }
      return found->second.as_string();
    }

    std::optional<std::int64_t> get_integer(std::string const &key) const
    {
      auto const found(data.find(key));
      if(found == data.end())
      {
        return std::nullopt;
      }

      auto const &val(found->second);
      if(val.is_integer())
      {
        return val.as_integer();
      }
      if(val.is_string())
      {
        auto const &str(val.as_string());
        if(str.empty())
        {
          return std::nullopt;
        }
        std::int64_t parsed{};
        auto const * const begin(str.data());
        auto const * const end(str.data() + str.size());
        auto const result(std::from_chars(begin, end, parsed));
        if(result.ec == std::errc{} && result.ptr == end)
        {
          return parsed;
        }
      }
      return std::nullopt;
    }

    std::string id() const
    {
      return get("id");
    }

    std::string op() const
    {
      return get("op");
    }

    std::string session() const
    {
      return get("session");
    }

    bencode::value const *find_value(std::string const &key) const
    {
      auto const found(data.find(key));
      if(found == data.end())
      {
        return nullptr;
      }
      return &found->second;
    }
  };

  class engine
  {
  public:
    struct serialized_note
    {
      serialized_note(std::string message, read::source source, std::string kind)
        : message{ std::move(message) }
        , source{ std::move(source) }
        , kind{ std::move(kind) }
      {
      }

      std::string message;
      read::source source;
      std::string kind;
    };

    struct serialized_error
    {
      serialized_error(std::string kind, std::string message, read::source source)
        : kind{ std::move(kind) }
        , message{ std::move(message) }
        , source{ std::move(source) }
      {
      }

      serialized_error(serialized_error const &) = default;
      serialized_error(serialized_error &&) noexcept = default;
      serialized_error &operator=(serialized_error const &) = default;
      serialized_error &operator=(serialized_error &&) noexcept = default;

      std::string kind;
      std::string message;
      read::source source;
      std::vector<serialized_note> notes;
      std::vector<serialized_error> causes;
    };

    struct session_state
    {
      std::string id;
      object_ref current_ns;
      bool forward_system_output{ false };
      std::string stdin_buffer;
      std::string active_request_id;
      bool running_eval{ false };
      std::optional<std::string> last_exception_message;
      std::optional<std::string> last_exception_type;
      std::optional<read::source> last_exception_source;
      std::optional<serialized_error> last_exception_details;
    };

    engine()
    {
      bootstrap_runtime_once();
      auto const version_var(__rt_ctx->intern_var("clojure.core", "jank-version"));
      if(version_var.is_ok())
      {
        auto const value(dynamic_call(version_var.expect_ok()->deref()));
        version_ = to_std_string(runtime::to_string(value));
      }
      else
      {
        version_ = "unknown";
      }
    }

    std::vector<bencode::value::dict> handle(message const &msg)
    {
      auto const op(msg.op());
      if(op.empty())
      {
        return handle_unsupported(msg, "missing-op");
      }

      if(op == "clone")
      {
        return handle_clone(msg);
      }
      if(op == "describe")
      {
        return handle_describe(msg);
      }
      if(op == "ls-sessions")
      {
        return handle_ls_sessions(msg);
      }
      if(op == "close")
      {
        return handle_close(msg);
      }
      if(op == "eval")
      {
        return handle_eval(msg);
      }
      if(op == "load-file")
      {
        return handle_load_file(msg);
      }
      if(op == "completions")
      {
        return handle_completions(msg);
      }
      if(op == "complete")
      {
        return handle_complete(msg);
      }
      if(op == "lookup")
      {
        return handle_lookup(msg);
      }
      if(op == "info")
      {
        return handle_info(msg);
      }
      if(op == "eldoc")
      {
        return handle_eldoc(msg);
      }
      if(op == "forward-system-output")
      {
        return handle_forward_system_output(msg);
      }
      if(op == "interrupt")
      {
        return handle_interrupt(msg);
      }
      if(op == "ls-middleware")
      {
        return handle_ls_middleware(msg);
      }
      if(op == "add-middleware")
      {
        return handle_add_middleware(msg);
      }
      if(op == "swap-middleware")
      {
        return handle_swap_middleware(msg);
      }
      if(op == "stdin")
      {
        return handle_stdin(msg);
      }
      if(op == "caught")
      {
        return handle_caught(msg);
      }
      if(op == "analyze-last-stacktrace")
      {
        return handle_analyze_last_stacktrace(msg);
      }
      if(op == "wasm-compile-patch")
      {
        return handle_wasm_compile_patch(msg);
      }

      return handle_unsupported(msg, "unknown-op");
    }

  private:
    std::unordered_map<std::string, session_state> sessions_;
    std::string default_session_id_;
    std::vector<std::string> middleware_stack_{
      "nrepl.middleware.session/session",
      "nrepl.middleware.caught/wrap-caught",
      "nrepl.middleware.print/wrap-print",
      "nrepl.middleware.interruptible-eval/interruptible-eval",
      "nrepl.middleware.load-file/wrap-load-file",
      "nrepl.middleware.completion/wrap-completion",
      "nrepl.middleware.lookup/wrap-lookup",
      "nrepl.middleware.dynamic-loader/wrap-dynamic-loader",
      "nrepl.middleware.io/wrap-out",
      "nrepl.middleware.session/add-stdin"
    };
    std::string version_;
    mutable native_header_index native_header_index_;

    session_state &ensure_session(std::string session_id)
    {
      if(session_id.empty())
      {
        if(default_session_id_.empty())
        {
          default_session_id_ = next_session_id();
        }
        session_id = default_session_id_;
      }
      else if(default_session_id_.empty())
      {
        default_session_id_ = session_id;
      }

      auto const [it, inserted](sessions_.try_emplace(session_id));
      if(inserted)
      {
        it->second.id = session_id;
        auto const user_symbol(make_box<obj::symbol>(make_immutable_string("user")));
        it->second.current_ns = __rt_ctx->intern_ns(user_symbol);
      }
      return it->second;
    }

    std::vector<bencode::value::dict> handle_clone(message const &msg);

    std::vector<bencode::value::dict> handle_describe(message const &msg);

    std::vector<bencode::value::dict> handle_ls_sessions(message const &msg);

    std::vector<bencode::value::dict> handle_close(message const &msg);

    std::vector<bencode::value::dict> handle_eval(message const &msg);

    std::vector<bencode::value::dict> handle_load_file(message const &msg);

    std::vector<bencode::value::dict> handle_completions(message const &msg);

    std::vector<bencode::value::dict> handle_complete(message const &msg);

    std::vector<bencode::value::dict> handle_lookup(message const &msg);

    std::vector<bencode::value::dict> handle_info(message const &msg);

    std::vector<bencode::value::dict> handle_eldoc(message const &msg);

    std::vector<bencode::value::dict> handle_forward_system_output(message const &msg);

    std::vector<bencode::value::dict> handle_caught(message const &msg);

    std::vector<bencode::value::dict> handle_analyze_last_stacktrace(message const &msg);

    std::vector<bencode::value::dict> handle_interrupt(message const &msg);

    std::vector<bencode::value::dict> handle_ls_middleware(message const &msg);

    std::vector<bencode::value::dict> handle_add_middleware(message const &msg);

    std::vector<bencode::value::dict> handle_swap_middleware(message const &msg);

    std::vector<bencode::value::dict> handle_stdin(message const &msg);

    std::vector<bencode::value::dict> handle_wasm_compile_patch(message const &msg);

    std::vector<bencode::value::dict>
    handle_unsupported(message const &msg, std::string_view reason)
    {
      bencode::value::dict payload;
      if(!msg.id().empty())
      {
        payload.emplace("id", msg.id());
      }
      if(!msg.session().empty())
      {
        payload.emplace("session", msg.session());
      }
      payload.emplace("status", bencode::list_of_strings({ "unsupported", "done" }));
      payload.emplace("err", std::string{ reason });
      return { std::move(payload) };
    }

    std::string next_session_id() const
    {
      auto const uuid(runtime::random_uuid());
      return to_std_string(runtime::to_string(uuid));
    }

    ns_ref resolve_namespace(session_state &session, std::string const &requested_ns) const
    {
      auto target_ns(expect_object<ns>(session.current_ns));
      if(requested_ns.empty())
      {
        return target_ns;
      }

      auto const symbol(make_box<obj::symbol>(make_immutable_string(requested_ns)));
      if(auto const alias = target_ns->find_alias(symbol); alias.is_some())
      {
        return alias;
      }

      if(auto const found = __rt_ctx->find_ns(symbol); found.is_some())
      {
        return found;
      }

      return target_ns;
    }

    std::optional<ns::native_alias>
    find_native_alias(ns_ref context_ns, std::string const &alias_name) const
    {
      if(alias_name.empty())
      {
        return std::nullopt;
      }

      auto const alias_sym(make_box<obj::symbol>(make_immutable_string(alias_name)));
      auto const native_alias_opt(context_ns->find_native_alias(alias_sym));
      if(native_alias_opt.is_none())
      {
        return std::nullopt;
      }

      return native_alias_opt.unwrap();
    }

    std::string current_ns_name(object_ref const ns_obj) const
    {
      if(ns_obj->type != object_type::ns)
      {
        return "user";
      }
      return to_std_string(expect_object<ns>(ns_obj)->name->to_string());
    }

    serialized_error build_serialized_error(error_ref const &error) const
    {
      serialized_error data{ std::string{ error::kind_str(error->kind) },
                             to_std_string(error->message),
                             error->source };
      data.notes.reserve(error->notes.size());
      for(auto const &note : error->notes)
      {
        data.notes.emplace_back(to_std_string(note.message),
                                note.source,
                                std::string{ error::note::kind_str(note.kind) });
      }
      std::ranges::stable_sort(data.notes,
                               [](serialized_note const &lhs, serialized_note const &rhs) {
                                 auto const lhs_unknown(lhs.source == read::source::unknown);
                                 auto const rhs_unknown(rhs.source == read::source::unknown);
                                 if(lhs_unknown != rhs_unknown)
                                 {
                                   return !lhs_unknown;
                                 }
                                 if(lhs.source.file != rhs.source.file)
                                 {
                                   return lhs.source.file < rhs.source.file;
                                 }
                                 if(lhs.source.start.line != rhs.source.start.line)
                                 {
                                   return lhs.source.start.line < rhs.source.start.line;
                                 }
                                 return lhs.source.start.col < rhs.source.start.col;
                               });
      if(error->cause)
      {
        data.causes.emplace_back(build_serialized_error(error->cause.as_ref()));
      }
      return data;
    }

    bencode::value::dict encode_source(read::source const &source) const
    {
      bencode::value::dict dict;
      auto const file_value(to_std_string(source.file));
      if(file_value != jank::read::no_source_path)
      {
        dict.emplace("file", file_value);
      }
      auto const module_value(to_std_string(source.module));
      if(module_value != jank::read::no_source_path)
      {
        dict.emplace("module", module_value);
      }
      if(source.start.line != 0)
      {
        dict.emplace("line", static_cast<std::int64_t>(source.start.line));
      }
      if(source.start.col != 0)
      {
        dict.emplace("column", static_cast<std::int64_t>(source.start.col));
      }
      if(source.end.line != 0)
      {
        dict.emplace("end-line", static_cast<std::int64_t>(source.end.line));
      }
      if(source.end.col != 0)
      {
        dict.emplace("end-column", static_cast<std::int64_t>(source.end.col));
      }
      if(source.start.offset != 0)
      {
        dict.emplace("start-offset", static_cast<std::int64_t>(source.start.offset));
      }
      if(source.end.offset != 0)
      {
        dict.emplace("end-offset", static_cast<std::int64_t>(source.end.offset));
      }
      return dict;
    }

    bencode::value::dict encode_note(serialized_note const &note) const
    {
      bencode::value::dict dict;
      dict.emplace("message", bencode::value{ note.message });
      dict.emplace("kind", bencode::value{ note.kind });
      auto source_dict(encode_source(note.source));
      if(!source_dict.empty())
      {
        dict.emplace("source", bencode::value{ std::move(source_dict) });
      }
      return dict;
    }

    bencode::value::dict encode_error(serialized_error const &error) const
    {
      bencode::value::dict dict;
      dict.emplace("kind", bencode::value{ error.kind });
      dict.emplace("message", bencode::value{ error.message });
      auto source_dict(encode_source(error.source));
      if(!source_dict.empty())
      {
        dict.emplace("source", bencode::value{ std::move(source_dict) });
      }
      if(!error.notes.empty())
      {
        bencode::value::list notes;
        notes.reserve(error.notes.size());
        for(auto const &note : error.notes)
        {
          notes.emplace_back(encode_note(note));
        }
        dict.emplace("notes", bencode::value{ std::move(notes) });
      }
      if(!error.causes.empty())
      {
        bencode::value::list causes;
        causes.reserve(error.causes.size());
        for(auto const &cause : error.causes)
        {
          causes.emplace_back(encode_error(cause));
        }
        dict.emplace("causes", bencode::value{ std::move(causes) });
      }
      return dict;
    }

    std::string_view describe_error_phase(error::kind const kind) const
    {
      using underlying = std::underlying_type_t<error::kind>;
      auto const value(static_cast<underlying>(kind));
      constexpr auto parse_max = static_cast<underlying>(error::kind::internal_parse_failure);
      if(value <= parse_max)
      {
        return "Syntax error reading source";
      }
      constexpr auto analyze_max = static_cast<underlying>(error::kind::internal_analyze_failure);
      if(value <= analyze_max)
      {
        return "Syntax error compiling";
      }
      constexpr auto codegen_max = static_cast<underlying>(error::kind::internal_codegen_failure);
      if(value <= codegen_max)
      {
        return "Compilation error";
      }
      constexpr auto aot_max = static_cast<underlying>(error::kind::internal_aot_failure);
      if(value <= aot_max)
      {
        return "Compilation error";
      }
      constexpr auto system_max = static_cast<underlying>(error::kind::system_failure);
      if(value <= system_max)
      {
        return "System error";
      }
      constexpr auto runtime_max = static_cast<underlying>(error::kind::internal_runtime_failure);
      if(value <= runtime_max)
      {
        return "Execution error";
      }
      return "Internal error";
    }

    std::string
    format_source_location(read::source const &source, std::string const &path_hint) const
    {
      auto const file_value(to_std_string(source.file));
      std::string location;
      if(file_value != jank::read::no_source_path)
      {
        location = file_value;
      }
      else if(!path_hint.empty())
      {
        location = path_hint;
      }

      if(source.start.line != 0)
      {
        if(!location.empty())
        {
          location.push_back(':');
        }
        location += std::to_string(source.start.line);
        if(source.start.col != 0)
        {
          location.push_back(':');
          location += std::to_string(source.start.col);
        }
      }
      return location;
    }

    std::string format_error_with_location(std::string const &message,
                                           error::kind const kind,
                                           read::source const &source,
                                           std::string const &path_hint) const
    {
      auto const location(format_source_location(source, path_hint));
      if(location.empty())
      {
        return message;
      }

      auto const prefix(describe_error_phase(kind));
      if(prefix.empty())
      {
        return message;
      }

      std::string formatted;
      formatted.reserve(prefix.size() + location.size() + message.size() + 5);
      formatted.append(prefix);
      formatted.append(" at (");
      formatted.append(location);
      formatted.append(").\n");
      formatted.append(message);
      return formatted;
    }

    void apply_source_hints(read::source &source,
                            std::optional<std::size_t> const &line_hint,
                            std::optional<std::size_t> const &column_hint) const
    {
      if(!line_hint && !column_hint)
      {
        return;
      }

      auto adjust_position = [&](read::source_position &position) {
        auto const relative_line(position.line);
        if(relative_line == 0)
        {
          return;
        }

        if(line_hint && *line_hint > 0)
        {
          position.line += *line_hint - 1;
        }

        if(column_hint && relative_line == 1)
        {
          if(position.col != 0)
          {
            position.col += *column_hint - 1;
          }
        }
      };

      adjust_position(source.start);
      adjust_position(source.end);
    }

    void apply_serialized_error_hints(serialized_error &error,
                                      std::optional<std::size_t> const &line_hint,
                                      std::optional<std::size_t> const &column_hint) const
    {
      if(!line_hint && !column_hint)
      {
        return;
      }

      apply_source_hints(error.source, line_hint, column_hint);
      for(auto &note : error.notes)
      {
        apply_source_hints(note.source, line_hint, column_hint);
      }
      for(auto &cause : error.causes)
      {
        apply_serialized_error_hints(cause, line_hint, column_hint);
      }
    }

    std::string make_file_url(std::string const &path) const
    {
      if(path.empty())
      {
        return {};
      }
      if(starts_with(path, "file:"))
      {
        return path;
      }
      return "file:" + path;
    }

    std::string deduce_phase_from_error(std::string const &kind) const
    {
      if(starts_with(kind, "lex/") || starts_with(kind, "parse/"))
      {
        return "read-source";
      }
      if(starts_with(kind, "analyze/"))
      {
        return "compile-syntax-check";
      }
      if(starts_with(kind, "runtime/"))
      {
        return "execution";
      }
      if(starts_with(kind, "aot/") || kind == "internal/codegen-failure")
      {
        return "compile";
      }
      if(starts_with(kind, "system/"))
      {
        return "system";
      }
      return {};
    }

    std::string escape_for_edn(std::string const &value) const
    {
      std::string escaped;
      escaped.reserve(value.size());
      for(char const ch : value)
      {
        switch(ch)
        {
          case '\\':
          case '"':
            escaped.push_back('\\');
            escaped.push_back(ch);
            break;
          case '\n':
            escaped.append("\\n");
            break;
          case '\r':
            escaped.append("\\r");
            break;
          case '\t':
            escaped.append("\\t");
            break;
          default:
            escaped.push_back(ch);
            break;
        }
      }
      return escaped;
    }

    std::string format_error_data(serialized_error const &error,
                                  read::source const *source,
                                  std::string const &phase) const
    {
      std::string data{ "{:jank/error-kind \"" };
      data += escape_for_edn(error.kind);
      data += "\" :jank/error-message \"";
      data += escape_for_edn(error.message);
      data += "\"";
      if(!phase.empty())
      {
        data += " :clojure.error/phase :";
        data += phase;
      }
      if(source != nullptr)
      {
        auto const file_value(to_std_string(source->file));
        if(file_value != jank::read::no_source_path)
        {
          data += " :clojure.error/source \"";
          data += escape_for_edn(file_value);
          data += "\"";
        }
        if(source->start.line != 0)
        {
          data += " :clojure.error/line ";
          data += std::to_string(source->start.line);
        }
        if(source->start.col != 0)
        {
          data += " :clojure.error/column ";
          data += std::to_string(source->start.col);
        }
      }
      data.push_back('}');
      return data;
    }

    bencode::value::dict build_location(read::source const *source, std::string const &phase) const
    {
      bencode::value::dict dict;
      if(source != nullptr)
      {
        auto const file_value(to_std_string(source->file));
        if(file_value != jank::read::no_source_path)
        {
          dict.emplace("clojure.error/source", file_value);
        }
        if(source->start.line != 0)
        {
          dict.emplace("clojure.error/line", static_cast<std::int64_t>(source->start.line));
        }
        if(source->start.col != 0)
        {
          dict.emplace("clojure.error/column", static_cast<std::int64_t>(source->start.col));
        }
      }
      if(!phase.empty())
      {
        dict.emplace("clojure.error/phase", phase);
      }
      return dict;
    }

    void append_stacktrace_frames(serialized_error const &error, bencode::value::list &frames) const
    {
      bencode::value::dict frame;
      frame.emplace("class", error.kind);
      frame.emplace("message", error.message);
      frame.emplace("type", std::string{ "jank" });
      frame.emplace("method", error.kind);
      frame.emplace("name", error.kind);
      auto const file_value(to_std_string(error.source.file));
      if(file_value != jank::read::no_source_path)
      {
        frame.emplace("file", file_value);
        frame.emplace("file-url", make_file_url(file_value));
      }
      if(error.source.start.line != 0)
      {
        frame.emplace("line", static_cast<std::int64_t>(error.source.start.line));
      }
      if(error.source.start.col != 0)
      {
        frame.emplace("column", static_cast<std::int64_t>(error.source.start.col));
      }
      bencode::value::list flags;
      flags.emplace_back("jank");
      frame.emplace("flags", bencode::value{ std::move(flags) });
      frames.emplace_back(std::move(frame));

      for(auto const &cause : error.causes)
      {
        append_stacktrace_frames(cause, frames);
      }
    }

    bencode::value::list build_stacktrace(serialized_error const &error) const
    {
      bencode::value::list frames;
      append_stacktrace_frames(error, frames);
      return frames;
    }

    void clear_last_exception(session_state &session)
    {
      session.last_exception_message.reset();
      session.last_exception_type.reset();
      session.last_exception_source.reset();
      session.last_exception_details.reset();
    }

    void record_exception(session_state &session,
                          std::string message,
                          std::string type,
                          std::optional<read::source> source = std::nullopt,
                          std::optional<serialized_error> detailed = std::nullopt)
    {
      session.last_exception_message = std::move(message);
      session.last_exception_type = std::move(type);
      session.last_exception_source = std::move(source);
      session.last_exception_details = std::move(detailed);
    }

    bool is_private_var(var_ref const &var) const
    {
      if(var.is_nil() || var->meta.is_none())
      {
        return false;
      }

      auto const meta_obj(var->meta.unwrap());
      auto const private_kw(__rt_ctx->intern_keyword("private").expect_ok());
      auto const private_flag(runtime::get(meta_obj, private_kw));
      if(private_flag == jank_nil)
      {
        return false;
      }
      return runtime::truthy(private_flag);
    }

    bool is_public_var_in_namespace(ns_ref target_ns, var_ref const &var) const
    {
      if(var.is_nil())
      {
        return false;
      }

      if(var->n != target_ns)
      {
        return false;
      }

      return !is_private_var(var);
    }

    struct completion_candidate
    {
      std::string symbol_name;
      std::string display_name;
    };

    struct completion_query
    {
      ns_ref target_ns;
      std::string prefix;
      std::optional<std::string> qualifier;
      std::optional<ns::native_alias> native_alias;
    };

    struct var_documentation
    {
      struct cpp_argument
      {
        std::string name;
        std::string type;
      };

      struct cpp_signature
      {
        std::string return_type;
        std::vector<cpp_argument> arguments;
      };

      struct cpp_field
      {
        std::string name;
        std::string type;
      };

      std::string ns_name;
      std::string name;
      std::optional<std::string> doc;
      std::vector<std::string> arglists;
      std::optional<std::string> arglists_str;
      std::optional<std::string> file;
      std::optional<std::int64_t> line;
      std::optional<std::int64_t> column;
      std::optional<std::string> return_type;
      std::vector<cpp_signature> cpp_signatures;
      std::vector<cpp_field> cpp_fields;
      bool is_macro{ false };
      bool is_function{ false };
      bool is_cpp_function{ false };
      bool is_cpp_type{ false };
      bool is_cpp_constructor{ false };
      bool is_cpp_macro{ false };
      bool is_cpp_variable{ false };
    };

    std::string completion_type_for(var_documentation const &info) const
    {
      if(info.is_cpp_constructor)
      {
        return std::string{ "constructor" };
      }
      if(info.is_cpp_type)
      {
        return std::string{ "type" };
      }
      if(info.is_cpp_function)
      {
        return std::string{ "function" };
      }
      if(info.is_cpp_macro)
      {
        return std::string{ "macro" };
      }
      if(info.is_cpp_variable)
      {
        return std::string{ "variable" };
      }
      if(info.is_macro)
      {
        return std::string{ "macro" };
      }
      return std::string{ "var" };
    }

    std::string cpp_record_kind_to_string(runtime::context::cpp_record_kind kind) const
    {
      switch(kind)
      {
        case runtime::context::cpp_record_kind::Class:
          return std::string{ "class" };
        case runtime::context::cpp_record_kind::Union:
          return std::string{ "union" };
        case runtime::context::cpp_record_kind::Struct:
        default:
          return std::string{ "struct" };
      }
    }

    struct cpp_decl_metadata
    {
      std::optional<std::string> doc;
      std::optional<std::string> file;
      std::optional<std::int64_t> line;
      std::optional<std::int64_t> column;
    };

    // Helper to extract comment lines from source text preceding a declaration
    std::optional<std::string>
    extract_preceding_comments(clang::SourceManager const &src_mgr, clang::SourceLocation loc) const
    {
      if(!loc.isValid())
      {
        return std::nullopt;
      }

      // Get the file and offset for this location
      auto const file_id = src_mgr.getFileID(loc);
      auto const file_offset = src_mgr.getFileOffset(loc);

      // Get the buffer for this file
      bool invalid = false;
      auto const buffer = src_mgr.getBufferData(file_id, &invalid);
      if(invalid || buffer.empty() || file_offset == 0)
      {
        return std::nullopt;
      }

      // Search backwards from the declaration to find comment lines
      // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage): size is explicitly provided
      std::string_view const before_decl(buffer.data(), file_offset);

      // Find the start of the current line
      auto line_start = before_decl.rfind('\n');
      if(line_start == std::string_view::npos)
      {
        line_start = 0;
      }
      else
      {
        line_start++; // Move past the newline
      }

      // Now search backwards for comment lines
      std::vector<std::string> comment_lines;
      size_t search_pos = line_start;

      while(search_pos > 0)
      {
        // Find the previous line
        auto prev_line_end = search_pos - 1; // Points to the '\n' before current line
        if(prev_line_end == 0)
        {
          break;
        }

        auto prev_line_start = before_decl.rfind('\n', prev_line_end - 1);
        if(prev_line_start == std::string_view::npos)
        {
          prev_line_start = 0;
        }
        else
        {
          prev_line_start++; // Move past the newline
        }

        std::string_view const prev_line
          = before_decl.substr(prev_line_start, prev_line_end - prev_line_start);

        // Trim leading whitespace
        auto content_start = prev_line.find_first_not_of(" \t");
        if(content_start == std::string_view::npos)
        {
          // Empty line - stop searching
          break;
        }

        std::string_view const trimmed = prev_line.substr(content_start);

        // Check if it's a comment line
        if(trimmed.starts_with("//") || trimmed.starts_with("/*") || trimmed.starts_with("*"))
        {
          comment_lines.emplace_back(prev_line);
          search_pos = prev_line_start;
        }
        else
        {
          // Not a comment line - stop searching
          break;
        }
      }

      if(comment_lines.empty())
      {
        return std::nullopt;
      }

      // Reverse to get comments in correct order (top to bottom)
      std::string result;
      for(auto const &comment_line : std::ranges::reverse_view(comment_lines))
      {
        if(!result.empty())
        {
          result.push_back('\n');
        }
        result += comment_line;
      }

      return result;
    }

    // Helper to extract trailing inline comment on the same line as a declaration
    // This handles raylib-style comments like:
    // RLAPI void InitWindow(int w, int h, const char* title);  // Initialize window
    std::optional<std::string>
    extract_trailing_comment(clang::SourceManager const &src_mgr, clang::SourceLocation end_loc) const
    {
      if(!end_loc.isValid())
      {
        return std::nullopt;
      }

      // Get the file and offset for the end location
      auto const file_id = src_mgr.getFileID(end_loc);
      auto const file_offset = src_mgr.getFileOffset(end_loc);

      // Get the buffer for this file
      bool invalid = false;
      auto const buffer = src_mgr.getBufferData(file_id, &invalid);
      if(invalid || buffer.empty() || file_offset >= buffer.size())
      {
        return std::nullopt;
      }

      // Search forward from the end of the declaration to find a // comment on the same line
      std::string_view const after_decl(buffer.data() + file_offset, buffer.size() - file_offset);

      // Find the end of the current line
      auto const line_end = after_decl.find('\n');
      std::string_view const rest_of_line
        = (line_end == std::string_view::npos) ? after_decl : after_decl.substr(0, line_end);

      // Look for // comment marker
      auto const comment_start = rest_of_line.find("//");
      if(comment_start == std::string_view::npos)
      {
        return std::nullopt;
      }

      // Extract the comment text after //
      auto comment_text = rest_of_line.substr(comment_start + 2);

      // Trim leading whitespace
      while(!comment_text.empty() && (comment_text.front() == ' ' || comment_text.front() == '\t'))
      {
        comment_text.remove_prefix(1);
      }

      // Trim trailing whitespace
      while(!comment_text.empty() && (comment_text.back() == ' ' || comment_text.back() == '\t'))
      {
        comment_text.remove_suffix(1);
      }

      if(comment_text.empty())
      {
        return std::nullopt;
      }

      return std::string(comment_text);
    }

    cpp_decl_metadata extract_cpp_decl_metadata(void *fn) const
    {
      cpp_decl_metadata result;
      if(!fn)
      {
        return result;
      }

      auto const *decl = static_cast<clang::Decl const *>(fn);
      auto &ast_ctx = decl->getASTContext();
      auto &src_mgr = ast_ctx.getSourceManager();

      // First try to get a Doxygen-style documentation comment
      if(auto const *raw_comment = ast_ctx.getRawCommentForDeclNoCache(decl))
      {
        result.doc = raw_comment->getRawText(src_mgr).str();
      }
      else
      {
        // Try to extract trailing inline comment (raylib-style)
        // e.g., void InitWindow(int w, int h, const char* title);  // Initialize window
        result.doc = extract_trailing_comment(src_mgr, decl->getEndLoc());

        // Fall back to extracting any comment preceding the declaration
        if(!result.doc.has_value())
        {
          result.doc = extract_preceding_comments(src_mgr, decl->getBeginLoc());
        }
      }

      // Extract source location
      auto const loc = decl->getLocation();
      if(loc.isValid())
      {
        auto const presumed = src_mgr.getPresumedLoc(loc);
        if(presumed.isValid())
        {
          std::string_view const filename(presumed.getFilename());
          // Skip internal Clang interpreter file names like "input_line_N"
          // These are not useful for users - they're virtual files from the REPL
          if(!filename.starts_with("input_line_"))
          {
            // Try to get the full path from the file entry
            auto const file_id = src_mgr.getFileID(loc);
            if(auto const file_entry = src_mgr.getFileEntryRefForID(file_id))
            {
              auto const real_path = file_entry->getFileEntry().tryGetRealPathName();
              if(!real_path.empty())
              {
                result.file = real_path.str();
              }
              else
              {
                // Fall back to the filename from the file entry
                result.file = file_entry->getName().str();
              }
            }
            else
            {
              // Fall back to the presumed filename
              result.file = std::string(filename);
            }
            result.line = presumed.getLine();
            result.column = presumed.getColumn();
          }
        }
      }

      return result;
    }

    bencode::value::dict make_done_response(std::string const &session,
                                            std::string const &id,
                                            std::vector<std::string> const &statuses) const
    {
      bencode::value::dict payload;
      if(!id.empty())
      {
        payload.emplace("id", id);
      }
      if(!session.empty())
      {
        payload.emplace("session", session);
      }
      payload.emplace("status", bencode::list_of_strings(statuses));
      return payload;
    }

    bencode::value::dict make_eval_error_response(std::string const &session,
                                                  std::string const &id,
                                                  std::string const &ex,
                                                  std::string const &root_ex) const
    {
      bencode::value::dict payload;
      if(!id.empty())
      {
        payload.emplace("id", id);
      }
      if(!session.empty())
      {
        payload.emplace("session", session);
      }
      payload.emplace("ex", ex);
      payload.emplace("root-ex", root_ex);
      payload.emplace("status", bencode::list_of_strings({ "eval-error" }));
      return payload;
    }

    completion_query prepare_completion_query(session_state &session,
                                              std::string prefix,
                                              std::string requested_ns,
                                              std::string const &raw_symbol) const
    {
      completion_query query;
      auto const &symbol_source
        = prefix.find('/') != std::string::npos || raw_symbol.empty() ? prefix : raw_symbol;
      auto const parts(parse_symbol(symbol_source));
      if(!parts.ns.empty())
      {
        requested_ns = parts.ns;
        prefix = parts.name;
        query.qualifier = parts.ns;
      }
      query.target_ns = resolve_namespace(session, requested_ns);
      query.prefix = prefix;
      if(query.qualifier.has_value())
      {
        auto const context_ns(expect_object<ns>(session.current_ns));
        query.native_alias = find_native_alias(context_ns, query.qualifier.value());
      }
      return query;
    }

    std::vector<std::string> collect_symbol_names(completion_query const &query) const
    {
      std::vector<std::string> matches;
      auto const &prefix(query.prefix);
      if(query.native_alias.has_value())
      {
        return native_header_index_.list_functions(query.native_alias.value(), prefix);
      }

      auto const target_ns(query.target_ns);
      auto const owned_only(query.qualifier.has_value());
      auto const mappings(target_ns->get_mappings());
      if(mappings.is_some())
      {
        matches.reserve(static_cast<std::size_t>(mappings->count()));
        for(auto const &entry : mappings->data)
        {
          auto const key(entry.first);
          if(!key.is_some() || key->type != object_type::symbol)
          {
            continue;
          }

          if(owned_only)
          {
            auto const value(entry.second);
            if(!value.is_some() || value->type != object_type::var)
            {
              continue;
            }

            auto const var(expect_object<var>(value));
            if(!is_public_var_in_namespace(target_ns, var))
            {
              continue;
            }
          }

          auto const sym(expect_object<obj::symbol>(key));
          auto const candidate_name(to_std_string(sym->name));
          if(!prefix.empty() && !starts_with(candidate_name, prefix))
          {
            continue;
          }
          matches.push_back(candidate_name);
        }
      }

      // When there's no qualifier, also check native refers in the target namespace
      if(!query.qualifier.has_value())
      {
        auto const native_refers = target_ns->native_refers_snapshot();
        for(auto const &[sym, refer_info] : native_refers)
        {
          auto const candidate_name(to_std_string(sym->name));
          if(!prefix.empty() && !starts_with(candidate_name, prefix))
          {
            continue;
          }
          matches.push_back(candidate_name);
        }
      }

      if(query.qualifier.has_value() && query.qualifier.value() == "cpp")
      {
        auto const locked_globals{ __rt_ctx->global_cpp_functions.rlock() };
        for(auto const &[name, _] : *locked_globals)
        {
          auto const candidate_name(to_std_string(name));
          if(!prefix.empty() && !starts_with(candidate_name, prefix))
          {
            continue;
          }
          matches.push_back(candidate_name);
        }

        auto const locked_types{ __rt_ctx->global_cpp_types.rlock() };
        for(auto const &[name, _] : *locked_types)
        {
          auto const base_name(to_std_string(name));
          if(prefix.empty() || starts_with(base_name, prefix))
          {
            matches.push_back(base_name);
          }

          auto const ctor_name(base_name + ".");
          if(prefix.empty() || starts_with(ctor_name, prefix))
          {
            matches.push_back(ctor_name);
          }
        }

        auto const locked_vars{ __rt_ctx->global_cpp_variables.rlock() };
        for(auto const &[name, _] : *locked_vars)
        {
          auto const candidate_name(to_std_string(name));
          if(!prefix.empty() && !starts_with(candidate_name, prefix))
          {
            continue;
          }
          matches.push_back(candidate_name);
        }
      }
      else if(query.native_alias.has_value())
      {
        auto const native_matches
          = native_header_index_.list_functions(query.native_alias.value(), prefix);
        matches.insert(matches.end(), native_matches.begin(), native_matches.end());
      }

      std::ranges::sort(matches);
      auto const unique_end(std::ranges::unique(matches));
      matches.erase(unique_end.begin(), unique_end.end());
      return matches;
    }

    /* Collect all available namespace names for completion in require forms.
     * This includes both already-loaded namespaces and modules available on the module path. */
    std::vector<std::string> collect_available_namespaces(std::string const &prefix) const
    {
      std::vector<std::string> matches;

      /* First, collect loaded namespaces */
      {
        auto const locked_namespaces{ __rt_ctx->namespaces.rlock() };
        for(auto const &[sym, ns_val] : *locked_namespaces)
        {
          auto const ns_name(to_std_string(sym->to_string()));
          if(prefix.empty() || starts_with(ns_name, prefix))
          {
            matches.push_back(ns_name);
          }
        }
      }

      /* Then, collect available modules from the module path */
      for(auto const &[module_name, entry] : __rt_ctx->module_loader.entries)
      {
        auto const name(to_std_string(module_name));
        if(prefix.empty() || starts_with(name, prefix))
        {
          /* Check if we already have this namespace */
          if(std::ranges::find(matches, name) == matches.end())
          {
            matches.push_back(name);
          }
        }
      }

      std::ranges::sort(matches);
      auto const unique_end(std::ranges::unique(matches));
      matches.erase(unique_end.begin(), unique_end.end());
      return matches;
    }

    /* Check if the completion context indicates we're in a require form.
     * The context string typically contains surrounding code like "(__prefix__ns " or
     * patterns like ":require [__prefix__" */
    bool is_require_context(std::string const &context) const
    {
      if(context.empty())
      {
        return false;
      }

      /* Check for patterns that suggest require context:
       * - ":require" followed by our position
       * - "(ns " with require vectors
       * - "(require " with module names */
      return context.find(":require") != std::string::npos
        || context.find("(require ") != std::string::npos
        || context.find("(require\n") != std::string::npos
        || context.find("(require'") != std::string::npos;
    }

    /* Make namespace completion candidates */
    std::vector<completion_candidate> make_namespace_candidates(std::string const &prefix) const
    {
      auto names(collect_available_namespaces(prefix));
      std::vector<completion_candidate> candidates;
      candidates.reserve(names.size());
      for(auto const &name : names)
      {
        completion_candidate entry;
        entry.symbol_name = name;
        entry.display_name = name;
        candidates.emplace_back(std::move(entry));
      }
      return candidates;
    }

    std::vector<completion_candidate>
    make_completion_candidates(completion_query const &query) const
    {
      auto names(collect_symbol_names(query));
      std::vector<completion_candidate> candidates;
      candidates.reserve(names.size());
      for(auto const &name : names)
      {
        completion_candidate entry;
        entry.symbol_name = name;
        entry.display_name
          = query.qualifier.has_value() ? query.qualifier.value() + "/" + name : name;
        candidates.emplace_back(std::move(entry));
      }
      return candidates;
    }

    std::string normalize_metadata_key(std::string token) const
    {
      if(!token.empty() && token.front() == ':')
      {
        token.erase(token.begin());
      }
      std::ranges::transform(token, token.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      return token;
    }

    std::vector<std::string> render_sequence_strings(object_ref seq_obj) const
    {
      std::vector<std::string> values;
      if(seq_obj.is_nil())
      {
        return values;
      }

      auto current(runtime::seq(seq_obj));
      while(current != jank_nil)
      {
        auto const head(runtime::first(current));
        values.push_back(to_std_string(runtime::to_code_string(head)));
        current = runtime::next(current);
      }
      return values;
    }

    std::string join_with_newline(std::vector<std::string> const &items) const
    {
      if(items.empty())
      {
        return {};
      }

      std::string joined;
      joined.reserve(64 * items.size());
      for(size_t i{}; i < items.size(); ++i)
      {
        if(i != 0)
        {
          joined.push_back('\n');
        }
        joined += items[i];
      }
      return joined;
    }

    std::optional<var_documentation>
    describe_var(var_ref var, std::string const &display_name) const
    {
      if(var.is_nil())
      {
        return std::nullopt;
      }

      var_documentation info;
      /* Use the var's actual namespace, not the lookup namespace.
       * This way clojure.core/map shows as clojure.core/map, not my-ns/map */
      info.ns_name = to_std_string(var->n->name->to_string());
      if(!display_name.empty())
      {
        info.name = display_name;
      }
      else if(var->name.is_some())
      {
        info.name = to_std_string(var->name->name);
      }

      object_ref const var_obj{ var };
      auto const meta(runtime::meta(var_obj));
      if(meta != jank_nil)
      {
        auto const doc_kw(__rt_ctx->intern_keyword("doc").expect_ok());
        auto const arglists_kw(__rt_ctx->intern_keyword("arglists").expect_ok());
        auto const file_kw(__rt_ctx->intern_keyword("file").expect_ok());
        auto const line_kw(__rt_ctx->intern_keyword("line").expect_ok());
        auto const column_kw(__rt_ctx->intern_keyword("column").expect_ok());
        auto const macro_kw(__rt_ctx->intern_keyword("macro").expect_ok());

        if(auto const doc(runtime::get(meta, doc_kw)); doc != jank_nil)
        {
          info.doc = to_std_string(runtime::to_string(doc));
        }
        if(auto const arglists(runtime::get(meta, arglists_kw)); arglists != jank_nil)
        {
          // Arglists are typically quoted: '([x] [x y])
          // We need to unwrap the quote if present
          auto arglists_seq(arglists);

          // Try to get the first element to check if it's a quote
          auto first_elem(runtime::first(arglists_seq));
          if(first_elem != jank_nil && first_elem->type == object_type::symbol)
          {
            auto const sym(expect_object<obj::symbol>(first_elem));
            if(sym->name == "quote")
            {
              // Get the second element (the actual arglists)
              auto const rest(runtime::next(arglists_seq));
              if(rest != jank_nil)
              {
                arglists_seq = runtime::first(rest);
              }
            }
          }

          info.arglists = render_sequence_strings(arglists_seq);
          if(!info.arglists.empty())
          {
            info.arglists_str = join_with_newline(info.arglists);
          }
        }
        // Try standard Clojure metadata keys first
        if(auto const file(runtime::get(meta, file_kw)); file != jank_nil)
        {
          info.file = to_std_string(runtime::to_string(file));
        }
        if(auto const line(runtime::get(meta, line_kw)); line != jank_nil)
        {
          info.line = runtime::to_int(line);
        }
        if(auto const column(runtime::get(meta, column_kw)); column != jank_nil)
        {
          info.column = runtime::to_int(column);
        }

        // Also check jank-specific :jank/source metadata
        // Format: {:jank/source {:start {:line 20, :col 7, :offset 260}, :end {...}}}
        auto const jank_source_kw(__rt_ctx->intern_keyword("jank", "source").expect_ok());
        if(auto const jank_source(runtime::get(meta, jank_source_kw)); jank_source != jank_nil)
        {
          auto const start_kw(__rt_ctx->intern_keyword("start").expect_ok());
          if(auto const start_map(runtime::get(jank_source, start_kw)); start_map != jank_nil)
          {
            // Extract line from :start map
            if(!info.line.has_value())
            {
              if(auto const start_line(runtime::get(start_map, line_kw)); start_line != jank_nil)
              {
                info.line = runtime::to_int(start_line);
              }
            }
            // Extract column from :start map (note: jank uses :col, not :column)
            if(!info.column.has_value())
            {
              auto const col_kw(__rt_ctx->intern_keyword("col").expect_ok());
              if(auto const start_col(runtime::get(start_map, col_kw)); start_col != jank_nil)
              {
                info.column = runtime::to_int(start_col);
              }
            }
          }
        }
        if(auto const macro_flag(runtime::get(meta, macro_kw)); macro_flag != jank_nil)
        {
          info.is_macro = runtime::truthy(macro_flag);
        }
      }

      // Check if the var's value is actually a function by examining its type
      auto const var_value(runtime::deref(var_obj));
      if(var_value != jank_nil)
      {
        auto const val_type(var_value->type);
        info.is_function = (val_type == object_type::jit_function
                            || val_type == object_type::native_function_wrapper
                            || val_type == object_type::multi_function);
      }

      return info;
    }

    std::optional<var_documentation>
    describe_cpp_function(ns_ref target_ns, std::string const &symbol_name) const
    {
      if(symbol_name.empty())
      {
        return std::nullopt;
      }

      var_documentation info;
      info.ns_name = current_ns_name(target_ns);
      info.name = symbol_name;
      info.is_function = true;
      info.is_cpp_function = true;

      auto const locked_globals{ __rt_ctx->global_cpp_functions.rlock() };
      auto const key(make_immutable_string(symbol_name));
      auto const it(locked_globals->find(key));

      auto const populate_from_cpp_functions = [&](auto const &functions) {
        info.arglists.clear();
        info.arglists.reserve(functions.size());
        info.cpp_signatures.reserve(functions.size());

        for(auto const fn : functions)
        {
          var_documentation::cpp_signature signature;
          auto const return_type(Cpp::GetFunctionReturnType(fn));
          if(return_type)
          {
            auto const ret_type_str = Cpp::GetTypeAsString(return_type);
            if(ret_type_str.find("NULL TYPE") == std::string::npos)
            {
              signature.return_type = ret_type_str;
            }
            else
            {
              auto const ret_scope = Cpp::GetScopeFromType(return_type);
              signature.return_type = ret_scope ? Cpp::GetQualifiedName(ret_scope) : "auto";
            }
          }
          else
          {
            signature.return_type = "void";
          }

          auto const num_args(Cpp::GetFunctionNumArgs(fn));
          std::string rendered_signature{ "[" };
          bool first_arg{ true };
          for(size_t idx{}; idx < num_args; ++idx)
          {
            var_documentation::cpp_argument arg_doc;
            auto const arg_type(Cpp::GetFunctionArgType(fn, idx));
            if(arg_type)
            {
              auto const type_str = Cpp::GetTypeAsString(arg_type);
              if(type_str.find("NULL TYPE") == std::string::npos)
              {
                arg_doc.type = type_str;
              }
              else
              {
                auto const type_scope = Cpp::GetScopeFromType(arg_type);
                // Use Clang AST directly for template types
                arg_doc.type = type_scope ? Cpp::GetQualifiedName(type_scope)
                                          : get_function_arg_type_string(fn, idx);
              }
            }
            else
            {
              // Use Clang AST directly for template types
              arg_doc.type = get_function_arg_type_string(fn, idx);
            }
            auto const arg_name(Cpp::GetFunctionArgName(fn, idx));
            if(arg_name.empty())
            {
              arg_doc.name = "arg" + std::to_string(idx);
            }
            else
            {
              arg_doc.name = arg_name;
            }
            signature.arguments.emplace_back(arg_doc);

            if(!first_arg)
            {
              rendered_signature.push_back(' ');
            }
            rendered_signature.push_back('[');
            rendered_signature += arg_doc.type;
            if(!arg_doc.name.empty())
            {
              rendered_signature.push_back(' ');
              rendered_signature += arg_doc.name;
            }
            /* Add default value if parameter has one */
            auto const default_val = get_function_arg_default(fn, idx);
            if(default_val.has_value())
            {
              rendered_signature += " {:default ";
              rendered_signature += default_val.value();
              rendered_signature += "}";
            }
            rendered_signature.push_back(']');
            first_arg = false;
          }
          /* Add variadic indicator if the function takes variable arguments */
          auto const *func_decl(get_function_decl(fn));
          if(func_decl && func_decl->isVariadic())
          {
            if(!first_arg)
            {
              rendered_signature.push_back(' ');
            }
            rendered_signature += "...";
          }
          rendered_signature.push_back(']');
          info.arglists.emplace_back(std::move(rendered_signature));
          info.cpp_signatures.emplace_back(std::move(signature));
        }
      };

      // Look up the actual function declaration for docstrings and location info
      auto const scope(Cpp::GetGlobalScope());
      auto const functions(Cpp::GetFunctionsUsingName(scope, symbol_name));

      // Extract metadata from the first function declaration (docstring, location)
      if(!functions.empty())
      {
        auto const metadata(extract_cpp_decl_metadata(functions.front()));
        if(metadata.doc.has_value())
        {
          info.doc = metadata.doc;
        }
        if(metadata.file.has_value())
        {
          info.file = metadata.file;
        }
        if(metadata.line.has_value())
        {
          info.line = metadata.line;
        }
        if(metadata.column.has_value())
        {
          info.column = metadata.column;
        }
      }

      if(it != locked_globals->end() && !it->second.empty())
      {
        info.arglists.clear();
        info.arglists.reserve(it->second.size());
        info.cpp_signatures.reserve(it->second.size());

        /* Use the jank source location from the first metadata entry if available */
        auto const &first_metadata(it->second.front());
        if(first_metadata.origin.is_some())
        {
          info.file = to_std_string(first_metadata.origin.unwrap());
        }
        if(first_metadata.origin_line.is_some())
        {
          info.line = first_metadata.origin_line.unwrap();
        }
        if(first_metadata.origin_column.is_some())
        {
          info.column = first_metadata.origin_column.unwrap();
        }

        for(auto const &metadata : it->second)
        {
          var_documentation::cpp_signature signature;
          signature.return_type = to_std_string(metadata.return_type);
          signature.arguments.reserve(metadata.arguments.size());

          std::string rendered_signature{ "[" };
          bool first_arg{ true };
          for(auto const &argument : metadata.arguments)
          {
            var_documentation::cpp_argument arg_doc;
            arg_doc.name = to_std_string(argument.name);
            arg_doc.type = to_std_string(argument.type);
            signature.arguments.emplace_back(arg_doc);

            if(!first_arg)
            {
              rendered_signature.push_back(' ');
            }
            rendered_signature.push_back('[');
            rendered_signature += arg_doc.type;
            if(!arg_doc.name.empty())
            {
              rendered_signature.push_back(' ');
              rendered_signature += arg_doc.name;
            }
            rendered_signature.push_back(']');
            first_arg = false;
          }
          rendered_signature.push_back(']');
          info.arglists.emplace_back(std::move(rendered_signature));
          info.cpp_signatures.emplace_back(std::move(signature));
        }
      }
      else if(!functions.empty())
      {
        populate_from_cpp_functions(functions);
      }
      else
      {
        return std::nullopt;
      }

      if(info.cpp_signatures.empty())
      {
        info.arglists.emplace_back("[]");
        info.arglists_str = info.arglists.front();
      }
      else if(!info.arglists.empty())
      {
        info.arglists_str = join_with_newline(info.arglists);
        if(!info.return_type.has_value())
        {
          info.return_type = info.cpp_signatures.front().return_type;
        }
      }

      return info;
    }

    std::optional<var_documentation>
    describe_cpp_type(ns_ref target_ns, std::string symbol_name) const
    {
      if(symbol_name.empty())
      {
        return std::nullopt;
      }

      bool const is_constructor{ symbol_name.back() == '.' };
      if(is_constructor)
      {
        symbol_name.pop_back();
        if(symbol_name.empty())
        {
          return std::nullopt;
        }
      }

      auto const locked_types{ __rt_ctx->global_cpp_types.rlock() };
      auto const key(make_immutable_string(symbol_name));
      auto const it(locked_types->find(key));
      if(it == locked_types->end())
      {
        return std::nullopt;
      }

      var_documentation info;
      info.ns_name = current_ns_name(target_ns);
      info.name = is_constructor ? symbol_name + "." : symbol_name;
      info.is_cpp_type = !is_constructor;
      info.is_cpp_constructor = is_constructor;
      info.return_type = to_std_string(it->second.qualified_cpp_name);

      auto kind_doc(cpp_record_kind_to_string(it->second.kind));
      if(!kind_doc.empty())
      {
        if(is_constructor)
        {
          kind_doc += " constructor";
        }
        else
        {
          kind_doc = "C++ " + kind_doc;
        }
        info.doc = std::move(kind_doc);
      }

      // For both type and constructor lookups, show constructor signatures if available
      // This is what users want to know - how to construct the type
      bool const show_constructors = !it->second.constructors.empty();

      if(show_constructors)
      {
        // Show explicit constructors
        for(auto const &ctor : it->second.constructors)
        {
          std::string signature{ "[" };
          for(size_t idx{}; idx < ctor.arguments.size(); ++idx)
          {
            if(idx != 0)
            {
              signature.push_back(' ');
            }
            // Wrap each argument in its own vector
            signature.push_back('[');
            signature += to_std_string(ctor.arguments[idx].type);
            auto const arg_name(to_std_string(ctor.arguments[idx].name));
            if(!arg_name.empty())
            {
              signature.push_back(' ');
              signature += arg_name;
            }
            signature.push_back(']');
          }
          signature.push_back(']');
          info.arglists.emplace_back(signature);
        }

        if(!info.arglists.empty())
        {
          std::string combined;
          for(size_t idx{}; idx < info.arglists.size(); ++idx)
          {
            if(idx != 0)
            {
              combined += " ";
            }
            combined += info.arglists[idx];
          }
          info.arglists_str = combined;
        }
      }
      else if(!it->second.fields.empty())
      {
        // No explicit constructors, show aggregate initialization with fields
        std::string signature{ "[" };
        for(size_t idx{}; idx < it->second.fields.size(); ++idx)
        {
          if(idx != 0)
          {
            signature.push_back(' ');
          }
          // Wrap each field in its own vector
          signature.push_back('[');
          signature += to_std_string(it->second.fields[idx].type);
          auto const field_name(to_std_string(it->second.fields[idx].name));
          if(!field_name.empty())
          {
            signature.push_back(' ');
            signature += field_name;
          }
          signature.push_back(']');
        }
        signature.push_back(']');
        info.arglists.emplace_back(signature);
        info.arglists_str = signature;
      }

      // Always populate cpp_fields for reference
      info.cpp_fields.reserve(it->second.fields.size());
      for(auto const &field : it->second.fields)
      {
        var_documentation::cpp_field field_doc;
        field_doc.name = to_std_string(field.name);
        field_doc.type = to_std_string(field.type);
        info.cpp_fields.emplace_back(std::move(field_doc));
      }

      return info;
    }

    std::optional<var_documentation>
    describe_cpp_variable(ns_ref target_ns, std::string const &symbol_name) const
    {
      if(symbol_name.empty())
      {
        return std::nullopt;
      }

      auto const locked_vars{ __rt_ctx->global_cpp_variables.rlock() };
      auto const key(make_immutable_string(symbol_name));
      auto const it(locked_vars->find(key));
      if(it == locked_vars->end())
      {
        return std::nullopt;
      }

      var_documentation info;
      info.ns_name = current_ns_name(target_ns);
      info.name = symbol_name;
      info.is_function = false;
      info.is_cpp_function = false;
      info.is_cpp_variable = true;

      /* Set the doc to show the variable type */
      info.doc = "C++ variable of type: " + to_std_string(it->second.type);

      /* Include source location if available */
      if(it->second.origin.is_some())
      {
        info.file = to_std_string(it->second.origin.unwrap());
      }
      if(it->second.origin_line.is_some())
      {
        info.line = it->second.origin_line.unwrap();
      }
      if(it->second.origin_column.is_some())
      {
        info.column = it->second.origin_column.unwrap();
      }

      return info;
    }

    std::optional<var_documentation>
    describe_cpp_entity(ns_ref target_ns, std::string const &symbol_name) const
    {
      if(auto info = describe_cpp_function(target_ns, symbol_name))
      {
        return info;
      }
      if(auto info = describe_cpp_type(target_ns, symbol_name))
      {
        return info;
      }
      if(auto info = describe_cpp_variable(target_ns, symbol_name))
      {
        return info;
      }

      return std::nullopt;
    }

    /* Convert jank-style dot notation to C++ scope notation.
     * e.g., "world.inner" -> "world::inner" */
    std::string dots_to_cpp_scope(std::string const &value) const
    {
      std::string result;
      result.reserve(value.size() * 2);
      for(auto const ch : value)
      {
        if(ch == '.')
        {
          result.append("::");
        }
        else
        {
          result.push_back(ch);
        }
      }
      return result;
    }

    /* Get FunctionDecl from a CppInterOp function pointer.
     * Handles both FunctionDecl and FunctionTemplateDecl (extracts templated decl). */
    clang::FunctionDecl const *get_function_decl(void *fn) const
    {
      if(!fn)
      {
        return nullptr;
      }
      auto const *decl = static_cast<clang::Decl const *>(fn);
      /* Try FunctionDecl first */
      if(auto const *func_decl = llvm::dyn_cast<clang::FunctionDecl>(decl))
      {
        return func_decl;
      }
      /* For function templates, get the templated FunctionDecl */
      if(auto const *tmpl_decl = llvm::dyn_cast<clang::FunctionTemplateDecl>(decl))
      {
        return tmpl_decl->getTemplatedDecl();
      }
      return nullptr;
    }

    /* Get argument type as string using Clang AST directly.
     * This handles template parameter types that CppInterOp fails to stringify. */
    std::string get_function_arg_type_string(void *fn, size_t idx) const
    {
      auto const *func_decl = get_function_decl(fn);
      if(!func_decl || idx >= func_decl->getNumParams())
      {
        return "auto";
      }
      auto const *param = func_decl->getParamDecl(idx);
      if(!param)
      {
        return "auto";
      }
      auto type_str = param->getType().getAsString();
      if(type_str.empty() || type_str.find("NULL TYPE") != std::string::npos)
      {
        return "auto";
      }
      return type_str;
    }

    /* Get return type as string using Clang AST directly.
     * This handles template return types that CppInterOp fails to stringify. */
    std::string get_function_return_type_string(void *fn) const
    {
      auto const *func_decl = get_function_decl(fn);
      if(!func_decl)
      {
        return "auto";
      }
      auto type_str = func_decl->getReturnType().getAsString();
      if(type_str.empty() || type_str.find("NULL TYPE") != std::string::npos)
      {
        return "auto";
      }
      return type_str;
    }

    /* Get default argument value as string for a function parameter.
     * Returns empty optional if the parameter has no default. */
    std::optional<std::string> get_function_arg_default(void *fn, size_t idx) const
    {
      auto const *func_decl = get_function_decl(fn);
      if(!func_decl || idx >= func_decl->getNumParams())
      {
        return std::nullopt;
      }
      auto const *param = func_decl->getParamDecl(idx);
      if(!param || !param->hasDefaultArg())
      {
        return std::nullopt;
      }
      /* Get the source range for the default argument */
      auto const range = param->getDefaultArgRange();
      if(!range.isValid())
      {
        return std::nullopt;
      }
      /* Extract the source text using SourceManager */
      auto &ast_ctx = func_decl->getASTContext();
      auto &src_mgr = ast_ctx.getSourceManager();
      auto const begin_loc = range.getBegin();
      auto const end_loc = range.getEnd();
      auto const begin_offset = src_mgr.getFileOffset(begin_loc);
      auto const end_offset = src_mgr.getFileOffset(end_loc);
      auto const file_id = src_mgr.getFileID(begin_loc);
      bool invalid = false;
      auto const buffer = src_mgr.getBufferData(file_id, &invalid);
      if(invalid || buffer.empty() || begin_offset >= buffer.size() || end_offset >= buffer.size())
      {
        return std::nullopt;
      }
      /* The end location points to the start of the last token, so we need to find the end of that token.
       * For simple literals, we scan forward to find the end. */
      size_t actual_end = end_offset;
      while(actual_end < buffer.size())
      {
        char c = buffer[actual_end];
        /* Stop at whitespace, comma, paren, semicolon */
        if(c == ' ' || c == '\t' || c == '\n' || c == ',' || c == ')' || c == ';' || c == '/')
        {
          break;
        }
        ++actual_end;
      }
      std::string result(buffer.data() + begin_offset, actual_end - begin_offset);
      return result;
    }

    std::optional<var_documentation>
    describe_native_header_function(ns::native_alias const &alias,
                                    std::string const &symbol_name) const
    {
      /* For empty scope (global C functions), use the global scope directly.
       * resolve_scope("") fails because it tries to look up an empty name. */
      jtl::ptr<void> scope_ptr;
      if(alias.scope.empty())
      {
        scope_ptr = Cpp::GetGlobalScope();
      }
      else
      {
        auto const scope_res(analyze::cpp_util::resolve_scope(alias.scope));
        if(scope_res.is_err())
        {
          return std::nullopt;
        }
        scope_ptr = scope_res.expect_ok();
      }

      struct
      {
        jtl::ptr<void> data;
      } scope{ scope_ptr };

      /* Check if symbol_name contains a dot - this indicates a member function.
       * e.g., "world.defer_begin" means we want defer_begin() method of type "world" */
      auto const last_dot = symbol_name.rfind('.');
      void *lookup_scope = scope.data;
      std::string function_name = symbol_name;

      /* Check if the scope itself is a class (class-level scope like "flecs::world").
       * In this case, symbol_name directly refers to a member method. */
      bool const is_class_scope = Cpp::IsClass(scope.data);

      if(last_dot != std::string::npos)
      {
        /* Split into type path and function name.
         * For "world.defer_begin": type_path="world", function_name="defer_begin" */
        auto const type_path = symbol_name.substr(0, last_dot);
        function_name = symbol_name.substr(last_dot + 1);

        /* Build the full C++ qualified name for the type */
        auto const cpp_type_path = dots_to_cpp_scope(type_path);
        auto const base_scope_name = to_std_string(alias.scope);
        std::string full_type_name;
        if(base_scope_name.empty())
        {
          full_type_name = cpp_type_path;
        }
        else
        {
          /* Convert base scope dots to :: as well */
          full_type_name = dots_to_cpp_scope(base_scope_name) + "::" + cpp_type_path;
        }

        auto const type_scope = Cpp::GetScopeFromCompleteName(full_type_name);
        if(!type_scope)
        {
          return std::nullopt;
        }

        lookup_scope = type_scope;
      }
      else if(is_class_scope)
      {
        /* No dot in symbol_name but scope is a class - symbol_name directly
         * refers to a member method (e.g., fw/defer_begin where fw has scope "flecs::world"). */
        lookup_scope = scope.data;
      }

      auto const fns(Cpp::GetFunctionsUsingName(lookup_scope, function_name));
      if(fns.empty())
      {
        return std::nullopt;
      }

      var_documentation info;
      /* Use "cpp" as namespace and include the full C++ path in the name.
       * e.g., cpp/flecs.world.get instead of flecs/world.get */
      info.ns_name = "cpp";
      if(alias.scope.empty())
      {
        info.name = symbol_name;
      }
      else
      {
        info.name = to_std_string(alias.scope) + "." + symbol_name;
      }
      info.is_function = true;
      info.is_cpp_function = true;

      for(auto const fn : fns)
      {
        var_documentation::cpp_signature signature;
        auto const return_type(Cpp::GetFunctionReturnType(fn));
        if(return_type)
        {
          auto const ret_type_str = Cpp::GetTypeAsString(return_type);
          // Check for "NULL TYPE" which indicates CppInterOp couldn't stringify the type
          if(ret_type_str.find("NULL TYPE") == std::string::npos)
          {
            signature.return_type = ret_type_str;
          }
          else
          {
            // Try to get a better type representation using qualified name
            auto const ret_scope = Cpp::GetScopeFromType(return_type);
            if(ret_scope)
            {
              signature.return_type = Cpp::GetQualifiedName(ret_scope);
            }
            else
            {
              // Use Clang AST directly for template types
              signature.return_type = get_function_return_type_string(fn);
            }
          }
        }
        else
        {
          signature.return_type = "void";
        }

        std::string rendered_signature{ "[" };
        bool first_arg{ true };

        /* For non-static member methods, add the implicit 'this' parameter.
         * This shows users what type the method expects to be called on. */
        bool const is_member_method = Cpp::IsMethod(fn) && !Cpp::IsStaticMethod(fn);
        if(is_member_method && lookup_scope)
        {
          var_documentation::cpp_argument this_arg;
          this_arg.type = Cpp::GetQualifiedName(lookup_scope);
          this_arg.name = "this";
          signature.arguments.emplace_back(this_arg);

          rendered_signature.push_back('[');
          rendered_signature += this_arg.type;
          rendered_signature.push_back(' ');
          rendered_signature += this_arg.name;
          rendered_signature.push_back(']');
          first_arg = false;
        }

        auto const num_args(Cpp::GetFunctionNumArgs(fn));
        for(size_t idx{}; idx < num_args; ++idx)
        {
          var_documentation::cpp_argument arg_doc;
          auto const arg_type(Cpp::GetFunctionArgType(fn, idx));
          if(arg_type)
          {
            auto const type_str = Cpp::GetTypeAsString(arg_type);
            // Check for "NULL TYPE" which indicates CppInterOp couldn't stringify the type
            if(type_str.find("NULL TYPE") == std::string::npos)
            {
              arg_doc.type = type_str;
            }
            else
            {
              // Try to get a better type representation using qualified name
              auto const type_scope = Cpp::GetScopeFromType(arg_type);
              if(type_scope)
              {
                arg_doc.type = Cpp::GetQualifiedName(type_scope);
              }
              else
              {
                // Use Clang AST directly for template types
                arg_doc.type = get_function_arg_type_string(fn, idx);
              }
            }
          }
          else
          {
            // Use Clang AST directly for template types
            arg_doc.type = get_function_arg_type_string(fn, idx);
          }
          arg_doc.name = Cpp::GetFunctionArgName(fn, idx);
          if(arg_doc.name.empty())
          {
            arg_doc.name = "arg" + std::to_string(idx);
          }
          signature.arguments.emplace_back(arg_doc);

          if(!first_arg)
          {
            rendered_signature.push_back(' ');
          }
          rendered_signature.push_back('[');
          rendered_signature += arg_doc.type;
          if(!arg_doc.name.empty())
          {
            rendered_signature.push_back(' ');
            rendered_signature += arg_doc.name;
          }
          /* Add default value if parameter has one */
          auto const default_val = get_function_arg_default(fn, idx);
          if(default_val.has_value())
          {
            rendered_signature += " {:default ";
            rendered_signature += default_val.value();
            rendered_signature += "}";
          }
          rendered_signature.push_back(']');
          first_arg = false;
        }
        /* Add variadic indicator if the function takes variable arguments */
        auto const *func_decl(get_function_decl(fn));
        if(func_decl && func_decl->isVariadic())
        {
          if(!first_arg)
          {
            rendered_signature.push_back(' ');
          }
          rendered_signature += "...";
        }
        rendered_signature.push_back(']');
        info.arglists.emplace_back(std::move(rendered_signature));
        info.cpp_signatures.emplace_back(std::move(signature));
      }

      /* Extract metadata (docstring, location) from the first function declaration */
      auto const metadata(extract_cpp_decl_metadata(fns.front()));
      if(metadata.doc.has_value())
      {
        info.doc = metadata.doc;
      }
      if(metadata.file.has_value())
      {
        info.file = metadata.file;
      }
      if(metadata.line.has_value())
      {
        info.line = metadata.line;
      }
      if(metadata.column.has_value())
      {
        info.column = metadata.column;
      }

      if(!info.cpp_signatures.empty())
      {
        info.return_type = info.cpp_signatures.front().return_type;
      }

      if(info.arglists.size() == 1)
      {
        info.arglists_str = info.arglists.front();
      }
      else if(!info.arglists.empty())
      {
        info.arglists_str = join_with_newline(info.arglists);
      }

      return info;
    }

    std::optional<var_documentation>
    describe_native_header_type(ns::native_alias const &alias, std::string const &symbol_name) const
    {
      /* Try to find the type using CppInterOp's scope resolution. The symbol_name
       * is just the base name (e.g., "world"), but we need to look it up within
       * the namespace scope (e.g., flecs::world). For global scope (empty alias.scope),
       * just use the symbol name directly. */
      std::string qualified_name;
      if(alias.scope.empty())
      {
        qualified_name = symbol_name;
      }
      else
      {
        qualified_name = to_std_string(alias.scope) + "::" + symbol_name;
      }
      auto type_scope(Cpp::GetScopeFromCompleteName(qualified_name));
      if(!type_scope)
      {
        return std::nullopt;
      }

      bool is_class = Cpp::IsClass(type_scope);
      bool is_enum = Cpp::IsEnumScope(type_scope);

      /* Handle C-style typedef structs: typedef struct {...} Name;
       * In this case, GetScopeFromCompleteName returns the TypedefDecl, not the RecordDecl.
       * We need to check if it's a typedef and get the underlying type. */
      if(!is_class && !is_enum)
      {
        auto *decl = static_cast<clang::Decl *>(type_scope);
        if(auto *typedef_decl = clang::dyn_cast<clang::TypedefNameDecl>(decl))
        {
          auto underlying_type = typedef_decl->getUnderlyingType();
          if(auto *tag_decl = underlying_type->getAsTagDecl())
          {
            if(clang::isa<clang::RecordDecl>(tag_decl))
            {
              is_class = true;
              type_scope = tag_decl;
            }
            else if(clang::isa<clang::EnumDecl>(tag_decl))
            {
              is_enum = true;
              type_scope = tag_decl;
            }
          }
        }
      }

      /* Verify it's actually a class/struct or enum */
      if(!is_class && !is_enum)
      {
        return std::nullopt;
      }

      var_documentation info;
      /* Use "cpp" as namespace and include the full C++ path in the name.
       * e.g., cpp/flecs.world instead of flecs/world */
      info.ns_name = "cpp";
      if(alias.scope.empty())
      {
        info.name = symbol_name;
      }
      else
      {
        info.name = to_std_string(alias.scope) + "." + symbol_name;
      }
      info.is_cpp_type = true;

      /* Get the fully qualified type name */
      auto const type_name(Cpp::GetQualifiedName(type_scope));
      info.return_type = type_name;

      /* Extract docstring and location from the type declaration */
      auto const metadata(extract_cpp_decl_metadata(type_scope));
      if(metadata.doc.has_value())
      {
        info.doc = metadata.doc;
      }
      if(metadata.file.has_value())
      {
        info.file = metadata.file;
      }
      if(metadata.line.has_value())
      {
        info.line = metadata.line;
      }
      if(metadata.column.has_value())
      {
        info.column = metadata.column;
      }

      /* For class/struct types, try to get constructor info */
      if(Cpp::IsClass(type_scope))
      {
        /* Get constructors using the type's name */
        auto const ctors(Cpp::GetFunctionsUsingName(type_scope, symbol_name));
        if(!ctors.empty())
        {
          for(auto const ctor : ctors)
          {
            if(!Cpp::IsConstructor(ctor))
            {
              continue;
            }

            std::string signature{ "[" };
            auto const num_args(Cpp::GetFunctionNumArgs(ctor));
            for(size_t idx{}; idx < num_args; ++idx)
            {
              if(idx != 0)
              {
                signature.push_back(' ');
              }
              signature.push_back('[');
              auto const arg_type(Cpp::GetFunctionArgType(ctor, idx));
              if(arg_type)
              {
                auto const type_str = Cpp::GetTypeAsString(arg_type);
                if(type_str.find("NULL TYPE") == std::string::npos)
                {
                  signature += type_str;
                }
                else
                {
                  auto const type_scope = Cpp::GetScopeFromType(arg_type);
                  // Use Clang AST directly for template types
                  signature += type_scope ? Cpp::GetQualifiedName(type_scope)
                                          : get_function_arg_type_string(ctor, idx);
                }
              }
              else
              {
                // Use Clang AST directly for template types
                signature += get_function_arg_type_string(ctor, idx);
              }
              auto const arg_name(Cpp::GetFunctionArgName(ctor, idx));
              if(!arg_name.empty())
              {
                signature.push_back(' ');
                signature += arg_name;
              }
              signature.push_back(']');
            }
            signature.push_back(']');
            info.arglists.emplace_back(std::move(signature));
          }
        }

        /* If no constructors found, show as empty brackets for default construction */
        if(info.arglists.empty())
        {
          info.arglists.emplace_back("[]");
        }
      }

      /* Enumerate struct/class fields for the Fields: section.
       * This uses Clang AST directly to handle both C++ classes and C structs.
       * Cpp::GetDatamembers() only works for CXXRecordDecl, not plain RecordDecl. */
      {
        auto *decl = static_cast<clang::Decl *>(type_scope);
        if(auto *record_decl = clang::dyn_cast<clang::RecordDecl>(decl))
        {
          for(auto *field : record_decl->fields())
          {
            if(!field)
            {
              continue;
            }

            var_documentation::cpp_field field_doc;
            field_doc.name = field->getNameAsString();

            /* Get the field type */
            auto const field_type = field->getType();
            field_doc.type = field_type.getAsString();

            info.cpp_fields.emplace_back(std::move(field_doc));
          }
        }
      }

      if(!info.arglists.empty())
      {
        info.arglists_str = join_with_newline(info.arglists);
      }

      return info;
    }

#if !defined(JANK_TARGET_EMSCRIPTEN) || defined(JANK_HAS_CPPINTEROP)
    std::optional<var_documentation>
    describe_native_header_macro(ns::native_alias const &alias,
                                 std::string const &symbol_name) const
    {
      bool const is_object_like = asio::is_native_header_macro(alias, symbol_name);
      bool const is_function_like = asio::is_native_header_function_like_macro(alias, symbol_name);

      if(!is_object_like && !is_function_like)
      {
        return std::nullopt;
      }

      var_documentation info;
      info.ns_name = alias.header;
      info.name = symbol_name;
      info.is_cpp_macro = true;

      /* Get the macro expansion as the docstring */
      auto expansion = get_native_header_macro_expansion(alias, symbol_name);
      if(expansion.has_value())
      {
        info.doc = "#define " + expansion.value();
      }
      else
      {
        info.doc = "#define " + symbol_name;
      }

      return info;
    }
#endif

    std::optional<var_documentation>
    describe_native_header_entity(ns::native_alias const &alias,
                                  std::string const &symbol_name) const
    {
      /* Try function first, then fall back to type, then to macro */
      if(auto fn_info = describe_native_header_function(alias, symbol_name))
      {
        return fn_info;
      }
      if(auto type_info = describe_native_header_type(alias, symbol_name))
      {
        return type_info;
      }
#if !defined(JANK_TARGET_EMSCRIPTEN) || defined(JANK_HAS_CPPINTEROP)
      return describe_native_header_macro(alias, symbol_name);
#else
      return std::nullopt;
#endif
    }

    bencode::value::list serialize_cpp_signatures(var_documentation const &info) const
    {
      bencode::value::list payload;
      payload.reserve(info.cpp_signatures.size());
      for(auto const &signature : info.cpp_signatures)
      {
        bencode::value::dict signature_payload;
        if(!signature.return_type.empty())
        {
          signature_payload.emplace("return-type", signature.return_type);
        }

        bencode::value::list args_payload;
        args_payload.reserve(signature.arguments.size());
        for(size_t idx{}; idx < signature.arguments.size(); ++idx)
        {
          bencode::value::dict arg_entry;
          arg_entry.emplace("index", bencode::value{ static_cast<std::int64_t>(idx) });
          arg_entry.emplace("type", signature.arguments[idx].type);
          if(!signature.arguments[idx].name.empty())
          {
            arg_entry.emplace("name", signature.arguments[idx].name);
          }
          args_payload.emplace_back(std::move(arg_entry));
        }

        signature_payload.emplace("args", bencode::value{ std::move(args_payload) });
        payload.emplace_back(std::move(signature_payload));
      }
      return payload;
    }

    static std::optional<std::vector<std::string>>
    parse_string_list(bencode::value::dict const &dict, std::string const &key)
    {
      auto const it(dict.find(key));
      if(it == dict.end() || !it->second.is_list())
      {
        return std::nullopt;
      }

      std::vector<std::string> items;
      items.reserve(it->second.as_list().size());
      for(auto const &entry : it->second.as_list())
      {
        if(!entry.is_string())
        {
          return std::nullopt;
        }
        items.push_back(entry.as_string());
      }
      return items;
    }
  };
}

#include <jank/nrepl_server/ops/clone.hpp>
#include <jank/nrepl_server/ops/describe.hpp>
#include <jank/nrepl_server/ops/ls_sessions.hpp>
#include <jank/nrepl_server/ops/close.hpp>
#include <jank/nrepl_server/ops/eval.hpp>
#include <jank/nrepl_server/ops/load_file.hpp>
#include <jank/nrepl_server/ops/completions.hpp>
#include <jank/nrepl_server/ops/complete.hpp>
#include <jank/nrepl_server/ops/lookup.hpp>
#include <jank/nrepl_server/ops/info.hpp>
#include <jank/nrepl_server/ops/eldoc.hpp>
#include <jank/nrepl_server/ops/forward_system_output.hpp>
#include <jank/nrepl_server/ops/caught.hpp>
#include <jank/nrepl_server/ops/analyze_last_stacktrace.hpp>
#include <jank/nrepl_server/ops/interrupt.hpp>
#include <jank/nrepl_server/ops/ls_middleware.hpp>
#include <jank/nrepl_server/ops/add_middleware.hpp>
#include <jank/nrepl_server/ops/swap_middleware.hpp>
#include <jank/nrepl_server/ops/stdin.hpp>
#include <jank/nrepl_server/ops/wasm_compile_patch.hpp>
