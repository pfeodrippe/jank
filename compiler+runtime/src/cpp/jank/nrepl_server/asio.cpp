#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <pthread.h>

/* Enable GC thread support. We need explicit forward declarations because
 * some system gc.h headers may not expose these even with GC_THREADS defined. */
#define GC_THREADS
#include <gc/gc.h>

/* Forward declarations for GC thread registration functions.
 * These ensure the functions are declared even if gc.h doesn't expose them
 * (e.g., when using system libgc that wasn't built with thread support).
 * If the library doesn't have them, we'll get a link error which is more
 * diagnosable than a compile error in clang-tidy. */
extern "C"
{
  GC_API void GC_CALL GC_allow_register_threads(void);
  GC_API int GC_CALL GC_register_my_thread(const struct GC_stack_base *);
  GC_API int GC_CALL GC_unregister_my_thread(void);
  GC_API int GC_CALL GC_get_stack_base(struct GC_stack_base *);
}

#ifndef BOOST_ERROR_CODE_HEADER_ONLY
  #define BOOST_ERROR_CODE_HEADER_ONLY
#endif
#ifndef BOOST_SYSTEM_NO_DEPRECATED
  #define BOOST_SYSTEM_NO_DEPRECATED
#endif

/* Boost.Asio injects several inline definitions for private nested helpers.        */
/* Clang strictly enforces access there when compiling generated snippets, so we   */
/* temporarily relax the access keywords during the includes.                      */
/* Pre-include <any> to avoid GCC 14 header bug - its <any> has private forward    */
/* declarations with public definitions, which Clang rejects when 'private' is     */
/* redefined to 'public'. Including it first ensures correct access specifiers.    */
#include <any>
#if defined(__clang__)
  #pragma clang diagnostic push
  #pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#define protected public
#include <boost/asio/ts/buffer.hpp>
#include <boost/asio/ts/internet.hpp>
#include <boost/asio/write.hpp>
#undef private
#undef protected
#if defined(__clang__)
  #pragma clang diagnostic pop
#endif

#include <jank/nrepl_server/engine.hpp>
#include <jank/runtime/behavior/callable.hpp>
#include <jank/runtime/context.hpp>
#include <jank/runtime/core.hpp>
#include <jank/runtime/module/loader.hpp>
#include <jank/runtime/ns.hpp>
#include <jank/runtime/object.hpp>
#include <jank/runtime/obj/native_function_wrapper.hpp>
#include <jank/runtime/obj/nil.hpp>
#include <jank/runtime/obj/symbol.hpp>
#include <jank/runtime/obj/persistent_hash_map.hpp>
#include <jank/runtime/var.hpp>

using jank_object_ref = void *;

namespace jank::nrepl_server::asio
{
  using namespace jank;
  using namespace jank::runtime;
  using boost::asio::ip::tcp;

  class connection : public std::enable_shared_from_this<connection>
  {
  public:
    connection(tcp::socket &&socket, engine &eng)
      : socket_{ std::move(socket) }
      , engine_{ eng }
    {
    }

    void start()
    {
      do_read();
    }

  private:
    void do_read()
    {
      auto self(shared_from_this());
      socket_.async_read_some(
        boost::asio::buffer(read_buffer_),
        [this, self](boost::system::error_code ec, std::size_t length) { on_read(ec, length); });
    }

    void on_read(boost::system::error_code const ec, std::size_t const length)
    {
      if(ec)
      {
        return;
      }

      buffer_.append(read_buffer_.data(), length);
      process_buffer();
      do_read();
    }

