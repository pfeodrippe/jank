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
#include <jank/jit/processor.hpp>

namespace jank::nrepl_server::asio
{
  // Global state for signal handling - thread-local to handle multiple connections
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
  inline thread_local jmp_buf eval_jmp_buf;
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
  inline thread_local volatile sig_atomic_t signal_received = 0;
  // Thread-local alternate signal stack for handling stack overflow
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
  inline thread_local void *alt_stack_memory = nullptr;
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
  inline thread_local bool alt_stack_installed = false;

  // Size of alternate stack (128KB should be enough for signal handler + longjmp)
  constexpr size_t ALT_STACK_SIZE = static_cast<size_t>(128) * 1024;

  inline void ensure_alt_stack()
  {
    if(alt_stack_installed)
    {
      return;
    }

    // Allocate alternate stack memory
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc): signal stack needs raw memory
    alt_stack_memory = std::malloc(ALT_STACK_SIZE);
    if(alt_stack_memory == nullptr)
    {
      std::cerr << "Failed to allocate alternate signal stack\n";
      return;
    }

    // Set up the alternate stack
    stack_t ss{};
    ss.ss_sp = alt_stack_memory;
    ss.ss_size = ALT_STACK_SIZE;
    ss.ss_flags = 0;

    if(sigaltstack(&ss, nullptr) != 0)
    {
      std::cerr << "Failed to install alternate signal stack: " << strerror(errno) << "\n";
      // NOLINTNEXTLINE(cppcoreguidelines-no-malloc): signal stack needs raw memory
      std::free(alt_stack_memory);
      alt_stack_memory = nullptr;
      return;
    }

    alt_stack_installed = true;
  }

  inline void eval_signal_handler(int sig)
  {
    signal_received = sig;
    // NOLINTNEXTLINE(modernize-avoid-setjmp-longjmp)
    longjmp(eval_jmp_buf, sig);
  }

  inline std::vector<bencode::value::dict> engine::handle_eval(message const &msg)
  {
    auto const code(msg.get("code"));
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
        std::make_pair(__rt_ctx->current_file_var, make_box(make_immutable_string(file_path))));
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

    // Set up signal handlers to catch crashes during eval
    signal_received = 0;

    // Ensure we have an alternate signal stack for stack overflow recovery
    ensure_alt_stack();

    struct sigaction sa_new{}, sa_old_segv{}, sa_old_abrt{}, sa_old_bus{};
    memset(&sa_new, 0, sizeof(sa_new));
    sa_new.sa_handler = eval_signal_handler;
    sigemptyset(&sa_new.sa_mask);
    // SA_ONSTACK: use alternate stack so handler can run even when main stack overflows
    sa_new.sa_flags = SA_ONSTACK;

    sigaction(SIGSEGV, &sa_new, &sa_old_segv);
    sigaction(SIGABRT, &sa_new, &sa_old_abrt);
    sigaction(SIGBUS, &sa_new, &sa_old_bus);

    // Set up longjmp point
    // NOLINTNEXTLINE(modernize-avoid-setjmp-longjmp,misc-const-correctness)
    int const jmp_result = setjmp(eval_jmp_buf);
    if(jmp_result != 0)
    {
      // We got here via longjmp from signal handler
      // Restore signal handlers
      sigaction(SIGSEGV, &sa_old_segv, nullptr);
      sigaction(SIGABRT, &sa_old_abrt, nullptr);
      sigaction(SIGBUS, &sa_old_bus, nullptr);

      // Create error response
      char sig_buf[256];
      if(jmp_result == jit::JIT_FATAL_ERROR_SIGNAL)
      {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): signal handler context
        std::snprintf(sig_buf,
                      sizeof(sig_buf),
                      "JIT/LLVM fatal error during eval (check stderr for details)");
      }
      else if(jmp_result == SIGSEGV)
      {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): signal handler context
        std::snprintf(sig_buf,
                      sizeof(sig_buf),
                      "Stack overflow or segmentation fault during eval (signal %d). This can "
                      "happen with large/complex C++ headers like flecs.h",
                      jmp_result);
      }
      else
      {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): signal handler context
        std::snprintf(sig_buf, sizeof(sig_buf), "Caught signal %d during eval", jmp_result);
      }

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

      return responses;
    }

    // Register eval_jmp_buf with JIT so LLVM fatal errors can recover too
    jit::scoped_jit_recovery const jit_recovery{ eval_jmp_buf };

    try
    {
      jtl::immutable_string_view const code_view{ code.data(), code.size() };
      auto const result(__rt_ctx->eval_string(code_view));

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
