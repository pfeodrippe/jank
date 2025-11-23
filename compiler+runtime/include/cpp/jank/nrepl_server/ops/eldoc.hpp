#pragma once

#include <cctype>

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
    auto context_ns(expect_object<ns>(session.current_ns));
    std::optional<ns::native_alias> requested_native_alias;
    std::string alias_display;
    if(!parts.ns.empty())
    {
      alias_display = parts.ns;
      requested_native_alias = find_native_alias(context_ns, alias_display);
    }

    auto target_ns(resolve_namespace(session, ns_request));
    auto const symbol(make_box<obj::symbol>(make_immutable_string(parts.name)));
    auto const var(target_ns->find_var(symbol));
    std::optional<var_documentation> info;
    if(!var.is_nil())
    {
      info = describe_var(target_ns, var, parts.name);
    }
    if(!info.has_value() && target_ns->name->name == "cpp")
    {
      info = describe_cpp_entity(target_ns, parts.name);
    }
    if(!info.has_value() && requested_native_alias.has_value())
    {
      info = describe_native_header_function(alias_display,
                                             requested_native_alias.value(),
                                             parts.name);
    }
    if(!info.has_value())
    {
      return { make_done_response(session.id, msg.id(), { "done", "no-eldoc" }) };
    }

    auto trim = [](std::string &text) {
      auto const start(text.find_first_not_of(" \t\n\r"));
      if(start == std::string::npos)
      {
        text.clear();
        return;
      }
      auto const end(text.find_last_not_of(" \t\n\r"));
      text = text.substr(start, end - start + 1);
    };

    auto tokenize_signature = [&](std::string signature) {
      trim(signature);
      if(signature.size() >= 2 && signature.front() == '[' && signature.back() == ']')
      {
        signature = signature.substr(1, signature.size() - 2);
      }

      std::vector<std::string> tokens;
      std::string current;
      for(char const ch : signature)
      {
        if(std::isspace(static_cast<unsigned char>(ch)))
        {
          if(!current.empty())
          {
            tokens.emplace_back(std::move(current));
            current.clear();
          }
        }
        else
        {
          current.push_back(ch);
        }
      }
      if(!current.empty())
      {
        tokens.emplace_back(std::move(current));
      }
      return tokens;
    };

    bencode::value::list eldoc_entries;
    if(!info->arglists.empty())
    {
      eldoc_entries.reserve(info->arglists.size());
      for(auto const &sig : info->arglists)
      {
        auto tokens(tokenize_signature(sig));
        while(!tokens.empty() && tokens.front() == "quote")
        {
          tokens.erase(tokens.begin());
        }
        bencode::value::list token_values;
        token_values.reserve(tokens.size());
        for(auto const &token : tokens)
        {
          token_values.emplace_back(token);
        }
        eldoc_entries.emplace_back(std::move(token_values));
      }
    }

    auto const type = [&]() {
      if(info->is_cpp_constructor)
      {
        return std::string{ "native-constructor" };
      }
      if(info->is_cpp_type)
      {
        return std::string{ "native-type" };
      }
      if(info->is_cpp_function)
      {
        return std::string{ "native-function" };
      }
      if(info->is_macro)
      {
        return std::string{ "macro" };
      }
      // Check if it's actually a function, even if arglists is missing
      if(info->is_function || !info->arglists.empty())
      {
        return std::string{ "function" };
      }
      return std::string{ "variable" };
    }();

    auto const build_docstring
      = [](var_documentation const &metadata) -> std::optional<std::string> {
      if(!metadata.return_type.has_value() && !metadata.doc.has_value())
      {
        return std::nullopt;
      }

      std::string rendered;
      if(metadata.return_type.has_value() && !metadata.return_type->empty())
      {
        rendered += metadata.return_type.value();
      }
      if(metadata.doc.has_value() && !metadata.doc->empty())
      {
        if(!rendered.empty())
        {
          rendered.push_back(' ');
        }
        rendered += metadata.doc.value();
      }
      if(rendered.empty())
      {
        return std::nullopt;
      }
      return rendered;
    };

    auto const docstring(build_docstring(*info));

    bencode::value::dict payload;
    if(!msg.id().empty())
    {
      payload.emplace("id", msg.id());
    }
    payload.emplace("session", session.id);
    payload.emplace("ns", info->ns_name);
    payload.emplace("name", info->name);
    payload.emplace("type", type);
    // Always include the eldoc field, even if empty, to indicate we successfully processed the request
    payload.emplace("eldoc", bencode::value{ std::move(eldoc_entries) });
    if(docstring.has_value())
    {
      payload.emplace("doc", docstring.value());
      payload.emplace("docstring", docstring.value());
    }
    if(info->return_type.has_value())
    {
      payload.emplace("return-type", info->return_type.value());
    }
    if(info->is_cpp_function && !info->cpp_signatures.empty())
    {
      payload.emplace("cpp-signatures", bencode::value{ serialize_cpp_signatures(info.value()) });
    }
    payload.emplace("status", bencode::list_of_strings({ "done" }));
    return { std::move(payload) };
  }
}
