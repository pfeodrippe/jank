#pragma once

// Minimal iOS Eval Server
//
// Like ClojureScript's browser client, this is intentionally simple:
// - Receive code string
// - Call eval_string()
// - Return result
//
// All nREPL complexity (middleware, sessions, completion) stays on macOS.
// This avoids the stack overflow caused by deep template instantiation.
//
// Protocol (JSON over TCP, newline-delimited):
//   Client → Server: {"op":"eval","id":1,"code":"(+ 1 2)","ns":"user"}
//   Server → Client: {"op":"result","id":1,"value":"3"}
//   Server → Client: {"op":"error","id":1,"error":"...","type":"compile|runtime"}
//
// See: /ai/20251223-ios-remote-nrepl-architecture.md

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <memory>
#include <cstring>
#include <iostream>
#include <csignal>
#include <csetjmp>

// BSD sockets (available on iOS)
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

// Boehm GC - for thread registration
// Note: gc.h is included elsewhere without GC_THREADS, so we declare functions explicitly
struct GC_stack_base; // Forward declaration (struct defined in gc.h)
extern "C" int GC_get_stack_base(struct GC_stack_base *);
extern "C" void GC_allow_register_threads(void); // Must be called before GC_register_my_thread
extern "C" int GC_register_my_thread(const struct GC_stack_base *);
extern "C" int GC_unregister_my_thread(void);
#ifndef GC_SUCCESS
  #define GC_SUCCESS 0
#endif

#ifdef __APPLE__
  #include <TargetConditionals.h>
#endif

// Only include on non-WASM platforms
#if !defined(__EMSCRIPTEN__)

  #include <jank/runtime/context.hpp>
  #include <jank/runtime/core/to_string.hpp>
  #include <jank/error.hpp>
  #include <jank/read/source.hpp>

  // Remote compilation support
  #include <jank/compile_server/remote_eval.hpp>

namespace jank::ios
{
  // Simple JSON helpers (avoid heavy dependencies)
  namespace json
  {
    inline std::string escape(std::string const &s)
    {
      std::string result;
      result.reserve(s.size() + 16);
      for(char c : s)
      {
        switch(c)
        {
          case '"':
            result += "\\\"";
            break;
          case '\\':
            result += "\\\\";
            break;
          case '\n':
            result += "\\n";
            break;
          case '\r':
            result += "\\r";
            break;
          case '\t':
            result += "\\t";
            break;
          default:
            result += c;
        }
      }
      return result;
    }

    inline std::string get_string(std::string const &json, std::string const &key)
    {
      auto key_pos = json.find("\"" + key + "\"");
      if(key_pos == std::string::npos)
      {
        return "";
      }
      auto colon_pos = json.find(':', key_pos);
      if(colon_pos == std::string::npos)
      {
        return "";
      }
      auto quote_start = json.find('"', colon_pos);
      if(quote_start == std::string::npos)
      {
        return "";
      }
      auto quote_end = quote_start + 1;
      while(quote_end < json.size())
      {
        if(json[quote_end] == '"' && json[quote_end - 1] != '\\')
        {
          break;
        }
        quote_end++;
      }
      // Unescape the string
      std::string result;
      for(size_t i = quote_start + 1; i < quote_end; i++)
      {
        if(json[i] == '\\' && i + 1 < quote_end)
        {
          switch(json[i + 1])
          {
            case 'n':
              result += '\n';
              i++;
              break;
            case 'r':
              result += '\r';
              i++;
              break;
            case 't':
              result += '\t';
              i++;
              break;
            case '"':
              result += '"';
              i++;
              break;
            case '\\':
              result += '\\';
              i++;
              break;
            default:
              result += json[i];
          }
        }
        else
        {
          result += json[i];
        }
      }
      return result;
    }

    inline int64_t get_int(std::string const &json, std::string const &key)
    {
      auto key_pos = json.find("\"" + key + "\"");
      if(key_pos == std::string::npos)
      {
        return 0;
      }
      auto colon_pos = json.find(':', key_pos);
      if(colon_pos == std::string::npos)
      {
        return 0;
      }
      // Skip whitespace
      auto num_start = colon_pos + 1;
      while(num_start < json.size() && (json[num_start] == ' ' || json[num_start] == '\t'))
      {
        num_start++;
      }
      try
      {
        return std::stoll(json.substr(num_start));
      }
      catch(...)
      {
        return 0;
      }
    }
  }

