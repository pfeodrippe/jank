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
    std::optional<var_documentation> info;

    // If we have a native alias, skip the regular var lookup and go straight to native header lookup
    if(!requested_native_alias.has_value())
    {
      // Check if this unqualified symbol is a native refer BEFORE checking for regular vars
      // This ensures native header documentation is prioritized
      if(parts.ns.empty())
      {
        auto const native_refer = target_ns->find_native_refer(symbol);
        if(native_refer.is_some())
        {
          auto const &refer_info = native_refer.unwrap();
          // Get the alias to look up the native header info
          auto const alias_name = to_std_string(refer_info.alias->name);
          auto const native_alias_opt = target_ns->find_native_alias(refer_info.alias);
          if(native_alias_opt.is_some())
          {
            info = describe_native_header_function(alias_name,
                                                   native_alias_opt.unwrap(),
                                                   to_std_string(refer_info.member->name));
          }
        }
      }

      if(!info.has_value())
      {
        auto const var(target_ns->find_var(symbol));
        if(!var.is_nil())
        {
          info = describe_var(target_ns, var, parts.name);
        }
      }

      if(!info.has_value() && target_ns->name->name == "cpp")
      {
        info = describe_cpp_entity(target_ns, parts.name);
      }
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

    bencode::value::list eldoc_entries;
    if(!info->arglists.empty())
    {
      eldoc_entries.reserve(info->arglists.size());
      for(auto const &sig : info->arglists)
      {
        // Parse the signature to extract parameter names
        // Signatures come in formats like:
        // - Clojure: [x] [x y] [x y & z]
        // - Native: [[i32 x] [i32 y]] or [[object_ref s]]

        std::string trimmed = sig;
        // Remove leading/trailing whitespace
        auto const start = trimmed.find_first_not_of(" \t\n\r");
        if(start == std::string::npos)
        {
          continue;
        }
        auto const end = trimmed.find_last_not_of(" \t\n\r");
        trimmed = trimmed.substr(start, end - start + 1);

        // Remove outer brackets
        if(trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']')
        {
          trimmed = trimmed.substr(1, trimmed.size() - 2);
        }

        bencode::value::list params;
        std::string current;
        int bracket_depth = 0;

        for(size_t i = 0; i < trimmed.size(); ++i)
        {
          char ch = trimmed[i];

          if(ch == '[')
          {
            bracket_depth++;
            current.push_back(ch);
          }
          else if(ch == ']')
          {
            bracket_depth--;
            current.push_back(ch);
            // If we're back to depth 0, this completes a typed parameter like [i32 x]
            if(bracket_depth == 0 && !current.empty())
            {
              // For typed parameters, keep the full [type name] format
              auto const param_start = current.find_first_not_of(" \t");
              if(param_start != std::string::npos)
              {
                auto const param_end = current.find_last_not_of(" \t");
                params.emplace_back(current.substr(param_start, param_end - param_start + 1));
              }
              current.clear();
            }
          }
          else if(std::isspace(static_cast<unsigned char>(ch)))
          {
            if(bracket_depth == 0 && !current.empty())
            {
              // Simple untyped parameter
              params.emplace_back(current);
              current.clear();
            }
            else if(bracket_depth > 0)
            {
              current.push_back(ch);
            }
          }
          else
          {
            current.push_back(ch);
          }
        }

        // Don't forget the last parameter
        if(!current.empty())
        {
          auto const param_start = current.find_first_not_of(" \t");
          if(param_start != std::string::npos)
          {
            auto const param_end = current.find_last_not_of(" \t");
            params.emplace_back(current.substr(param_start, param_end - param_start + 1));
          }
        }

        eldoc_entries.emplace_back(bencode::value{ std::move(params) });
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
