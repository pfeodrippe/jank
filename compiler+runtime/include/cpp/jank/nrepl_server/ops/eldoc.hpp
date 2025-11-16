#pragma once

namespace jank::nrepl_server::asio
{
  inline std::vector<bencode::value::dict> engine::handle_eldoc(message const &msg)
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
}