    void process_buffer()
    {
      while(!buffer_.empty())
      {
        auto const decoded(bencode::decode(std::string_view{ buffer_.data(), buffer_.size() }));
        if(decoded.state == bencode::parse_state::need_more)
        {
          return;
        }

        if(decoded.state != bencode::parse_state::ok)
        {
          std::cerr << "bencode decode error: " << decoded.error << '\n';
          boost::system::error_code close_ec;
          auto const close_result(socket_.close(close_ec));
          if(close_result)
          {
            std::cerr << "socket close error: " << close_result.message() << '\n';
          }
          return;
        }

        if(!decoded.data.is_dict())
        {
          std::cerr << "invalid nREPL payload" << '\n';
          boost::system::error_code close_ec;
          auto const close_result(socket_.close(close_ec));
          if(close_result)
          {
            std::cerr << "socket close error: " << close_result.message() << '\n';
          }
          return;
        }

        message const msg{ decoded.data.as_dict() };
        auto responses(engine_.handle(msg));
        for(auto &payload : responses)
        {
          std::string encoded;
          encoded.reserve(256);
          bencode::value const wrapper{ payload };
          bencode::encode_value(wrapper, encoded);
          enqueue_write(std::move(encoded));
        }

        buffer_.erase(0, decoded.consumed);
      }
    }

    void enqueue_write(std::string response)
    {
      write_queue_.push_back(std::move(response));
      if(!writing_)
      {
        do_write();
      }
    }

    void do_write()
    {
      if(write_queue_.empty())
      {
        writing_ = false;
        return;
      }

      writing_ = true;
      auto self(shared_from_this());
      auto &front(write_queue_.front());
      boost::asio::async_write(
        socket_,
        boost::asio::buffer(front),
        [this, self](boost::system::error_code ec, std::size_t) { on_write(ec); });
    }

    void on_write(boost::system::error_code const ec)
    {
      if(ec)
      {
        return;
      }

      write_queue_.pop_front();
      if(write_queue_.empty())
      {
        writing_ = false;
      }
      else
      {
        do_write();
      }
    }

    tcp::socket socket_;
    engine &engine_;
    std::string buffer_{};
    std::array<char, 4096> read_buffer_{};
    std::deque<std::string> write_queue_;
    bool writing_{ false };
  };

  /* Global flag to track application shutdown.
   * When true, IO threads should NOT call GC_unregister_my_thread() since
   * the GC may already be partially destroyed during atexit() processing. */
  std::atomic<bool> shutting_down{ false };

  // Server class to manage an embeddable nREPL server instance
  class server
  {
  public:
    server(int port, std::string const &bind_address)
      : bind_address_{ bind_address }
      , io_context_{}
      , engine_{ std::make_shared<engine>() }
      , work_guard_{ boost::asio::make_work_guard(io_context_) }
    {
      // Create acceptor
      auto const address(boost::asio::ip::make_address(bind_address));
      acceptor_ = std::make_unique<tcp::acceptor>(io_context_, tcp::endpoint(address, port));
      actual_port_ = acceptor_->local_endpoint().port();

      // Start accepting connections
      accept_connection();

      // Run io_context in a separate thread with large stack size
      // Large stack is needed for complex C++ headers like flecs.h that trigger
      // deep template instantiation in Clang (can exceed 500+ stack frames)
      // Must register with Boehm GC since this thread will allocate GC memory during eval
      GC_allow_register_threads();

      pthread_attr_t attr;
      pthread_attr_init(&attr);
      // Set stack size to 16MB (default is ~512KB on macOS, too small for flecs.h)
      constexpr size_t LARGE_STACK_SIZE = static_cast<size_t>(16) * 1024 * 1024;
      pthread_attr_setstacksize(&attr, LARGE_STACK_SIZE);

      /* Note: With GC_THREADS defined, pthread_create is redirected to GC_pthread_create,
       * which automatically handles GC thread registration/unregistration via its wrapper.
       * We should NOT manually call GC_register_my_thread/GC_unregister_my_thread as this
       * causes conflicts with the automatic wrapper and can crash during shutdown. */
      auto thread_func = [](void *arg) -> void * {
        auto *self = static_cast<server *>(arg);
        /* Set up thread bindings for dynamic vars like *ns*.
         * This is required because eval operations may call (in-ns ...) which
         * needs *ns* to be thread-bound. */
        jank::runtime::context::binding_scope bindings;
        self->io_context_.run();
        return nullptr;
      };

      pthread_create(&io_thread_, &attr, thread_func, this);
      pthread_attr_destroy(&attr);
    }

    ~server()
    {
      stop();
    }

