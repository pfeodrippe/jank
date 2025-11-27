#pragma once

#include <iostream>
#include <fstream>
#include <csignal>
#include <unistd.h>
#include <cstring>
#include <setjmp.h>
#include <execinfo.h>
#include <cstdlib>

#include <jank/error.hpp>

// Debug file logger that persists even through crashes
namespace jank::nrepl_server::asio
{
  inline void debug_to_file(char const* msg)
  {
    int fd = open("/tmp/jank_nrepl_debug.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if(fd >= 0)
    {
      write(fd, msg, strlen(msg));
      fsync(fd);  // Force flush to disk
      close(fd);
    }
  }
}

namespace jank::nrepl_server::asio
{
  // Global state for signal handling - thread-local to handle multiple connections
  inline thread_local jmp_buf eval_jmp_buf;
  inline thread_local volatile sig_atomic_t signal_received = 0;
  inline thread_local int saved_debug_fd = -1;

  inline void eval_signal_handler(int sig)
  {
    signal_received = sig;

    // Write signal info using raw syscall to saved fd
    if(saved_debug_fd >= 0)
    {
      char const *sig_name = "UNKNOWN";
      switch(sig)
      {
        case SIGSEGV: sig_name = "SIGSEGV"; break;
        case SIGABRT: sig_name = "SIGABRT"; break;
        case SIGBUS: sig_name = "SIGBUS"; break;
        case SIGFPE: sig_name = "SIGFPE"; break;
        case SIGILL: sig_name = "SIGILL"; break;
      }

      char buf[256];
      snprintf(buf, sizeof(buf), "\n[nREPL SIGNAL HANDLER] Caught signal %d (%s)!\n", sig, sig_name);
      write(saved_debug_fd, buf, strlen(buf));

      // Print backtrace
      void *bt_buffer[64];
      int bt_size = backtrace(bt_buffer, 64);
      write(saved_debug_fd, "[nREPL SIGNAL HANDLER] Backtrace:\n", 34);
      backtrace_symbols_fd(bt_buffer, bt_size, saved_debug_fd);
    }

    // Jump back to eval handler - the longjmp will abort the current eval
    longjmp(eval_jmp_buf, sig);
  }

  inline std::vector<bencode::value::dict> engine::handle_eval(message const &msg)
  {
    std::cerr << "[nREPL DEBUG eval.hpp:11] Entered handle_eval\n";
    auto const code(msg.get("code"));
    std::cerr << "[nREPL DEBUG eval.hpp:13] Got code, length=" << code.size() << "\n";
    auto file_path(msg.get("path"));
    if(file_path.empty())
    {
      file_path = msg.get("file");
    }
    auto const to_positive_size
      = [](std::optional<std::int64_t> raw) -> std::optional<std::size_t> {
      if(!raw.has_value() || raw.value() <= 0)
      {
        return std::nullopt;
      }
      return static_cast<std::size_t>(raw.value());
    };
    auto const line_hint(to_positive_size(msg.get_integer("line")));
    auto const column_hint(to_positive_size(msg.get_integer("column")));
    if(code.empty())
    {
      return handle_unsupported(msg, "missing-code");
    }

    std::cerr << "[nREPL DEBUG eval.hpp:33] About to ensure_session\n";
    auto &session(ensure_session(msg.session()));
    std::cerr << "[nREPL DEBUG eval.hpp:35] Got session, id=" << session.id << "\n";
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
        std::make_pair(__rt_ctx->current_file_var, make_box(make_immutable_string(file_path))));
    }
    std::cerr << "[nREPL DEBUG eval.hpp:48] Created bindings\n";
    context::binding_scope const scope{ bindings };
    std::cerr << "[nREPL DEBUG eval.hpp:50] Created binding scope\n";
    session.running_eval = true;
    session.active_request_id = msg.id();
    auto const reset_state([&session]() {
      session.running_eval = false;
      session.active_request_id.clear();
    });
    util::scope_exit const done{ reset_state };
    std::cerr << "[nREPL DEBUG eval.hpp:58] About to clear_last_exception\n";
    clear_last_exception(session);
    std::cerr << "[nREPL DEBUG eval.hpp:60] Cleared last exception\n";