  class eval_server
  {
  public:
    eval_server(uint16_t port = 5558)
      : port_(port)
      , running_(false)
      , server_fd_(-1)
      , use_remote_compile_(false)
    {
    }

    // Enable remote compilation to macOS
    void enable_remote_compile(std::string const &compile_host,
                               uint16_t compile_port = compile_server::default_compile_port)
    {
      compile_server::init_remote_eval(compile_host, compile_port);
      use_remote_compile_ = true;
      std::cout << "[ios-eval] Remote compilation enabled: " << compile_host << ":" << compile_port
                << std::endl;
    }

    void disable_remote_compile()
    {
      use_remote_compile_ = false;
      std::cout << "[ios-eval] Remote compilation disabled" << std::endl;
    }

    bool is_remote_compile_enabled() const
    {
      return use_remote_compile_;
    }

    ~eval_server()
    {
      stop();
    }

    void start()
    {
      if(running_.exchange(true))
      {
        return; // Already running
      }

      std::cout << "[ios-eval] Starting eval server on port " << port_ << "..." << std::endl;

      // Enable thread registration with GC (must be called from main thread)
      GC_allow_register_threads();

      // Start server on separate thread with large stack
      pthread_attr_t attr;
      pthread_attr_init(&attr);
      pthread_attr_setstacksize(&attr, 8 * 1024 * 1024); // 8MB stack

      auto thread_func = [](void *arg) -> void * {
        auto *self = static_cast<eval_server *>(arg);
        self->run_server();
        return nullptr;
      };

      int err = pthread_create(&server_thread_, &attr, thread_func, this);
      pthread_attr_destroy(&attr);

      if(err != 0)
      {
        std::cerr << "[ios-eval] Failed to create server thread: " << err << std::endl;
        running_ = false;
        return;
      }

      std::cout << "[ios-eval] Server started! Connect from macOS to eval code." << std::endl;
    }

    void stop()
    {
      if(!running_.exchange(false))
      {
        return; // Not running
      }

      std::cout << "[ios-eval] Stopping eval server..." << std::endl;

      // Close the server socket to interrupt accept()
      if(server_fd_ >= 0)
      {
        ::close(server_fd_);
        server_fd_ = -1;
      }

      // Wait for thread
      pthread_join(server_thread_, nullptr);

      std::cout << "[ios-eval] Server stopped." << std::endl;
    }

    bool is_running() const
    {
      return running_;
    }

    uint16_t port() const
    {
      return port_;
    }

