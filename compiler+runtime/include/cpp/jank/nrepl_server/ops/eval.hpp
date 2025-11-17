#pragma once

#include <iostream>

#include <jank/error.hpp>

namespace jank::nrepl_server::asio
{
  inline std::vector<bencode::value::dict> engine::handle_eval(message const &msg)
  {
    auto const code(msg.get("code"));
    auto const file_path(msg.get("path"));
    if(code.empty())
    {
      return handle_unsupported(msg, "missing-code");
    }

    auto &session(ensure_session(msg.session()));
    obj::persistent_hash_map_ref bindings;
    if(file_path.empty())
    {
      bindings = obj::persistent_hash_map::create_unique(
        std::make_pair(__rt_ctx->current_ns_var, session.current_ns));
    }
    else
    {
      bindings = obj::persistent_hash_map::create_unique(
        std::make_pair(__rt_ctx->current_ns_var, session.current_ns),
        std::make_pair(__rt_ctx->current_file_var,
                       make_box(make_immutable_string(file_path))));
    }
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
    runtime::scoped_output_redirect const redirect{ [&](std::string const &chunk) {
      captured_out += chunk;
    } };
    runtime::scoped_stderr_redirect const stderr_redirect{};

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
      record_exception(session, err_string, object_type_str(ex_obj->type), std::nullopt);
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
    catch(jank::error_ref const &err)
    {
      update_ns();
      emit_pending_output();
      std::cerr << "err source line: " << err->source.start.line << '\n';
      if(!err->notes.empty())
      {
        std::cerr << "first err note line: " << err->notes.front().source.start.line << '\n';
      }
      emit_pending_output();
      std::string const err_string{ err->message.data(), err->message.size() };
      std::string const err_type{ error::kind_str(err->kind) };
      auto const serialized_error(build_serialized_error(err));
      record_exception(session, err_string, err_type, err->source, serialized_error);
      bencode::value::dict err_msg;
      if(!msg.id().empty())
      {
        err_msg.emplace("id", msg.id());
      }
      err_msg.emplace("session", session.id);
      err_msg.emplace("status", bencode::list_of_strings({ "error" }));
      err_msg.emplace("err", err_string);
      err_msg.emplace("exception-type", err_type);
      auto const file_string(to_std_string(err->source.file));
      if(file_string != jank::read::no_source_path)
      {
        err_msg.emplace("file", file_string);
      }
      if(err->source.start.line != 0)
      {
        err_msg.emplace("line", std::to_string(err->source.start.line));
      }
      if(err->source.start.col != 0)
      {
        err_msg.emplace("column", std::to_string(err->source.start.col));
      }
      err_msg.emplace("jank/error", bencode::value{ encode_error(serialized_error) });
      responses.emplace_back(std::move(err_msg));
      responses.emplace_back(make_done_response(session.id, msg.id(), { "done", "error" }));
    }
    catch(std::exception const &ex)
    {
      update_ns();
      emit_pending_output();
      record_exception(session, std::string{ ex.what() }, typeid(ex).name(), std::nullopt);
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
      record_exception(session, "unknown exception", "unknown", std::nullopt);
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
