#pragma once

// iOS Compile Client
//
// Connects to macOS compilation server to request cross-compiled object files.
// Used by iOS eval_server to delegate compilation to macOS, avoiding symbol
// duplication issues from CppInterOp parsing C++ headers on iOS.
//
// Protocol: JSON over TCP (newline-delimited) - see protocol.hpp
//
// Usage:
//   compile_client client("192.168.1.100", 5559);
//   if(client.connect()) {
//     auto result = client.compile("(defn foo [] 42)", "user");
//     if(result.success) {
//       // Load result.object_data into ORC JIT
//     }
//   }

#include <string>
#include <vector>
#include <cstdint>
#include <iostream>
#include <atomic>
#include <cstring>

// BSD sockets
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <fcntl.h>

#include <jank/compile_server/protocol.hpp>

namespace jank::compile_server
{
  class client
  {
  public:
    client(std::string const &host = "127.0.0.1", uint16_t port = default_compile_port)
      : host_(host)
      , port_(port)
      , socket_fd_(-1)
      , next_id_(1)
    {
    }

    ~client()
    {
      disconnect();
    }

    bool connect()
    {
      if(socket_fd_ >= 0)
      {
        return true;  // Already connected
      }

      // Resolve hostname
      struct hostent *server = gethostbyname(host_.c_str());
      if(!server)
      {
        std::cerr << "[compile-client] Failed to resolve host: " << host_ << std::endl;
        return false;
      }

      // Create socket
      socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
      if(socket_fd_ < 0)
      {
        std::cerr << "[compile-client] Failed to create socket: " << strerror(errno) << std::endl;
        return false;
      }

      // Set socket timeout for connect
      struct timeval timeout;
      timeout.tv_sec = 5;  // 5 second connect timeout
      timeout.tv_usec = 0;
      setsockopt(socket_fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
      setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

      // Connect
      struct sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(port_);
      std::memcpy(&addr.sin_addr.s_addr, server->h_addr, server->h_length);

      if(::connect(socket_fd_, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0)
      {
        std::cerr << "[compile-client] Failed to connect to " << host_ << ":" << port_
                  << " - " << strerror(errno) << std::endl;
        ::close(socket_fd_);
        socket_fd_ = -1;
        return false;
      }

      // Set longer timeout for compilation - needs to be long enough for compiling
      // many transitive dependencies (15+ modules can take several minutes)
      timeout.tv_sec = 300;  // 5 minute compilation timeout
      setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

      std::cout << "[compile-client] Connected to " << host_ << ":" << port_ << std::endl;
      return true;
    }

    void disconnect()
    {
      if(socket_fd_ >= 0)
      {
        ::close(socket_fd_);
        socket_fd_ = -1;
      }
    }

    bool is_connected() const
    {
      return socket_fd_ >= 0;
    }

    // Compile jank code to ARM64 object file
    compile_response compile(std::string const &code, std::string const &ns = "user",
                             std::string const &module = "")
    {
      compile_response response;

      if(!connect())
      {
        response.success = false;
        response.error = "Not connected to compile server";
        response.error_type = "connection";
        return response;
      }

      // Build request
      int64_t const id = next_id_++;
      std::string request = R"({"op":"compile","id":)" + std::to_string(id)
        + R"(,"code":")" + escape_json(code)
        + R"(","ns":")" + escape_json(ns)
        + R"(","module":")" + escape_json(module) + R"("})" + "\n";

      // Send request
      if(send_all(request) < 0)
      {
        response.success = false;
        response.error = "Failed to send compile request";
        response.error_type = "connection";
        disconnect();
        return response;
      }

      // Receive response (single line)
      std::string line = recv_line();
      if(line.empty())
      {
        response.success = false;
        response.error = "No response from compile server";
        response.error_type = "connection";
        disconnect();
        return response;
      }

      // Parse response
      auto op = get_json_string(line, "op");
      auto resp_id = get_json_int(line, "id");

      if(resp_id != id)
      {
        response.success = false;
        response.error = "Response ID mismatch";
        response.error_type = "protocol";
        return response;
      }

      if(op == "compiled")
      {
        response.id = resp_id;
        response.success = true;
        response.entry_symbol = get_json_string(line, "symbol");
        auto encoded = get_json_string(line, "object");
        response.object_data = base64_decode(encoded);

        std::cout << "[compile-client] Compiled successfully, object size: "
                  << response.object_data.size() << " bytes" << std::endl;
      }
      else if(op == "error")
      {
        response.id = resp_id;
        response.success = false;
        response.error = get_json_string(line, "error");
        response.error_type = get_json_string(line, "type");

        std::cerr << "[compile-client] Compile error: " << response.error << std::endl;
      }
      else
      {
        response.success = false;
        response.error = "Unknown response op: " + op;
        response.error_type = "protocol";
      }

      return response;
    }