  private:
    void run_server()
    {
      // Register this thread with Boehm GC
      // This is critical - without it, GC will crash when scanning stacks
      struct GC_stack_base sb;
      if(GC_get_stack_base(&sb) != GC_SUCCESS)
      {
        std::cerr << "[ios-eval] Failed to get stack base for GC registration" << std::endl;
        running_ = false;
        return;
      }
      if(GC_register_my_thread(&sb) != GC_SUCCESS)
      {
        std::cerr << "[ios-eval] Failed to register thread with GC" << std::endl;
        running_ = false;
        return;
      }
      std::cout << "[ios-eval] Thread registered with GC" << std::endl;

      // Create socket
      server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
      if(server_fd_ < 0)
      {
        std::cerr << "[ios-eval] Failed to create socket: " << strerror(errno) << std::endl;
        running_ = false;
        return;
      }

      // Allow address reuse
      int opt = 1;
      setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

      // Bind to port
      struct sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = INADDR_ANY;
      addr.sin_port = htons(port_);

      if(bind(server_fd_, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0)
      {
        std::cerr << "[ios-eval] Failed to bind to port " << port_ << ": " << strerror(errno)
                  << std::endl;
        ::close(server_fd_);
        server_fd_ = -1;
        running_ = false;
        return;
      }

      // Listen
      if(listen(server_fd_, 5) < 0)
      {
        std::cerr << "[ios-eval] Failed to listen: " << strerror(errno) << std::endl;
        ::close(server_fd_);
        server_fd_ = -1;
        running_ = false;
        return;
      }

      std::cout << "[ios-eval] Listening on 0.0.0.0:" << port_ << std::endl;
      std::cout << "[ios-eval] Protocol: JSON over TCP (newline-delimited)" << std::endl;

      // Accept loop
      while(running_)
      {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        int client_fd
          = accept(server_fd_, reinterpret_cast<struct sockaddr *>(&client_addr), &client_len);
        if(client_fd < 0)
        {
          if(running_)
          {
            std::cerr << "[ios-eval] Accept error: " << strerror(errno) << std::endl;
          }
          continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        std::cout << "[ios-eval] Client connected from " << client_ip << ":"
                  << ntohs(client_addr.sin_port) << std::endl;

        // Handle connection
        handle_connection(client_fd);
      }

      if(server_fd_ >= 0)
      {
        ::close(server_fd_);
        server_fd_ = -1;
      }

      // Unregister thread from GC before exiting
      GC_unregister_my_thread();
      std::cout << "[ios-eval] Thread unregistered from GC" << std::endl;
    }

    void handle_connection(int client_fd)
    {
      // Send welcome message
      std::string welcome = R"({"op":"welcome","runtime":"jank-ios","version":"0.1"})"
                            "\n";
      send(client_fd, welcome.c_str(), welcome.size(), 0);

      std::string buffer;
      char recv_buf[4096];

      while(running_)
      {
        ssize_t n = recv(client_fd, recv_buf, sizeof(recv_buf) - 1, 0);
        if(n <= 0)
        {
          if(n < 0 && errno != ECONNRESET)
          {
            std::cerr << "[ios-eval] Recv error: " << strerror(errno) << std::endl;
          }
          break;
        }

        recv_buf[n] = '\0';
        buffer += recv_buf;

        // Process complete lines
        size_t pos;
        while((pos = buffer.find('\n')) != std::string::npos)
        {
          std::string line = buffer.substr(0, pos);
          buffer.erase(0, pos + 1);

          // Remove trailing \r if present
          if(!line.empty() && line.back() == '\r')
          {
            line.pop_back();
          }

          if(line.empty())
          {
            continue;
          }

          // Handle message and send response
          std::string response = handle_message(line) + "\n";
          if(send(client_fd, response.c_str(), response.size(), 0) < 0)
          {
            std::cerr << "[ios-eval] Send error: " << strerror(errno) << std::endl;
            goto connection_end;
          }
        }
      }

connection_end:
      ::close(client_fd);
      std::cout << "[ios-eval] Client disconnected." << std::endl;
    }

    std::string handle_message(std::string const &msg)
    {
      auto op = json::get_string(msg, "op");
      auto id = json::get_int(msg, "id");

      if(op == "eval")
      {
        auto code = json::get_string(msg, "code");
        auto ns = json::get_string(msg, "ns");

        if(code.empty())
        {
          return R"({"op":"error","id":)" + std::to_string(id)
            + R"(,"error":"Missing 'code' field","type":"protocol"})";
        }

        return eval_code(id, code, ns);
      }
      else if(op == "ping")
      {
        return R"({"op":"pong","id":)" + std::to_string(id) + "}";
      }
      else if(op == "shutdown")
      {
        running_ = false;
        return R"({"op":"goodbye","id":)" + std::to_string(id) + "}";
      }
      else
      {
        return R"({"op":"error","id":)" + std::to_string(id) + R"(,"error":"Unknown op: )"
          + json::escape(op) + R"(","type":"protocol"})";
      }
    }

