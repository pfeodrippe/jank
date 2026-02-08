#pragma once

#include <jank/codegen/wasm_patch_processor.hpp>
#include <jank/analyze/visit.hpp>

namespace jank::nrepl_server::asio
{
  /* Counter for generating unique patch IDs per session. */
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
  inline std::atomic<size_t> global_patch_counter{ 0 };

  inline std::vector<bencode::value::dict> engine::handle_wasm_compile_patch(message const &msg)
  {
    auto const code(msg.get("code"));
    if(code.empty())
    {
      return handle_unsupported(msg, "missing-code");
    }

    auto &session(ensure_session(msg.session()));
    std::vector<bencode::value::dict> responses;

    try
    {
      /* Parse the patch_id if provided, otherwise generate one. */
      size_t patch_id{};
      auto const patch_id_str(msg.get("patch-id"));
      if(patch_id_str.empty())
      {
        patch_id = global_patch_counter.fetch_add(1);
      }
      else
      {
        patch_id = static_cast<size_t>(std::stoul(patch_id_str));
      }

      /* Parse and analyze the code. */
      jtl::immutable_string_view const code_view{ code.data(), code.size() };
      auto const exprs(__rt_ctx->analyze_string(code_view, false));

      if(exprs.empty())
      {
        bencode::value::dict err_msg;
        if(!msg.id().empty())
        {
          err_msg.emplace("id", msg.id());
        }
        err_msg.emplace("session", session.id);
        err_msg.emplace("status", bencode::list_of_strings({ "error", "done" }));
        err_msg.emplace("err", "No expressions found in code");
        return { std::move(err_msg) };
      }

      /* Find the def expression. We expect a single def for patches. */
      jtl::option<analyze::expr::def_ref> def_expr_opt;
      jtl::immutable_string var_name;

      for(auto const &expr : exprs)
      {
        if(expr->kind == analyze::expression_kind::def)
        {
          auto const typed_expr(jtl::static_ref_cast<analyze::expr::def>(expr));
          def_expr_opt = typed_expr;
          var_name = typed_expr->name->get_name();
          break;
        }
      }

      /* Get namespace: prefer explicit "ns" parameter, then def's symbol, then session's current ns. */
      auto const ns_param(msg.get("ns"));
      jtl::immutable_string ns_name;
      if(!ns_param.empty())
      {
        ns_name = jtl::immutable_string{ ns_param };
      }
      else if(def_expr_opt.is_some())
      {
        auto const &sym_ns(def_expr_opt.unwrap()->name->get_namespace());
        if(!sym_ns.empty())
        {
          ns_name = sym_ns;
        }
        else
        {
          auto const current_ns_obj(session.current_ns);
          auto const ns(expect_object<ns>(current_ns_obj));
          ns_name = ns->name->to_string();
        }
      }
      else
      {
        auto const current_ns_obj(session.current_ns);
        auto const ns(expect_object<ns>(current_ns_obj));
        ns_name = ns->name->to_string();
      }

      if(!def_expr_opt.is_some())
      {
        bencode::value::dict err_msg;
        if(!msg.id().empty())
        {
          err_msg.emplace("id", msg.id());
        }
        err_msg.emplace("session", session.id);
        err_msg.emplace("status", bencode::list_of_strings({ "error", "done" }));
        err_msg.emplace("err", "No def expression found in code. Patches must be (def ...) forms.");
        return { std::move(err_msg) };
      }

      /* Generate C++ code using wasm_patch_processor. */
      codegen::wasm_patch_processor processor{ def_expr_opt.unwrap(), ns_name, patch_id };
      auto const cpp_code(processor.generate());

      /* Build successful response. */
      bencode::value::dict result_msg;
      if(!msg.id().empty())
      {
        result_msg.emplace("id", msg.id());
      }
      result_msg.emplace("session", session.id);
      result_msg.emplace("cpp-code", to_std_string(cpp_code));
      result_msg.emplace("var-name", to_std_string(var_name));
      result_msg.emplace("ns-name", to_std_string(ns_name));
      result_msg.emplace("patch-id", std::to_string(patch_id));
      responses.emplace_back(std::move(result_msg));

      responses.emplace_back(make_done_response(session.id, msg.id(), { "done" }));
    }
    catch(jank::error_ref const &err)
    {
      bencode::value::dict err_msg;
      if(!msg.id().empty())
      {
        err_msg.emplace("id", msg.id());
      }
      err_msg.emplace("session", session.id);
      err_msg.emplace("status", bencode::list_of_strings({ "error" }));
      err_msg.emplace("err", to_std_string(err->message));
      responses.emplace_back(std::move(err_msg));
      responses.emplace_back(make_done_response(session.id, msg.id(), { "done", "error" }));
    }
    catch(std::exception const &ex)
    {
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

    return responses;
  }
}