    std::vector<bencode::value::dict> responses;
    std::string captured_out;
    std::cerr << "[nREPL DEBUG eval.hpp:64] About to create scoped_output_redirect\n";

    // Save original stderr fd BEFORE any redirection for debug output
    int const saved_stderr_fd = dup(STDERR_FILENO);
    auto debug_write = [saved_stderr_fd](char const* msg) {
      if(saved_stderr_fd >= 0)
      {
        write(saved_stderr_fd, msg, strlen(msg));
      }
    };

    debug_write("[nREPL DEBUG] saved_stderr_fd created\n");

    runtime::scoped_output_redirect const redirect{ [&](std::string const &chunk) {
      captured_out += chunk;
    } };
    debug_write("[nREPL DEBUG] scoped_output_redirect created\n");
    runtime::scoped_stderr_redirect const stderr_redirect{};
    debug_write("[nREPL DEBUG] scoped_stderr_redirect created\n");
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

    debug_write("[nREPL DEBUG] Defined emit_pending_output lambda\n");
    auto update_ns([&session]() { session.current_ns = __rt_ctx->current_ns_var->deref(); });
    debug_write("[nREPL DEBUG] Defined update_ns lambda\n");

    debug_write("[nREPL DEBUG] About to enter try block\n");

    // Set up signal handlers to catch crashes during eval
    saved_debug_fd = saved_stderr_fd;
    signal_received = 0;

    struct sigaction sa_new, sa_old_segv, sa_old_abrt, sa_old_bus;
    memset(&sa_new, 0, sizeof(sa_new));
    sa_new.sa_handler = eval_signal_handler;
    sigemptyset(&sa_new.sa_mask);
    sa_new.sa_flags = 0;

    sigaction(SIGSEGV, &sa_new, &sa_old_segv);
    sigaction(SIGABRT, &sa_new, &sa_old_abrt);
    sigaction(SIGBUS, &sa_new, &sa_old_bus);

    debug_write("[nREPL DEBUG] Signal handlers installed\n");

    // Set up longjmp point
    int jmp_result = setjmp(eval_jmp_buf);
    if(jmp_result != 0)
    {
      // We got here via longjmp from signal handler
      debug_write("[nREPL DEBUG] Returned from signal via longjmp!\n");

      // Restore signal handlers
      sigaction(SIGSEGV, &sa_old_segv, nullptr);
      sigaction(SIGABRT, &sa_old_abrt, nullptr);
      sigaction(SIGBUS, &sa_old_bus, nullptr);

      // Create error response
      char sig_buf[128];
      snprintf(sig_buf, sizeof(sig_buf), "Caught signal %d during eval", jmp_result);

      update_ns();
      emit_pending_output();
      std::string const ex_type{ "signal" };
      record_exception(session, std::string{ sig_buf }, ex_type, std::nullopt);
      responses.emplace_back(make_eval_error_response(session.id, msg.id(), ex_type, ex_type));
      bencode::value::dict err_msg;
      if(!msg.id().empty())
      {
        err_msg.emplace("id", msg.id());
      }
      err_msg.emplace("session", session.id);
      err_msg.emplace("status", bencode::list_of_strings({ "error" }));
      err_msg.emplace("err", std::string{ sig_buf });
      responses.emplace_back(std::move(err_msg));
      responses.emplace_back(make_done_response(session.id, msg.id(), { "done", "error" }));

      // Close saved fd
      if(saved_stderr_fd >= 0)
      {
        close(saved_stderr_fd);
      }
      return responses;
    }