    void stop()
    {
      if(!running_)
      {
        return;
      }

      running_ = false;
      work_guard_.reset();

      if(acceptor_)
      {
        boost::system::error_code ec;
        auto const close_result(acceptor_->close(ec));
        if(close_result)
        {
          std::cerr << "acceptor close error: " << close_result.message() << '\n';
        }
        acceptor_.reset();
      }

      io_context_.stop();

      /* During application shutdown (atexit or static destruction), the GC's thread
       * wrapper (GC_pthread_start_inner) will call cleanup functions that may crash
       * if the GC is being torn down. To avoid this, we detach the thread during
       * shutdown instead of joining - the process is exiting anyway. */
      if(shutting_down.load(std::memory_order_acquire))
      {
        pthread_detach(io_thread_);
      }
      else
      {
        pthread_join(io_thread_, nullptr);
      }
    }

    int get_port() const
    {
      return actual_port_;
    }

  private:
    void accept_connection()
    {
      if(!running_)
      {
        return;
      }

      auto next_socket = std::make_shared<tcp::socket>(io_context_);
      acceptor_->async_accept(
        *next_socket,
        [this, next_socket](boost::system::error_code const ec) {
          if(!ec && engine_ && running_)
          {
            auto conn(std::make_shared<connection>(std::move(*next_socket), *engine_));
            conn->start();
          }
          else if(ec && running_)
          {
            std::cerr << "accept error: " << ec.message() << '\n';
          }

          accept_connection();
        });
    }

    int actual_port_;
    std::string bind_address_;
    boost::asio::io_context io_context_;
    std::shared_ptr<engine> engine_;
    std::unique_ptr<tcp::acceptor> acceptor_;
    pthread_t io_thread_{};
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_guard_;
    bool running_{ true };
  };

  /* Forward declarations for shutdown handling. These are defined in the anonymous namespace
   * below but need to be callable from here for the static initialization. */
  namespace
  {
    std::map<std::uintptr_t, std::shared_ptr<server>> &server_registry();
    void shutdown_all_servers();
  }

  /* Static initialization to ensure the atexit handler is always registered when any code
   * using nREPL servers is loaded. This runs before main() and guarantees cleanup happens
   * before the GC is torn down during process exit. */
  namespace
  {
    struct shutdown_handler_registrar
    {
      shutdown_handler_registrar()
      {
        std::atexit(&shutdown_all_servers);
      }
    };

    static shutdown_handler_registrar const registrar_instance{};
  }

  namespace
  {
    class port_file_guard
    {
    public:
      explicit port_file_guard(std::filesystem::path path)
        : path_{ std::move(path) }
      {
      }

      void write(std::int64_t const port) const
      {
        std::ofstream ofs{ path_ };
        if(ofs)
        {
          ofs << port;
        }
      }

      ~port_file_guard()
      {
        std::error_code ec;
        [[maybe_unused]]
        bool const removed(std::filesystem::remove(path_, ec));
        if(ec)
        {
          std::cerr << "failed to remove nREPL port file: " << ec.message() << '\n';
        }
      }

    private:
      std::filesystem::path path_;
    };

    std::map<std::uintptr_t, std::shared_ptr<server>> &server_registry()
    {
      /* IMPORTANT: We intentionally heap-allocate and LEAK this map.
       * Using a static local would cause the map's destructor to run during
       * __cxa_finalize_ranges, which crashes because:
       * 1. The destructor calls clear() to destroy each server
       * 2. Server destructors may access GC-managed memory that's already freed
       * 3. Even with pthread_detach, the map tree traversal itself can crash
       *    if the allocator or memory has been torn down
       *
       * By leaking the map, we avoid the destructor entirely. The process is
       * exiting anyway, so the OS will reclaim all memory. */
      static auto *servers = new std::map<std::uintptr_t, std::shared_ptr<server>>();
      return *servers;
    }

    /* Shutdown handler called via atexit() to signal that application shutdown is in progress.
     * IMPORTANT: We ONLY set the flag here - we do NOT try to clear the server_registry.
     * The registry will be cleaned up by normal static destruction. If we try to clear()
     * the map here, we race with static destructors and may access already-freed memory.
     *
     * The flag tells server destructors to use pthread_detach instead of pthread_join,
     * which avoids blocking on GC cleanup during shutdown. */
    void shutdown_all_servers()
    {
      shutting_down.store(true, std::memory_order_release);
      /* Do NOT call server_registry().clear() here - it races with static destruction
       * and causes crashes when the map's internal tree nodes are already destroyed. */
    }