    std::string eval_code(int64_t id, std::string const &code, std::string const &ns)
    {
      // Setup signal recovery for crashes
      static thread_local sigjmp_buf jmp_buf;
      static thread_local volatile sig_atomic_t signal_received = 0;

      struct sigaction sa_new{}, sa_old_segv{}, sa_old_bus{}, sa_old_abrt{};
      sa_new.sa_handler = [](int sig) {
        signal_received = sig;
        siglongjmp(jmp_buf, sig);
      };
      sa_new.sa_flags = SA_ONSTACK;
      sigemptyset(&sa_new.sa_mask);

      sigaction(SIGSEGV, &sa_new, &sa_old_segv);
      sigaction(SIGBUS, &sa_new, &sa_old_bus);
      sigaction(SIGABRT, &sa_new, &sa_old_abrt);

      std::string result;
      int jmp_result = sigsetjmp(jmp_buf, 1);

      if(jmp_result == 0)
      {
        // Normal evaluation path
        try
        {
          runtime::object_ref obj;

          if(use_remote_compile_)
          {
            // Remote compilation: send code to macOS, load resulting object file
            auto *remote = compile_server::get_remote_eval();
            if(!remote)
            {
              throw std::runtime_error("Remote compilation enabled but not initialized");
            }

            auto eval_result = remote->eval(code, ns.empty() ? "user" : ns);
            if(!eval_result.success)
            {
              // Format error as a jank error response
              std::string error_msg = eval_result.error;
              std::string error_type = eval_result.error_type;

              result = R"({"op":"error","id":)" + std::to_string(id) + R"(,"error":")"
                + json::escape(error_msg) + R"(","type":")" + error_type + R"("})";

              // Restore signal handlers and return early
              sigaction(SIGSEGV, &sa_old_segv, nullptr);
              sigaction(SIGBUS, &sa_old_bus, nullptr);
              sigaction(SIGABRT, &sa_old_abrt, nullptr);
              return result;
            }

            obj = eval_result.value;
          }
          else
          {
            // Local compilation (original path)
            // Note: Namespace switching is handled on the macOS side (ios_remote_eval.hpp)
            // by prepending (in-ns ...) to the code before sending to iOS.
            // This keeps the iOS eval server simple.
            obj = runtime::__rt_ctx->eval_string(code).unwrap_or(runtime::jank_nil().erase());
          }
          auto code_str = runtime::to_code_string(obj);
          std::string value_str(code_str.data(), code_str.size());