    try
    {
      debug_write("[nREPL DEBUG] Inside try block\n");
      std::string debug_msg = "[nREPL DEBUG] About to eval: " + code.substr(0, 60) + "...\n";
      debug_write(debug_msg.c_str());
      jtl::immutable_string_view const code_view{ code.data(), code.size() };
      auto const result(__rt_ctx->eval_string(code_view));
      debug_write("[nREPL DEBUG] eval_string completed successfully\n");

      // Restore signal handlers after successful eval
      sigaction(SIGSEGV, &sa_old_segv, nullptr);
      sigaction(SIGABRT, &sa_old_abrt, nullptr);
      sigaction(SIGBUS, &sa_old_bus, nullptr);
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
      debug_write("[nREPL DEBUG] CAUGHT runtime::object_ref exception\n");
      sigaction(SIGSEGV, &sa_old_segv, nullptr);
      sigaction(SIGABRT, &sa_old_abrt, nullptr);
      sigaction(SIGBUS, &sa_old_bus, nullptr);
      update_ns();
      emit_pending_output();
      auto const err_string(to_std_string(runtime::to_code_string(ex_obj)));
      auto const ex_type(object_type_str(ex_obj->type));
      record_exception(session, err_string, ex_type, std::nullopt);
      responses.emplace_back(make_eval_error_response(session.id, msg.id(), ex_type, ex_type));
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
      debug_write("[nREPL DEBUG] CAUGHT jank::error_ref exception\n");
      sigaction(SIGSEGV, &sa_old_segv, nullptr);
      sigaction(SIGABRT, &sa_old_abrt, nullptr);
      sigaction(SIGBUS, &sa_old_bus, nullptr);
      update_ns();
      emit_pending_output();
      std::cerr << "err source line: " << err->source.start.line << '\n';
      if(!err->notes.empty())
      {
        std::cerr << "first err note line: " << err->notes.front().source.start.line << '\n';
      }
      emit_pending_output();
      std::string const base_err_string{ err->message.data(), err->message.size() };
      std::string const err_type{ error::kind_str(err->kind) };
      auto adjusted_source(err->source);
      apply_source_hints(adjusted_source, line_hint, column_hint);
      auto serialized_error(build_serialized_error(err));
      apply_serialized_error_hints(serialized_error, line_hint, column_hint);
      auto const display_err_string(
        format_error_with_location(base_err_string, err->kind, adjusted_source, file_path));
      record_exception(session, display_err_string, err_type, adjusted_source, serialized_error);
      responses.emplace_back(make_eval_error_response(session.id, msg.id(), err_type, err_type));
      bencode::value::dict err_msg;
      if(!msg.id().empty())
      {
        err_msg.emplace("id", msg.id());
      }
      err_msg.emplace("session", session.id);
      err_msg.emplace("status", bencode::list_of_strings({ "error" }));
      err_msg.emplace("err", display_err_string);
      err_msg.emplace("exception-type", err_type);
      auto const file_string(to_std_string(adjusted_source.file));
      if(file_string != jank::read::no_source_path)
      {
        err_msg.emplace("file", file_string);
      }
      if(adjusted_source.start.line != 0)
      {
        err_msg.emplace("line", std::to_string(adjusted_source.start.line));
      }
      if(adjusted_source.start.col != 0)
      {
        err_msg.emplace("column", std::to_string(adjusted_source.start.col));
      }
      err_msg.emplace("jank/error", bencode::value{ encode_error(serialized_error) });
      responses.emplace_back(std::move(err_msg));
      responses.emplace_back(make_done_response(session.id, msg.id(), { "done", "error" }));
    }
    catch(std::exception const &ex)
    {
      debug_write("[nREPL DEBUG] CAUGHT std::exception\n");
      sigaction(SIGSEGV, &sa_old_segv, nullptr);
      sigaction(SIGABRT, &sa_old_abrt, nullptr);
      sigaction(SIGBUS, &sa_old_bus, nullptr);
      update_ns();
      emit_pending_output();
      auto const ex_type(std::string{ typeid(ex).name() });
      record_exception(session, std::string{ ex.what() }, ex_type, std::nullopt);
      responses.emplace_back(make_eval_error_response(session.id, msg.id(), ex_type, ex_type));
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
      debug_write("[nREPL DEBUG] CAUGHT unknown exception (...)\n");
      sigaction(SIGSEGV, &sa_old_segv, nullptr);
      sigaction(SIGABRT, &sa_old_abrt, nullptr);
      sigaction(SIGBUS, &sa_old_bus, nullptr);
      update_ns();
      emit_pending_output();
      std::string const ex_type{ "unknown" };
      record_exception(session, "unknown exception", ex_type, std::nullopt);
      responses.emplace_back(make_eval_error_response(session.id, msg.id(), ex_type, ex_type));
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
