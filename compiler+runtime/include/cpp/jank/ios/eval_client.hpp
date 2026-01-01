#pragma once

// iOS Remote Eval Client (macOS side)
//
// Connects to the iOS eval server and forwards code for evaluation.
// Used by the jank nREPL server to implement remote eval targets.
//
// Like Piggieback for ClojureScript, this allows full IDE features
// (completion, lookup) on macOS while eval happens on iOS.

#include <string>
#include <optional>
#include <chrono>

#ifndef __EMSCRIPTEN__

  #include <boost/asio.hpp>
  #include <sys/socket.h>

namespace jank::ios
{
  using boost::asio::ip::tcp;

  struct eval_result
  {
    bool success{ false };
    std::string value;
    std::string error;
    std::string error_type; // "compile", "runtime", "crash", "timeout", "connection"
  };

  class eval_client
  {
  public:
    eval_client() = default;

    // Connect to iOS device
    bool connect(std::string const &host, uint16_t port = 5558)
    {
      try
      {
        socket_ = std::make_unique<tcp::socket>(io_context_);
        tcp::resolver resolver(io_context_);
        auto endpoints = resolver.resolve(host, std::to_string(port));
        boost::asio::connect(*socket_, endpoints);

        host_ = host;
        port_ = port;

        // Read welcome message fully
        boost::asio::streambuf buf;
        boost::asio::read_until(*socket_, buf, '\n');

        // Consume the welcome message from buffer
        std::string welcome;
        std::istream is(&buf);
        std::getline(is, welcome);
        std::cout << "[ios-client] Connected, welcome: " << welcome << std::endl;

        // Small delay to ensure connection is stable
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        connected_ = true;
        return true;
      }
      catch(std::exception const &e)
      {
        last_error_ = e.what();
        return false;
      }
    }

    void disconnect()
    {
      if(socket_ && socket_->is_open())
      {
        boost::system::error_code ec;
        socket_->shutdown(tcp::socket::shutdown_both, ec);
        socket_->close(ec);
      }
      socket_.reset();
      connected_ = false;
    }

    bool is_connected() const
    {
      return connected_ && socket_ && socket_->is_open();
    }

    std::string const &host() const
    {
      return host_;
    }

    uint16_t port() const
    {
      return port_;
    }

    std::string const &last_error() const
    {
      return last_error_;
    }

    // Evaluate code on iOS device
    eval_result
    eval(std::string const &code, std::string const &ns = "user", int timeout_ms = 30000)
    {
      if(!is_connected())
      {
        return { false, "", "Not connected to iOS device", "connection" };
      }

      try
      {
        // Build request
        std::string request = R"({"op":"eval","id":)" + std::to_string(++request_id_)
          + R"(,"code":")" + escape_json(code) + R"(","ns":")" + escape_json(ns) + "\"}\n";

        std::cout << "[ios-client] Sending eval request id=" << request_id_ << std::endl;

        // Set socket timeout
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        setsockopt(socket_->native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // Send request
        boost::asio::write(*socket_, boost::asio::buffer(request));

        // Read response (blocking with timeout set above)
        boost::asio::streambuf buf;
        boost::system::error_code ec;
        (void)boost::asio::read_until(*socket_, buf, '\n', ec);

        if(ec)
        {
          std::cerr << "[ios-client] Read error: " << ec.message() << std::endl;
          if(ec == boost::asio::error::operation_aborted || ec == boost::asio::error::timed_out)
          {
            return { false, "", "Timeout waiting for response", "timeout" };
          }
          connected_ = false;
          return { false, "", "Read error: " + ec.message(), "connection" };
        }

        std::string response;
        std::istream is(&buf);
        std::getline(is, response);

        std::cout << "[ios-client] Got response: " << response.substr(0, 100)
                  << (response.size() > 100 ? "..." : "") << std::endl;

        if(response.empty())
        {
          return { false, "", "Empty response from iOS", "connection" };
        }

        // Parse response
        return parse_response(response);
      }
      catch(std::exception const &e)
      {
        last_error_ = e.what();
        connected_ = false;
        return { false, "", std::string("Connection error: ") + e.what(), "connection" };
      }
    }

    // Ping the server
    bool ping()
    {
      if(!is_connected())
      {
        return false;
      }

      try
      {
        std::string request = R"({"op":"ping","id":)" + std::to_string(++request_id_) + "}\n";
        boost::asio::write(*socket_, boost::asio::buffer(request));

        boost::asio::streambuf buf;
        boost::asio::read_until(*socket_, buf, '\n');

        return true;
      }
      catch(...)
      {
        connected_ = false;
        return false;
      }
    }

  private:
    static std::string escape_json(std::string const &s)
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

    static std::string get_json_string(std::string const &json, std::string const &key)
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
      // Unescape
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

    eval_result parse_response(std::string const &json)
    {
      auto op = get_json_string(json, "op");

      if(op == "result")
      {
        return { true, get_json_string(json, "value"), "", "" };
      }
      else if(op == "error")
      {
        return { false, "", get_json_string(json, "error"), get_json_string(json, "type") };
      }
      else
      {
        return { false, "", "Unknown response op: " + op, "protocol" };
      }
    }

    boost::asio::io_context io_context_;
    std::unique_ptr<tcp::socket> socket_;
    std::string host_;
    uint16_t port_{ 0 };
    bool connected_{ false };
    std::string last_error_;
    int64_t request_id_{ 0 };
  };

  // Global client instance
  inline eval_client &get_eval_client()
  {
    static eval_client client;
    return client;
  }

} // namespace jank::ios

#endif // !__EMSCRIPTEN__