          result = R"({"op":"result","id":)" + std::to_string(id) + R"(,"value":")"
            + json::escape(value_str) + R"("})";
        }
        catch(runtime::object_ref const &e)
        {
          // Clojure (throw ...) throws runtime objects
          auto msg = runtime::to_code_string(e);
          result = R"({"op":"error","id":)" + std::to_string(id) + R"(,"error":")"
            + json::escape(std::string(msg.data(), msg.size())) + R"(","type":"runtime"})";
        }
        catch(jtl::ref<error::base> const &e)
        {
          // Build comprehensive error message
          std::string error_msg;

          // Add error kind
          error_msg += error::kind_str(e->kind);

          // Add main message if present
          if(!e->message.empty())
          {
            error_msg += ": ";
            error_msg += std::string(e->message.data(), e->message.size());
          }

          // Add source location if available
          if(e->source.file != read::no_source_path)
          {
            error_msg += " at ";
            error_msg += std::string(e->source.file.data(), e->source.file.size());
            error_msg += ":" + std::to_string(e->source.start.line);
            error_msg += ":" + std::to_string(e->source.start.col);
          }

          // Add notes
          for(auto const &note : e->notes)
          {
            error_msg += "\n  ";
            error_msg += std::string(note.message.data(), note.message.size());
          }

          // Add cause
          if(e->cause)
          {
            error_msg += "\nCaused by: ";
            error_msg += std::string(e->cause->message.data(), e->cause->message.size());
          }

          result = R"({"op":"error","id":)" + std::to_string(id) + R"(,"error":")"
            + json::escape(error_msg) + R"(","type":"compile"})";
        }
        catch(error::base const &e)
        {
          // Error by value
          std::string error_msg = error::kind_str(e.kind);
          if(!e.message.empty())
          {
            error_msg += ": ";
            error_msg += std::string(e.message.data(), e.message.size());
          }
          result = R"({"op":"error","id":)" + std::to_string(id) + R"(,"error":")"
            + json::escape(error_msg) + R"(","type":"compile"})";
        }
        catch(error::base *e)
        {
          // Error as pointer
          std::string error_msg = e ? error::kind_str(e->kind) : "null error";
          if(e && !e->message.empty())
          {
            error_msg += ": ";
            error_msg += std::string(e->message.data(), e->message.size());
          }
          result = R"({"op":"error","id":)" + std::to_string(id) + R"(,"error":")"
            + json::escape(error_msg) + R"(","type":"compile"})";
        }
        catch(std::exception const &e)
        {
          result = R"({"op":"error","id":)" + std::to_string(id) + R"(,"error":")"
            + json::escape(e.what()) + R"(","type":"runtime"})";
        }
        catch(char const *e)
        {
          result = R"({"op":"error","id":)" + std::to_string(id) + R"(,"error":")"
            + json::escape(e ? e : "null") + R"(","type":"runtime"})";
        }
        catch(std::string const &e)
        {
          result = R"({"op":"error","id":)" + std::to_string(id) + R"(,"error":")" + json::escape(e)
            + R"(","type":"runtime"})";
        }
        catch(...)
        {
          result = R"({"op":"error","id":)" + std::to_string(id)
            + R"(,"error":"Unknown exception","type":"runtime"})";
        }
      }
      else
      {
        // Recovered from signal (stack overflow, segfault, etc.)
        std::string sig_name;
        switch(signal_received)
        {
          case SIGSEGV:
            sig_name = "SIGSEGV (segmentation fault)";
            break;
          case SIGBUS:
            sig_name = "SIGBUS (bus error)";
            break;
          case SIGABRT:
            sig_name = "SIGABRT (abort)";
            break;
          default:
            sig_name = "signal " + std::to_string(signal_received);
        }
        result = R"({"op":"error","id":)" + std::to_string(id) + R"(,"error":"Crashed: )" + sig_name
          + R"(","type":"crash"})";
      }

      // Restore signal handlers
      sigaction(SIGSEGV, &sa_old_segv, nullptr);
      sigaction(SIGBUS, &sa_old_bus, nullptr);
      sigaction(SIGABRT, &sa_old_abrt, nullptr);

      return result;
    }

    uint16_t port_;
    std::atomic<bool> running_;
    int server_fd_;
    pthread_t server_thread_;
    bool use_remote_compile_;
  };

  // Global server instance for easy access
  inline std::unique_ptr<eval_server> &get_eval_server_ptr()
  {
    static std::unique_ptr<eval_server> server;
    return server;
  }

  // Convenience functions
  inline void start_eval_server(uint16_t port = 5558)
  {
    auto &server_ptr = get_eval_server_ptr();
    if(!server_ptr || !server_ptr->is_running())
    {
      // Recreate with new port if needed
      if(!server_ptr || server_ptr->port() != port)
      {
        if(server_ptr)
        {
          server_ptr->stop();
        }
        server_ptr = std::make_unique<eval_server>(port);
      }
      server_ptr->start();
    }
  }

  // Start eval server with remote compilation enabled
  inline void start_eval_server_with_remote_compile(uint16_t eval_port,
                                                    std::string const &compile_host,
                                                    uint16_t compile_port
                                                    = compile_server::default_compile_port)
  {
    auto &server_ptr = get_eval_server_ptr();
    if(!server_ptr || !server_ptr->is_running())
    {
      // Recreate with new settings
      if(server_ptr)
      {
        server_ptr->stop();
      }
      server_ptr = std::make_unique<eval_server>(eval_port);
      server_ptr->enable_remote_compile(compile_host, compile_port);
      server_ptr->start();
    }
  }

  inline void stop_eval_server()
  {
    auto &server_ptr = get_eval_server_ptr();
    if(server_ptr)
    {
      server_ptr->stop();
    }
  }

  // Enable remote compilation on running server
  inline void enable_remote_compile(std::string const &compile_host,
                                    uint16_t compile_port = compile_server::default_compile_port)
  {
    auto &server_ptr = get_eval_server_ptr();
    if(server_ptr)
    {
      server_ptr->enable_remote_compile(compile_host, compile_port);
    }
  }

} // namespace jank::ios

#endif // !__EMSCRIPTEN__
