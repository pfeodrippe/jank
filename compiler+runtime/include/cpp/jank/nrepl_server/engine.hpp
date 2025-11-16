#pragma once

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <jtl/immutable_string.hpp>
#include <jtl/immutable_string_view.hpp>

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
    if(slash == std::string::npos || slash == 0 || slash == raw.size() - 1)
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

      list const &as_list() const
      {
        return std::get<list>(data);
      }

      dict const &as_dict() const
      {
        return std::get<dict>(data);
      }
    };

    enum class parse_state
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
      if(static_cast<std::size_t>(size) > remaining)
      {
        return parse_state::need_more;
      }

      auto const start(colon + 1);
      out = value{ std::string{ input.substr(start, static_cast<std::size_t>(size)) } };
      offset = start + static_cast<std::size_t>(size);
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
      if(!requested_ns.empty())
      {
        auto const symbol(make_box<obj::symbol>(make_immutable_string(requested_ns)));
        if(auto const found = __rt_ctx->find_ns(symbol); found.is_some())
        {
          target_ns = found;
        }
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

    void clear_last_exception(session_state &session)
    {
      session.last_exception_message.reset();
      session.last_exception_type.reset();
    }

    void record_exception(session_state &session, std::string message, std::string type)
    {
      session.last_exception_message = std::move(message);
      session.last_exception_type = std::move(type);
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
    };

    bencode::value::dict make_done_response(std::string const &session,
                                            std::string const &id,
                                            std::vector<std::string> statuses) const
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

    completion_query prepare_completion_query(session_state &session,
                                              std::string prefix,
                                              std::string requested_ns) const
    {
      completion_query query;
      auto const parts(parse_symbol(prefix));
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

    std::vector<std::string> collect_symbol_names(ns_ref target_ns, std::string const &prefix) const
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

          auto const sym(expect_object<obj::symbol>(key));
          auto const candidate_name(to_std_string(sym->name));
          if(!prefix.empty() && !starts_with(candidate_name, prefix))
          {
            continue;
          }
          matches.push_back(candidate_name);
        }
      }

      std::sort(matches.begin(), matches.end());
      matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
      return matches;
    }

    std::vector<completion_candidate>
    make_completion_candidates(completion_query const &query) const
    {
      auto names(collect_symbol_names(query.target_ns, query.prefix));
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
      std::transform(token.begin(), token.end(), token.begin(), [](unsigned char c) {
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

      object_ref var_obj{ var };
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
        if(auto const macro_flag(runtime::get(meta, macro_kw)); macro_flag != jank_nil)
        {
          info.is_macro = runtime::truthy(macro_flag);
        }
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
#include <jank/nrepl_server/ops/interrupt.hpp>
#include <jank/nrepl_server/ops/ls_middleware.hpp>
#include <jank/nrepl_server/ops/add_middleware.hpp>
#include <jank/nrepl_server/ops/swap_middleware.hpp>
#include <jank/nrepl_server/ops/stdin.hpp>