    /* Ensure the shutdown handler is registered exactly once.
     * Uses a static local for thread-safe one-time initialization. */
    void ensure_shutdown_handler_registered()
    {
      static bool const registered = []() {
        std::atexit(&shutdown_all_servers);
        return true;
      }();
      (void)registered;
    }

    object_ref run_server(object_ref const port_obj)
    {
      bootstrap_runtime_once();

      auto const port(to_int(port_obj));
      std::cout << "Starting jank nREPL on port " << port << '\n';

      boost::asio::io_context io_context;
      auto server_engine = std::make_shared<engine>();
      tcp::acceptor acceptor{ io_context, tcp::endpoint(tcp::v4(), port) };
      port_file_guard const port_file{ ".nrepl-port" };
      port_file.write(port);

      auto accept_loop = std::make_shared<std::function<void()>>();
      *accept_loop = [&io_context, &acceptor, server_engine, accept_loop]() {
        auto socket = std::make_shared<tcp::socket>(io_context);
        acceptor.async_accept(
          *socket,
          [&acceptor, server_engine, socket, accept_loop](boost::system::error_code const ec) {
            if(ec)
            {
              if(ec != boost::asio::error::operation_aborted)
              {
                std::cerr << "accept error: " << ec.message() << '\n';
              }
              return;
            }

            auto conn(std::make_shared<connection>(std::move(*socket), *server_engine));
            conn->start();
            if(acceptor.is_open())
            {
              (*accept_loop)();
            }
          });
      };

      (*accept_loop)();
      io_context.run();
      return runtime::jank_nil();
    }

    object_ref start_server(object_ref const port_obj, object_ref const bind_obj)
    {
      bootstrap_runtime_once();
      ensure_shutdown_handler_registered();

      auto const port(to_int(port_obj));
      auto const bind_address(to_std_string(runtime::to_string(bind_obj)));

      try
      {
        auto srv = std::make_shared<server>(port, bind_address);
        auto const ptr_val = reinterpret_cast<std::uintptr_t>(srv.get());
        server_registry()[ptr_val] = srv;

        auto const srv_ptr_kw(__rt_ctx->intern_keyword("server-ptr").expect_ok());
        auto const port_kw(__rt_ctx->intern_keyword("port").expect_ok());

        auto result = make_box<obj::persistent_hash_map>();
        result = runtime::assoc(result, srv_ptr_kw, make_box(static_cast<int64_t>(ptr_val)));
        result = runtime::assoc(result, port_kw, make_box(static_cast<int64_t>(srv->get_port())));

        return result;
      }
      catch(std::exception const &e)
      {
        std::cerr << "Failed to start nREPL server: " << e.what() << '\n';
        return runtime::jank_nil();
      }
    }

    object_ref stop_server(object_ref const server_obj)
    {
      if(server_obj == runtime::jank_nil())
      {
        return runtime::jank_nil();
      }

      auto const server_map(expect_object<obj::persistent_hash_map>(server_obj));
      auto const srv_ptr_kw(__rt_ctx->intern_keyword("server-ptr").expect_ok());
      auto const ptr_obj(runtime::get(server_map, srv_ptr_kw));

      if(ptr_obj == runtime::jank_nil())
      {
        return runtime::jank_nil();
      }

      auto const ptr_val(static_cast<std::uintptr_t>(to_int(ptr_obj)));
      server_registry().erase(ptr_val);

      return runtime::jank_nil();
    }

    object_ref get_server_port(object_ref const server_obj)
    {
      if(server_obj == runtime::jank_nil())
      {
        return runtime::jank_nil();
      }

      auto const server_map(expect_object<obj::persistent_hash_map>(server_obj));
      auto const port_kw(__rt_ctx->intern_keyword("port").expect_ok());
      return runtime::get(server_map, port_kw);
    }
  }

