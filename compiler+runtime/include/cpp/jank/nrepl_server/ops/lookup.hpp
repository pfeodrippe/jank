#pragma once

namespace jank::nrepl_server::asio
{
  inline std::vector<bencode::value::dict> engine::handle_lookup(message const &msg)
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
}
