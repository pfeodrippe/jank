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
        session_id = next_session_id();
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

    std::vector<bencode::value::dict> handle_clone(message const &msg)
    {
      auto const parent(msg.session());
      auto const &parent_session(ensure_session(parent));

      auto const new_id(next_session_id());
      auto &child(sessions_[new_id]);
      child.id = new_id;
      child.current_ns = parent_session.current_ns;
      child.forward_system_output = parent_session.forward_system_output;

      bencode::value::dict payload;
      if(!msg.id().empty())
      {
        payload.emplace("id", msg.id());
      }
      payload.emplace("session", new_id);
      payload.emplace("new-session", new_id);
      payload.emplace("status", bencode::list_of_strings({ "done" }));
      return { std::move(payload) };
    }

    std::vector<bencode::value::dict> handle_describe(message const &msg)
    {
      bencode::value::dict payload;
      if(!msg.id().empty())
      {
        payload.emplace("id", msg.id());
      }

      bencode::value::dict versions;
      versions.emplace("jank", version_);
      payload.emplace("versions", bencode::value{ std::move(versions) });

      bencode::value::dict ops;
      ops.emplace("clone", bencode::make_doc_value("Create a new session"));
      ops.emplace("describe", bencode::make_doc_value("Describe server capabilities"));
      ops.emplace("ls-sessions", bencode::make_doc_value("List active sessions"));
      ops.emplace("close", bencode::make_doc_value("Close the provided session"));
      ops.emplace("eval", bencode::make_doc_value("Evaluate code in the given session"));
      ops.emplace("load-file", bencode::make_doc_value("Load and evaluate a file"));
      ops.emplace("completions", bencode::make_doc_value("Return completion candidates"));
      ops.emplace("complete",
                  bencode::make_doc_value("Return metadata-rich completion candidates"));
      ops.emplace("lookup", bencode::make_doc_value("Lookup metadata about a symbol"));
      ops.emplace("info", bencode::make_doc_value("Return CIDER-compatible symbol info"));
      ops.emplace("eldoc", bencode::make_doc_value("Return eldoc hints for a symbol"));
      ops.emplace("forward-system-output",
                  bencode::make_doc_value("Enable forwarding of System/out and System/err"));
      ops.emplace("interrupt", bencode::make_doc_value("Attempt to interrupt a running eval"));
      ops.emplace("ls-middleware", bencode::make_doc_value("List middleware stack"));
      ops.emplace("add-middleware", bencode::make_doc_value("Add middleware"));
      ops.emplace("swap-middleware", bencode::make_doc_value("Swap middleware order"));
      ops.emplace("stdin", bencode::make_doc_value("Provide stdin content"));
      ops.emplace("caught",
                  bencode::make_doc_value("Return details about the last evaluation error"));
      payload.emplace("ops", bencode::value{ std::move(ops) });

      payload.emplace("status", bencode::list_of_strings({ "done" }));
      return { std::move(payload) };
    }

    std::vector<bencode::value::dict> handle_ls_sessions(message const &msg)
    {
      std::vector<std::string> ids;
      ids.reserve(sessions_.size());
      for(auto const &entry : sessions_)
      {
        ids.push_back(entry.first);
      }
      std::sort(ids.begin(), ids.end());

      bencode::value::dict payload;
      if(!msg.id().empty())
      {
        payload.emplace("id", msg.id());
      }
      payload.emplace("sessions", bencode::list_of_strings(ids));
      payload.emplace("status", bencode::list_of_strings({ "done" }));
      return { std::move(payload) };
    }

    std::vector<bencode::value::dict> handle_close(message const &msg)
    {
      auto const session_id(msg.session());
      auto const erased(sessions_.erase(session_id));
      if(erased == 0)
      {
        return handle_unsupported(msg, "unknown-session");
      }

      return { make_done_response(session_id, msg.id(), { "done" }) };
    }

    std::vector<bencode::value::dict> handle_eval(message const &msg)
    {
      auto const code(msg.get("code"));
      if(code.empty())
      {
        return handle_unsupported(msg, "missing-code");
      }

      auto &session(ensure_session(msg.session()));
      auto const bindings(obj::persistent_hash_map::create_unique(
        std::make_pair(__rt_ctx->current_ns_var, session.current_ns)));
      context::binding_scope const scope{ bindings };
      session.running_eval = true;
      session.active_request_id = msg.id();
      auto const reset_state([&session]() {
        session.running_eval = false;
        session.active_request_id.clear();
      });
      util::scope_exit const done{ reset_state };
      clear_last_exception(session);

      std::vector<bencode::value::dict> responses;
      std::string captured_out;
      runtime::scoped_output_redirect const redirect{ [&](std::string chunk) {
        captured_out += std::move(chunk);
      } };

      auto emit_pending_output([&]() {
        if(captured_out.empty())
        {
          return;
        }

        bencode::value::dict out_msg;
        if(!msg.id().empty())
        {
          out_msg.emplace("id", msg.id());
        }
        out_msg.emplace("session", session.id);
        out_msg.emplace("out", captured_out);
        responses.emplace_back(std::move(out_msg));
        captured_out.clear();
      });

      auto update_ns([&session]() { session.current_ns = __rt_ctx->current_ns_var->deref(); });

      try
      {
        jtl::immutable_string_view const code_view{ code.data(), code.size() };
        auto const result(__rt_ctx->eval_string(code_view));
        update_ns();
        emit_pending_output();

        bencode::value::dict value_msg;
        if(!msg.id().empty())
        {
          value_msg.emplace("id", msg.id());
        }
        value_msg.emplace("session", session.id);
        value_msg.emplace("ns", current_ns_name(session.current_ns));
        value_msg.emplace("value", to_std_string(runtime::to_code_string(result)));
        responses.emplace_back(std::move(value_msg));

        responses.emplace_back(make_done_response(session.id, msg.id(), { "done" }));
      }
      catch(runtime::object_ref const &ex_obj)
      {
        update_ns();
        emit_pending_output();
        auto const err_string(to_std_string(runtime::to_code_string(ex_obj)));
        record_exception(session, err_string, object_type_str(ex_obj->type));
        bencode::value::dict err_msg;
        if(!msg.id().empty())
        {
          err_msg.emplace("id", msg.id());
        }
        err_msg.emplace("session", session.id);
        err_msg.emplace("status", bencode::list_of_strings({ "error" }));
        err_msg.emplace("err", err_string);
        responses.emplace_back(std::move(err_msg));
        responses.emplace_back(make_done_response(session.id, msg.id(), { "done", "error" }));
      }
      catch(std::exception const &ex)
      {
        update_ns();
        emit_pending_output();
        record_exception(session, std::string{ ex.what() }, typeid(ex).name());
        bencode::value::dict err_msg;
        if(!msg.id().empty())
        {
          err_msg.emplace("id", msg.id());
        }
        err_msg.emplace("session", session.id);
        err_msg.emplace("status", bencode::list_of_strings({ "error" }));
        err_msg.emplace("err", std::string{ ex.what() });
        responses.emplace_back(std::move(err_msg));
        responses.emplace_back(make_done_response(session.id, msg.id(), { "done", "error" }));
      }
      catch(...)
      {
        update_ns();
        emit_pending_output();
        record_exception(session, "unknown exception", "unknown");
        bencode::value::dict err_msg;
        if(!msg.id().empty())
        {
          err_msg.emplace("id", msg.id());
        }
        err_msg.emplace("session", session.id);
        err_msg.emplace("status", bencode::list_of_strings({ "error" }));
        err_msg.emplace("err", std::string{ "unknown exception" });
        responses.emplace_back(std::move(err_msg));
        responses.emplace_back(make_done_response(session.id, msg.id(), { "done", "error" }));
      }

      return responses;
    }

    std::vector<bencode::value::dict> handle_load_file(message const &msg)
    {
      auto const file_contents(msg.get("file"));
      if(file_contents.empty())
      {
        return handle_unsupported(msg, "missing-file");
      }

      auto eval_dict(msg.data);
      eval_dict.erase("file");
      eval_dict["code"] = file_contents;

      message eval_msg{ std::move(eval_dict) };
      auto responses(handle_eval(eval_msg));
      for(auto &payload : responses)
      {
        payload.erase("ns");
      }
      return responses;
    }

    std::vector<bencode::value::dict> handle_completions(message const &msg)
    {
      auto const prefix(msg.get("prefix"));
      auto &session(ensure_session(msg.session()));
      auto const requested_ns(msg.get("ns"));
      auto const query(prepare_completion_query(session, prefix, requested_ns));
      auto const candidates(make_completion_candidates(query));

      bencode::value::list completions;
      completions.reserve(candidates.size());
      for(auto const &candidate : candidates)
      {
        bencode::value::dict entry;
        entry.emplace("candidate", candidate.display_name);
        entry.emplace("type", std::string{ "var" });
        completions.emplace_back(bencode::value{ std::move(entry) });
      }

      bencode::value::dict payload;
      if(!msg.id().empty())
      {
        payload.emplace("id", msg.id());
      }
      payload.emplace("session", session.id);
      payload.emplace("completions", bencode::value{ std::move(completions) });
      payload.emplace("status", bencode::list_of_strings({ "done" }));
      return { std::move(payload) };
    }

    std::vector<bencode::value::dict> handle_complete(message const &msg)
    {
      auto prefix(msg.get("prefix"));
      auto const symbol_input(msg.get("symbol"));
      if(prefix.empty())
      {
        prefix = symbol_input;
      }
      if(prefix.empty())
      {
        return handle_unsupported(msg, "missing-prefix");
      }

      auto &session(ensure_session(msg.session()));
      auto const query(prepare_completion_query(session, prefix, msg.get("ns")));
      auto const candidates(make_completion_candidates(query));

      bool include_doc{ true };
      bool include_arglists{ true };
      bool include_ns_info{ true };
      if(auto const extra = parse_string_list(msg.data, "extra-metadata"))
      {
        std::unordered_set<std::string> normalized;
        normalized.reserve(extra->size());
        for(auto entry : extra.value())
        {
          auto const normalized_entry(normalize_metadata_key(entry));
          normalized.insert(normalized_entry);
        }
        include_doc = normalized.contains("doc");
        include_arglists = normalized.contains("arglists");
        include_ns_info = normalized.contains("ns");
      }

      bencode::value::list completion_payloads;
      completion_payloads.reserve(candidates.size());
      for(auto const &candidate : candidates)
      {
        auto const symbol(make_box<obj::symbol>(make_immutable_string(candidate.symbol_name)));
        auto const var(query.target_ns->find_var(symbol));
        if(var.is_nil())
        {
          continue;
        }

        auto const var_info(describe_var(query.target_ns, var, candidate.symbol_name));
        if(!var_info.has_value())
        {
          continue;
        }

        bencode::value::dict entry;
        entry.emplace("candidate", candidate.display_name);
        entry.emplace("type", var_info->is_macro ? std::string{ "macro" } : std::string{ "var" });
        if(include_ns_info)
        {
          entry.emplace("ns", var_info->ns_name);
        }
        if(include_doc && var_info->doc.has_value())
        {
          entry.emplace("doc", var_info->doc.value());
        }
        if(include_arglists && !var_info->arglists.empty())
        {
          entry.emplace("arglists", bencode::list_of_strings(var_info->arglists));
          if(var_info->arglists_str.has_value())
          {
            entry.emplace("arglists-str", var_info->arglists_str.value());
          }
        }
        completion_payloads.emplace_back(bencode::value{ std::move(entry) });
      }

      bencode::value::dict payload;
      if(!msg.id().empty())
      {
        payload.emplace("id", msg.id());
      }
      payload.emplace("session", session.id);
      payload.emplace("completions", bencode::value{ std::move(completion_payloads) });
      payload.emplace("status", bencode::list_of_strings({ "done" }));
      return { std::move(payload) };
    }

    std::vector<bencode::value::dict> handle_lookup(message const &msg)
    {
      auto const sym_input(msg.get("sym"));
      if(sym_input.empty())
      {
        return handle_unsupported(msg, "missing-symbol");
      }

      auto parts(parse_symbol(sym_input));
      if(parts.name.empty())
      {
        return handle_unsupported(msg, "missing-symbol");
      }

      auto ns_request(msg.get("ns"));
      if(!parts.ns.empty())
      {
        ns_request = parts.ns;
      }

      auto &session(ensure_session(msg.session()));
      auto target_ns(resolve_namespace(session, ns_request));
      auto const symbol_name(make_immutable_string(parts.name));
      auto const symbol(make_box<obj::symbol>(symbol_name));
      auto const var(target_ns->find_var(symbol));

      bencode::value::dict info;
      info.emplace("name", parts.name);
      info.emplace("ns", current_ns_name(target_ns));
      if(var.is_some())
      {
        info.emplace("var", to_std_string(var->to_code_string()));
      }
      else
      {
        info.emplace("missing", std::string{ "true" });
      }

      bencode::value::dict payload;
      if(!msg.id().empty())
      {
        payload.emplace("id", msg.id());
      }
      payload.emplace("session", session.id);
      payload.emplace("info", bencode::value{ std::move(info) });
      payload.emplace("status", bencode::list_of_strings({ "done" }));
      return { std::move(payload) };
    }

    std::vector<bencode::value::dict> handle_info(message const &msg)
    {
      auto sym_input(msg.get("sym"));
      if(sym_input.empty())
      {
        sym_input = msg.get("symbol");
      }
      if(sym_input.empty())
      {
        return handle_unsupported(msg, "missing-symbol");
      }

      auto const parts(parse_symbol(sym_input));
      if(parts.name.empty())
      {
        return handle_unsupported(msg, "missing-symbol");
      }

      auto ns_request(msg.get("ns"));
      if(!parts.ns.empty())
      {
        ns_request = parts.ns;
      }

      auto &session(ensure_session(msg.session()));
      auto target_ns(resolve_namespace(session, ns_request));
      auto const symbol(make_box<obj::symbol>(make_immutable_string(parts.name)));
      auto const var(target_ns->find_var(symbol));
      if(var.is_nil())
      {
        return { make_done_response(session.id, msg.id(), { "done", "no-info" }) };
      }

      auto const info(describe_var(target_ns, var, parts.name));
      if(!info.has_value())
      {
        return { make_done_response(session.id, msg.id(), { "done", "no-info" }) };
      }

      bencode::value::dict info_dict;
      info_dict.emplace("name", info->name);
      info_dict.emplace("ns", info->ns_name);
      info_dict.emplace("type", info->is_macro ? std::string{ "macro" } : std::string{ "var" });
      if(info->doc.has_value())
      {
        info_dict.emplace("doc", info->doc.value());
      }
      if(!info->arglists.empty())
      {
        info_dict.emplace("arglists", bencode::list_of_strings(info->arglists));
      }
      if(info->arglists_str.has_value())
      {
        info_dict.emplace("arglists-str", info->arglists_str.value());
      }
      if(info->file.has_value())
      {
        info_dict.emplace("file", info->file.value());
      }
      if(info->line.has_value())
      {
        info_dict.emplace("line", bencode::value{ info->line.value() });
      }
      if(info->column.has_value())
      {
        info_dict.emplace("column", bencode::value{ info->column.value() });
      }

      bencode::value::dict payload;
      if(!msg.id().empty())
      {
        payload.emplace("id", msg.id());
      }
      payload.emplace("session", session.id);
      payload.emplace("info", bencode::value{ std::move(info_dict) });
      payload.emplace("status", bencode::list_of_strings({ "done" }));
      return { std::move(payload) };
    }

    std::vector<bencode::value::dict> handle_eldoc(message const &msg)
    {
      auto sym_input(msg.get("sym"));
      if(sym_input.empty())
      {
        sym_input = msg.get("symbol");
      }
      if(sym_input.empty())
      {
        return handle_unsupported(msg, "missing-symbol");
      }

      auto const parts(parse_symbol(sym_input));
      if(parts.name.empty())
      {
        return handle_unsupported(msg, "missing-symbol");
      }

      auto ns_request(msg.get("ns"));
      if(!parts.ns.empty())
      {
        ns_request = parts.ns;
      }

      auto &session(ensure_session(msg.session()));
      auto target_ns(resolve_namespace(session, ns_request));
      auto const symbol(make_box<obj::symbol>(make_immutable_string(parts.name)));
      auto const var(target_ns->find_var(symbol));
      if(var.is_nil())
      {
        return { make_done_response(session.id, msg.id(), { "done", "no-eldoc" }) };
      }

      auto const info(describe_var(target_ns, var, parts.name));
      if(!info.has_value())
      {
        return { make_done_response(session.id, msg.id(), { "done", "no-eldoc" }) };
      }

      bencode::value::list eldoc_entries;
      if(!info->arglists.empty())
      {
        eldoc_entries.reserve(info->arglists.size());
        for(auto const &sig : info->arglists)
        {
          eldoc_entries.emplace_back(info->name + " " + sig);
        }
      }
      else
      {
        eldoc_entries.emplace_back(info->name);
      }

      bencode::value::dict payload;
      if(!msg.id().empty())
      {
        payload.emplace("id", msg.id());
      }
      payload.emplace("session", session.id);
      payload.emplace("ns", info->ns_name);
      payload.emplace("eldoc", bencode::value{ std::move(eldoc_entries) });
      if(info->doc.has_value())
      {
        payload.emplace("doc", info->doc.value());
      }
      payload.emplace("status", bencode::list_of_strings({ "done" }));
      return { std::move(payload) };
    }

    std::vector<bencode::value::dict> handle_forward_system_output(message const &msg)
    {
      auto &session(ensure_session(msg.session()));
      session.forward_system_output = true;

      bencode::value::dict payload;
      if(!msg.id().empty())
      {
        payload.emplace("id", msg.id());
      }
      payload.emplace("session", session.id);
      payload.emplace("status", bencode::list_of_strings({ "done" }));
      return { std::move(payload) };
    }

    std::vector<bencode::value::dict> handle_caught(message const &msg)
    {
      auto &session(ensure_session(msg.session()));
      bencode::value::dict payload;
      if(!msg.id().empty())
      {
        payload.emplace("id", msg.id());
      }
      payload.emplace("session", session.id);

      if(session.last_exception_message.has_value())
      {
        payload.emplace("err", session.last_exception_message.value());
        if(session.last_exception_type.has_value())
        {
          payload.emplace("exception-type", session.last_exception_type.value());
        }
        payload.emplace("status", bencode::list_of_strings({ "done" }));
      }
      else
      {
        payload.emplace("status", bencode::list_of_strings({ "done", "no-error" }));
      }

      return { std::move(payload) };
    }

    std::vector<bencode::value::dict> handle_interrupt(message const &msg)
    {
      auto const target_id(msg.get("interrupt-id"));
      if(target_id.empty())
      {
        return handle_unsupported(msg, "missing-interrupt-id");
      }

      auto &session(ensure_session(msg.session()));
      bencode::value::dict payload;
      if(!msg.id().empty())
      {
        payload.emplace("id", msg.id());
      }
      payload.emplace("session", session.id);
      payload.emplace("interrupt-id", target_id);

      if(session.active_request_id == target_id)
      {
        payload.emplace("status", bencode::list_of_strings({ "interrupt-unsent", "done" }));
      }
      else
      {
        payload.emplace("status", bencode::list_of_strings({ "session-idle", "done" }));
      }
      return { std::move(payload) };
    }

    std::vector<bencode::value::dict> handle_ls_middleware(message const &msg)
    {
      auto &session(ensure_session(msg.session()));
      bencode::value::dict payload;
      if(!msg.id().empty())
      {
        payload.emplace("id", msg.id());
      }
      payload.emplace("session", session.id);
      payload.emplace("middleware", bencode::list_of_strings(middleware_stack_));
      payload.emplace("status", bencode::list_of_strings({ "done" }));
      return { std::move(payload) };
    }

    std::vector<bencode::value::dict> handle_add_middleware(message const &msg)
    {
      auto maybe_items(parse_string_list(msg.data, "middleware"));
      if(!maybe_items.has_value())
      {
        return handle_unsupported(msg, "missing-middleware");
      }

      for(auto const &entry : maybe_items.value())
      {
        auto const already(std::find(middleware_stack_.begin(), middleware_stack_.end(), entry));
        if(already == middleware_stack_.end())
        {
          middleware_stack_.push_back(entry);
        }
      }

      auto &session(ensure_session(msg.session()));
      bencode::value::dict payload;
      if(!msg.id().empty())
      {
        payload.emplace("id", msg.id());
      }
      payload.emplace("session", session.id);
      payload.emplace("middleware", bencode::list_of_strings(middleware_stack_));
      payload.emplace("status", bencode::list_of_strings({ "done" }));
      return { std::move(payload) };
    }

    std::vector<bencode::value::dict> handle_swap_middleware(message const &msg)
    {
      auto maybe_items(parse_string_list(msg.data, "middleware"));
      if(!maybe_items.has_value())
      {
        return handle_unsupported(msg, "missing-middleware");
      }

      std::set<std::string> existing{ middleware_stack_.begin(), middleware_stack_.end() };
      std::set<std::string> incoming{ maybe_items->begin(), maybe_items->end() };
      if(existing != incoming)
      {
        return handle_unsupported(msg, "middleware-mismatch");
      }

      middleware_stack_ = maybe_items.value();
      auto &session(ensure_session(msg.session()));
      bencode::value::dict payload;
      if(!msg.id().empty())
      {
        payload.emplace("id", msg.id());
      }
      payload.emplace("session", session.id);
      payload.emplace("middleware", bencode::list_of_strings(middleware_stack_));
      payload.emplace("status", bencode::list_of_strings({ "done" }));
      return { std::move(payload) };
    }

    std::vector<bencode::value::dict> handle_stdin(message const &msg)
    {
      auto chunk(msg.get("stdin"));
      if(chunk.empty())
      {
        return handle_unsupported(msg, "missing-stdin");
      }

      auto &session(ensure_session(msg.session()));
      session.stdin_buffer += chunk;

      bencode::value::dict payload;
      if(!msg.id().empty())
      {
        payload.emplace("id", msg.id());
      }
      payload.emplace("session", session.id);
      payload.emplace("stdin", chunk);
      payload.emplace("unread", session.stdin_buffer);
      payload.emplace("status", bencode::list_of_strings({ "done" }));
      return { std::move(payload) };
    }

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
          info.arglists = render_sequence_strings(arglists);
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
