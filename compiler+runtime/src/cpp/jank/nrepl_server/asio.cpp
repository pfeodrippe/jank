#include <array>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#ifndef BOOST_ERROR_CODE_HEADER_ONLY
  #define BOOST_ERROR_CODE_HEADER_ONLY
#endif
#ifndef BOOST_SYSTEM_NO_DEPRECATED
  #define BOOST_SYSTEM_NO_DEPRECATED
#endif

/* Boost.Asio injects several inline definitions for private nested helpers.        */
/* Clang strictly enforces access there when compiling generated snippets, so we   */
/* temporarily relax the access keywords during the includes.                      */
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

      // Run io_context in a separate thread
      io_thread_ = std::thread([this]() { io_context_.run(); });
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

      if(io_thread_.joinable())
      {
        io_thread_.join();
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
    std::thread io_thread_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_guard_;
    bool running_{ true };
  };

  namespace
  {
    struct legacy_server_state
    {
      boost::asio::io_context io_context{};
      std::unique_ptr<tcp::acceptor> acceptor{};
      std::unique_ptr<tcp::socket> next_socket{};
      std::shared_ptr<engine> server_engine{};
      std::filesystem::path port_file{ ".nrepl-port" };
    };

    legacy_server_state &legacy_state()
    {
      static legacy_server_state state;
      return state;
    }

    std::map<std::uintptr_t, std::shared_ptr<server>> &server_registry()
    {
      static std::map<std::uintptr_t, std::shared_ptr<server>> servers;
      return servers;
    }

    void legacy_accept_connection()
    {
      auto &state(legacy_state());
      if(!state.acceptor)
      {
        return;
      }
      state.next_socket = std::make_unique<tcp::socket>(state.io_context);
      state.acceptor->async_accept(*state.next_socket, [](boost::system::error_code const ec) {
        auto &state(legacy_state());
        if(!ec && state.server_engine)
        {
          auto conn(std::make_shared<connection>(std::move(*state.next_socket), *state.server_engine));
          conn->start();
        }
        else if(ec)
        {
          std::cerr << "accept error: " << ec.message() << '\n';
        }

        legacy_accept_connection();
      });
    }

    void write_port_file(std::int64_t const port)
    {
      auto &state(legacy_state());
      std::ofstream ofs{ state.port_file };
      if(ofs)
      {
        ofs << port;
      }
    }

    void remove_port_file()
    {
      auto &state(legacy_state());
      std::error_code ec;
      [[maybe_unused]] bool const removed(std::filesystem::remove(state.port_file, ec));
      if(ec)
      {
        std::cerr << "failed to remove nREPL port file: " << ec.message() << '\n';
      }
    }

    object_ref run_server(object_ref const port_obj)
    {
      bootstrap_runtime_once();

      auto const port(to_int(port_obj));
      std::cout << "Starting jank nREPL on port " << port << '\n';

      auto &state(legacy_state());
      state.server_engine = std::make_shared<engine>();
      state.acceptor = std::make_unique<tcp::acceptor>(state.io_context, tcp::endpoint(tcp::v4(), port));
      write_port_file(port);
      std::atexit(remove_port_file);

      legacy_accept_connection();
      state.io_context.run();
      return runtime::jank_nil;
    }

    object_ref start_server(object_ref const port_obj, object_ref const bind_obj)
    {
      bootstrap_runtime_once();

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
        return runtime::jank_nil;
      }
    }

    object_ref stop_server(object_ref const server_obj)
    {
      if(server_obj == runtime::jank_nil)
      {
        return runtime::jank_nil;
      }

      auto const server_map(expect_object<obj::persistent_hash_map>(server_obj));
      auto const srv_ptr_kw(__rt_ctx->intern_keyword("server-ptr").expect_ok());
      auto const ptr_obj(runtime::get(server_map, srv_ptr_kw));

      if(ptr_obj == runtime::jank_nil)
      {
        return runtime::jank_nil;
      }

      auto const ptr_val(static_cast<std::uintptr_t>(to_int(ptr_obj)));
      server_registry().erase(ptr_val);

      return runtime::jank_nil;
    }

    object_ref get_server_port(object_ref const server_obj)
    {
      if(server_obj == runtime::jank_nil)
      {
        return runtime::jank_nil;
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
      return runtime::jank_nil;
    }

    object_ref this_object_ref() override
    {
      return runtime::jank_nil;
    }
  };
}

extern "C" jank_object_ref jank_load_jank_nrepl_server_asio()
{
  jank::nrepl_server::asio::__ns loader;
  return loader.call().erase();
}