    // Ping the server
    bool ping()
    {
      if(!connect())
      {
        return false;
      }

      int64_t const id = next_id_++;
      std::string request = R"({"op":"ping","id":)" + std::to_string(id) + "}\n";

      if(send_all(request) < 0)
      {
        disconnect();
        return false;
      }

      std::string line = recv_line();
      auto op = get_json_string(line, "op");
      return op == "pong";
    }

    // Request native C++ source for a form (for jank.compiler-native/native-source)
    native_source_response native_source(std::string const &code, std::string const &ns = "user")
    {
      native_source_response response;

      if(!connect())
      {
        response.success = false;
        response.error = "Not connected to compile server";
        return response;
      }

      // Build request
      int64_t const id = next_id_++;
      std::string request = R"({"op":"native-source","id":)" + std::to_string(id)
        + R"(,"code":")" + escape_json(code)
        + R"(","ns":")" + escape_json(ns) + R"("})" + "\n";

      // Send request
      if(send_all(request) < 0)
      {
        response.success = false;
        response.error = "Failed to send native-source request";
        disconnect();
        return response;
      }

      // Receive response (single line)
      std::string line = recv_line();
      if(line.empty())
      {
        response.success = false;
        response.error = "No response from compile server";
        disconnect();
        return response;
      }

      // Parse response
      auto op = get_json_string(line, "op");
      auto resp_id = get_json_int(line, "id");

      if(resp_id != id)
      {
        response.success = false;
        response.error = "Response ID mismatch";
        return response;
      }

      if(op == "native-source-result")
      {
        response.id = resp_id;
        response.success = true;
        response.source = get_json_string(line, "source");
      }
      else if(op == "error")
      {
        response.id = resp_id;
        response.success = false;
        response.error = get_json_string(line, "error");

        std::cerr << "[compile-client] Native-source error: " << response.error << std::endl;
      }
      else
      {
        response.success = false;
        response.error = "Unknown response op: " + op;
      }

      return response;
    }

    // Require (load) a namespace - send source to compile server
    require_response require_ns(std::string const &ns, std::string const &source)
    {
      require_response response;

      if(!connect())
      {
        response.success = false;
        response.error = "Not connected to compile server";
        response.error_type = "connection";
        return response;
      }

      // Build request
      int64_t const id = next_id_++;
      std::string request = R"({"op":"require","id":)" + std::to_string(id)
        + R"(,"ns":")" + escape_json(ns)
        + R"(","source":")" + escape_json(source) + R"("})" + "\n";

      // Send request
      if(send_all(request) < 0)
      {
        response.success = false;
        response.error = "Failed to send require request";
        response.error_type = "connection";
        disconnect();
        return response;
      }

      // Receive response (single line)
      std::string line = recv_line();
      if(line.empty())
      {
        response.success = false;
        response.error = "No response from compile server";
        response.error_type = "connection";
        disconnect();
        return response;
      }

      // Parse response
      auto op = get_json_string(line, "op");
      auto resp_id = get_json_int(line, "id");

      if(resp_id != id)
      {
        response.success = false;
        response.error = "Response ID mismatch";
        response.error_type = "protocol";
        return response;
      }

      if(op == "required")
      {
        response.id = resp_id;
        response.success = true;

        // Parse modules array - simple parsing for single module
        // Format: "modules":[{"name":"...","symbol":"...","object":"..."}]
        auto modules_start = line.find("\"modules\"");
        if(modules_start != std::string::npos)
        {
          // Find array content
          auto array_start = line.find('[', modules_start);
          auto array_end = line.rfind(']');
          if(array_start != std::string::npos && array_end != std::string::npos && array_end > array_start)
          {
            std::string array_content = line.substr(array_start + 1, array_end - array_start - 1);

            // Simple parsing: look for each object in the array
            size_t pos = 0;
            while(pos < array_content.size())
            {
              auto obj_start = array_content.find('{', pos);
              if(obj_start == std::string::npos) break;

              auto obj_end = array_content.find('}', obj_start);
              if(obj_end == std::string::npos) break;

              std::string obj_str = array_content.substr(obj_start, obj_end - obj_start + 1);

              compiled_module mod;
              mod.name = get_json_string(obj_str, "name");
              mod.entry_symbol = get_json_string(obj_str, "symbol");
              auto encoded = get_json_string(obj_str, "object");
              mod.object_data = base64_decode(encoded);

              if(!mod.name.empty())
              {
                response.modules.push_back(std::move(mod));
              }

              pos = obj_end + 1;
            }
          }
        }

        std::cout << "[compile-client] Required namespace successfully, "
                  << response.modules.size() << " module(s)" << std::endl;
      }
      else if(op == "error")
      {
        response.id = resp_id;
        response.success = false;
        response.error = get_json_string(line, "error");
        response.error_type = get_json_string(line, "type");

        std::cerr << "[compile-client] Require error: " << response.error << std::endl;
      }
      else
      {
        response.success = false;
        response.error = "Unknown response op: " + op;
        response.error_type = "protocol";
      }

      return response;
    }

  private:
    ssize_t send_all(std::string const &data)
    {
      size_t sent = 0;
      while(sent < data.size())
      {
        ssize_t n = send(socket_fd_, data.c_str() + sent, data.size() - sent, 0);
        if(n <= 0)
        {
          return -1;
        }
        sent += n;
      }
      return sent;
    }

    std::string recv_line()
    {
      std::string result;
      char c;
      while(true)
      {
        ssize_t n = recv(socket_fd_, &c, 1, 0);
        if(n <= 0)
        {
          break;
        }
        if(c == '\n')
        {
          break;
        }
        if(c != '\r')
        {
          result += c;
        }
      }
      return result;
    }

    static std::string escape_json(std::string const &s)
    {
      std::string result;
      result.reserve(s.size() + 16);
      for(char c : s)
      {
        switch(c)
        {
          case '"': result += "\\\""; break;
          case '\\': result += "\\\\"; break;
          case '\n': result += "\\n"; break;
          case '\r': result += "\\r"; break;
          case '\t': result += "\\t"; break;
          default: result += c;
        }
      }
      return result;
    }

    static std::string get_json_string(std::string const &json, std::string const &key)
    {
      // Search for the key as a JSON key (followed by colon), not as a value
      std::string const search_key = "\"" + key + "\"";
      size_t key_pos = 0;
      while(true)
      {
        key_pos = json.find(search_key, key_pos);
        if(key_pos == std::string::npos) return "";

        // Check if this is followed by a colon (possibly with whitespace)
        size_t after_key = key_pos + search_key.size();
        while(after_key < json.size() && (json[after_key] == ' ' || json[after_key] == '\t'))
          after_key++;

        if(after_key < json.size() && json[after_key] == ':')
        {
          // Found the key, not a value
          break;
        }
        // This was a value, keep searching
        key_pos++;
      }

      auto colon_pos = json.find(':', key_pos);
      if(colon_pos == std::string::npos) return "";
      auto quote_start = json.find('"', colon_pos);
      if(quote_start == std::string::npos) return "";
      auto quote_end = quote_start + 1;
      while(quote_end < json.size())
      {
        if(json[quote_end] == '"' && (quote_end == 0 || json[quote_end - 1] != '\\'))
          break;
        quote_end++;
      }
      std::string result;
      for(size_t i = quote_start + 1; i < quote_end; i++)
      {
        if(json[i] == '\\' && i + 1 < quote_end)
        {
          switch(json[i + 1])
          {
            case 'n': result += '\n'; i++; break;
            case 'r': result += '\r'; i++; break;
            case 't': result += '\t'; i++; break;
            case '"': result += '"'; i++; break;
            case '\\': result += '\\'; i++; break;
            default: result += json[i];
          }
        }
        else
        {
          result += json[i];
        }
      }
      return result;
    }

    static int64_t get_json_int(std::string const &json, std::string const &key)
    {
      // Search for the key as a JSON key (followed by colon), not as a value
      std::string const search_key = "\"" + key + "\"";
      size_t key_pos = 0;
      while(true)
      {
        key_pos = json.find(search_key, key_pos);
        if(key_pos == std::string::npos) return 0;

        // Check if this is followed by a colon (possibly with whitespace)
        size_t after_key = key_pos + search_key.size();
        while(after_key < json.size() && (json[after_key] == ' ' || json[after_key] == '\t'))
          after_key++;

        if(after_key < json.size() && json[after_key] == ':')
        {
          // Found the key, not a value
          break;
        }
        // This was a value, keep searching
        key_pos++;
      }
      auto colon_pos = json.find(':', key_pos);
      if(colon_pos == std::string::npos) return 0;
      auto num_start = colon_pos + 1;
      while(num_start < json.size() && (json[num_start] == ' ' || json[num_start] == '\t'))
        num_start++;
      try { return std::stoll(json.substr(num_start)); }
      catch(...) { return 0; }
    }

    static std::vector<uint8_t> base64_decode(std::string const &encoded)
    {
      static constexpr int8_t table[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
      };

      std::vector<uint8_t> result;
      result.reserve((encoded.size() / 4) * 3);

      for(size_t i = 0; i < encoded.size(); i += 4)
      {
        if(i + 3 >= encoded.size()) break;

        int8_t a = table[static_cast<uint8_t>(encoded[i])];
        int8_t b = table[static_cast<uint8_t>(encoded[i + 1])];
        int8_t c = table[static_cast<uint8_t>(encoded[i + 2])];
        int8_t d = table[static_cast<uint8_t>(encoded[i + 3])];

        if(a < 0 || b < 0) break;

        result.push_back((a << 2) | (b >> 4));
        if(c >= 0)
        {
          result.push_back(((b & 0x0F) << 4) | (c >> 2));
          if(d >= 0)
          {
            result.push_back(((c & 0x03) << 6) | d);
          }
        }
      }

      return result;
    }

    std::string host_;
    uint16_t port_;
    int socket_fd_;
    std::atomic<int64_t> next_id_;
  };

} // namespace jank::compile_server
