#pragma once

namespace jank::nrepl_server::asio
{
  inline std::vector<bencode::value::dict> engine::handle_eval(message const &msg)
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
}