  struct __ns : behavior::callable
  {
    object_ref call() override
    {
      auto const ns(__rt_ctx->intern_ns("jank.nrepl-server.asio"));
      // Legacy blocking run function
      ns->intern_var("run!")->bind_root(make_box<obj::native_function_wrapper>(&run_server));
      // Embeddable server functions
      ns->intern_var("start!")->bind_root(make_box<obj::native_function_wrapper>(&start_server));
      ns->intern_var("stop!")->bind_root(make_box<obj::native_function_wrapper>(&stop_server));
      ns->intern_var("get-port")
        ->bind_root(make_box<obj::native_function_wrapper>(&get_server_port));

      constexpr char module_name[]{ "jank.nrepl-server.asio" };
      if(!__rt_ctx->module_loader.is_loaded(module_name))
      {
        __rt_ctx->module_loader.set_is_loaded(module_name);
        auto const locked_modules{ __rt_ctx->loaded_modules_in_order.wlock() };
        locked_modules->emplace_back(module_name);
      }
      return runtime::jank_nil();
    }

    object_ref this_object_ref() override
    {
      return runtime::jank_nil();
    }
  };
}

/* The 'used' and 'visibility' attributes prevent the linker from stripping
 * this symbol during dead code elimination. This is needed for iOS JIT where
 * native modules are looked up via dlsym at runtime. */
extern "C" __attribute__((used, visibility("default"))) void
jank_load_jank_nrepl_server_asio()
{
  jank::nrepl_server::asio::__ns loader;
  loader.call();
}

/* Start the nREPL server on the given port and bind address.
 * Returns a server handle that can be used to stop the server,
 * or nullptr if the server failed to start.
 * This is a C API wrapper for iOS to start the full nREPL server. */
extern "C" __attribute__((used, visibility("default"))) jank_object_ref
jank_nrepl_start_server(int port, char const *bind_address)
{
  using namespace jank;
  using namespace jank::runtime;

  try
  {
    // NOTE: In hybrid mode, jank_aot_init() has already loaded clojure.core
    // via AOT compilation. We just need to set up the user namespace.
    // Set up user namespace (clojure.core should already be loaded)
    dynamic_call(__rt_ctx->in_ns_var->deref(), make_box<obj::symbol>("user"));
    auto refer_var = __rt_ctx->intern_var("clojure.core", "refer");
    if(refer_var.is_ok())
    {
      dynamic_call(refer_var.expect_ok(), make_box<obj::symbol>("clojure.core"));
    }

    std::cout << "[nrepl] Creating server on port " << port << "..." << std::endl;

    auto const port_obj = make_box(static_cast<int64_t>(port));
    auto const bind_obj = make_box<obj::persistent_string>(bind_address ? bind_address : "0.0.0.0");

    auto result = jank::nrepl_server::asio::start_server(port_obj, bind_obj);

    if(result == jank_nil())
    {
      std::cerr << "[nrepl] start_server returned nil" << std::endl;
      return nullptr;
    }

    std::cout << "[nrepl] Server started successfully!" << std::endl;
    return result.erase().data;
  }
  catch(jtl::ref<error::base> const &e)
  {
    std::cerr << "[nrepl] Error starting server: " << e->message << std::endl;
    if(e->cause)
    {
      std::cerr << "[nrepl]   caused by: " << e->cause->message << std::endl;
    }
    return nullptr;
  }
  catch(object_ref const &e)
  {
    std::cerr << "[nrepl] Jank exception: " << to_code_string(e) << std::endl;
    return nullptr;
  }
  catch(std::exception const &e)
  {
    std::cerr << "[nrepl] Exception: " << e.what() << std::endl;
    return nullptr;
  }
  catch(...)
  {
    std::cerr << "[nrepl] Unknown exception starting server" << std::endl;
    return nullptr;
  }
}

/* Stop the nREPL server given the server handle from jank_nrepl_start_server. */
extern "C" __attribute__((used, visibility("default"))) void
jank_nrepl_stop_server(jank_object_ref server_handle)
{
  if(server_handle)
  {
    jank::nrepl_server::asio::stop_server(
      jank::runtime::object_ref{ static_cast<jank::runtime::object *>(server_handle) });
  }
}
