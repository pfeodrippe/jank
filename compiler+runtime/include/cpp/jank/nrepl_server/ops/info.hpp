#pragma once

namespace jank::nrepl_server::asio
{
  inline std::vector<bencode::value::dict> engine::handle_info(message const &msg)
  {
    auto sym_input(msg.get("sym"));
    if(sym_input.empty())
    {
      sym_input = msg.get("symbol");
    }
    sym_input = strip_text_properties(sym_input);
    if(sym_input.empty())
    {
      return handle_unsupported(msg, "missing-symbol");
    }

    auto const parts(parse_symbol(sym_input));
    if(parts.name.empty())
    {
      return handle_unsupported(msg, "missing-symbol");
    }

    auto ns_request(strip_text_properties(msg.get("ns")));
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
}
