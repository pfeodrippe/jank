#include <array>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#define JANK_NREPL_REDEFINE_ACCESS 1
/* Boost.Asio injects several inline definitions for private nested helpers.        */
/* Clang strictly enforces access there when compiling generated snippets, so we   */
/* temporarily relax the access keywords during the includes.                      */
#define private public
#define protected public
#include <boost/asio/ts/buffer.hpp>
#include <boost/asio/ts/internet.hpp>
#include <boost/asio/write.hpp>
#undef private
#undef protected
#undef JANK_NREPL_REDEFINE_ACCESS

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
      socket_.async_read_some(boost::asio::buffer(read_buffer_),
                              [this, self](boost::system::error_code ec, std::size_t length) {
                                on_read(ec, length);
                              });
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
          boost::system::error_code ignored;
          socket_.close(ignored);
          return;
        }

        if(!decoded.data.is_dict())
        {
          std::cerr << "invalid nREPL payload" << '\n';
          boost::system::error_code ignored;
          socket_.close(ignored);
          return;
        }

        message msg{ decoded.data.as_dict() };
        auto responses(engine_.handle(msg));
        for(auto &payload : responses)
        {
          std::string encoded;
          encoded.reserve(256);
          bencode::value wrapper{ payload };
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
      boost::asio::async_write(socket_,
                               boost::asio::buffer(front),
                               [this, self](boost::system::error_code ec, std::size_t) {
                                 on_write(ec);
                               });
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

  boost::asio::io_context io_context;
  std::unique_ptr<tcp::acceptor> acceptor;
  std::unique_ptr<tcp::socket> next_socket;
  std::shared_ptr<engine> server_engine;
  std::filesystem::path const port_file{ ".nrepl-port" };

  void accept_connection()
  {
    next_socket = std::make_unique<tcp::socket>(io_context);
    acceptor->async_accept(*next_socket, [](boost::system::error_code const ec) {
      if(!ec && server_engine)
      {
        auto conn(std::make_shared<connection>(std::move(*next_socket), *server_engine));
        conn->start();
      }
      else if(ec)
      {
        std::cerr << "accept error: " << ec.message() << '\n';
      }

      accept_connection();
    });
  }

  void write_port_file(int const port)
  {
    std::ofstream ofs{ port_file };
    if(ofs)
    {
      ofs << port;
    }
  }

  void remove_port_file()
  {
    std::error_code ec;
    std::filesystem::remove(port_file, ec);
  }

  object_ref run_server(object_ref const port_obj)
  {
    bootstrap_runtime_once();

    auto const port(to_int(port_obj));
    std::cout << "Starting jank nREPL on port " << port << '\n';

    server_engine = std::make_shared<engine>();
    acceptor = std::make_unique<tcp::acceptor>(io_context, tcp::endpoint(tcp::v4(), port));
    write_port_file(port);
    std::atexit(remove_port_file);

    accept_connection();
    io_context.run();
    return runtime::jank_nil;
  }

  struct __ns : behavior::callable
  {
    object_ref call() override
    {
      auto const ns(__rt_ctx->intern_ns("jank.nrepl-server.asio"));
      ns->intern_var("run!")->bind_root(make_box<obj::native_function_wrapper>(&run_server));
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
