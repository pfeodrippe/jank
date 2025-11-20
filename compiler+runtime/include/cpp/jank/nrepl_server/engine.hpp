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
    };

    struct var_documentation
    {
      std::string ns_name;
      std::string name;
      std::optional<std::string> doc;
      std::vector<std::string> arglists;
      std::optional<std::string> arglists_str;
      std::optional<std::string> file;
      std::optional<std::int64_t> line;
      std::optional<std::int64_t> column;
      bool is_macro{ false };
      bool is_function{ false };
    };

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
      return query;
    }

    std::vector<std::string>
    collect_symbol_names(ns_ref target_ns, std::string const &prefix, bool owned_only) const
    {
      std::vector<std::string> matches;
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

      if(target_ns->name->name == "cpp")
      {
        auto const locked_globals{ __rt_ctx->global_cpp_functions.rlock() };
        for(auto const &name : *locked_globals)
        {
          auto const candidate_name(to_std_string(name));
          if(!prefix.empty() && !starts_with(candidate_name, prefix))
          {
            continue;
          }
          matches.push_back(candidate_name);
        }
      }

      std::ranges::sort(matches);
      auto const unique_end(std::ranges::unique(matches));
      matches.erase(unique_end.begin(), unique_end.end());
      return matches;
    }

    std::vector<completion_candidate>
    make_completion_candidates(completion_query const &query) const
    {
      auto names(collect_symbol_names(query.target_ns, query.prefix, query.qualifier.has_value()));
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
    describe_var(ns_ref target_ns, var_ref var, std::string const &display_name) const
    {
      if(var.is_nil())
      {
        return std::nullopt;
      }

      var_documentation info;
      info.ns_name = current_ns_name(target_ns);
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
