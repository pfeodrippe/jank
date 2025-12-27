// iOS Eval CLI - Simple command-line client for testing iOS eval server
//
// Usage:
//   ./ios-eval-cli <ios-ip> [port]
//   ./ios-eval-cli 192.168.1.100
//   ./ios-eval-cli 192.168.1.100 5558
//
// Then type jank expressions and press Enter to evaluate.
// Type 'quit' or Ctrl-D to exit.

#include <iostream>
#include <string>
#include <boost/asio.hpp>

using boost::asio::ip::tcp;

std::string escape_json(std::string const &s)
{
  std::string result;
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

std::string get_json_string(std::string const &json, std::string const &key)
{
  auto key_pos = json.find("\"" + key + "\"");
  if(key_pos == std::string::npos)
    return "";
  auto colon_pos = json.find(':', key_pos);
  if(colon_pos == std::string::npos)
    return "";
  auto quote_start = json.find('"', colon_pos);
  if(quote_start == std::string::npos)
    return "";
  auto quote_end = quote_start + 1;
  while(quote_end < json.size())
  {
    if(json[quote_end] == '"' && json[quote_end - 1] != '\\')
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
        case 'n':
          result += '\n';
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

int main(int argc, char *argv[])
{
  if(argc < 2)
  {
    std::cerr << "Usage: " << argv[0] << " <ios-ip> [port]" << std::endl;
    std::cerr << "Example: " << argv[0] << " 192.168.1.100" << std::endl;
    return 1;
  }

  std::string host = argv[1];
  uint16_t port = argc > 2 ? std::stoi(argv[2]) : 5558;

  std::cout << "Connecting to " << host << ":" << port << "..." << std::endl;

  try
  {
    boost::asio::io_context io;
    tcp::socket socket(io);
    tcp::resolver resolver(io);

    auto endpoints = resolver.resolve(host, std::to_string(port));
    boost::asio::connect(socket, endpoints);

    std::cout << "Connected!" << std::endl;

    // Read welcome message
    boost::asio::streambuf buf;
    boost::asio::read_until(socket, buf, '\n');
    std::istream is(&buf);
    std::string welcome;
    std::getline(is, welcome);
    std::cout << "Server: " << welcome << std::endl;

    std::cout << std::endl;
    std::cout << "jank iOS REPL (type 'quit' to exit)" << std::endl;
    std::cout << "====================================" << std::endl;

    int request_id = 0;
    std::string line;

    while(true)
    {
      std::cout << "ios> ";
      std::cout.flush();

      if(!std::getline(std::cin, line))
      {
        break; // EOF
      }

      if(line.empty())
      {
        continue;
      }

      if(line == "quit" || line == "exit")
      {
        break;
      }

      // Build request
      std::string request = R"({"op":"eval","id":)" + std::to_string(++request_id)
        + R"(,"code":")" + escape_json(line) + R"(","ns":"user"})" + "\n";

      // Send
      boost::asio::write(socket, boost::asio::buffer(request));

      // Read response
      buf.consume(buf.size());
      boost::asio::read_until(socket, buf, '\n');
      std::istream response_stream(&buf);
      std::string response;
      std::getline(response_stream, response);

      // Parse and display
      auto op = get_json_string(response, "op");
      if(op == "result")
      {
        std::cout << "=> " << get_json_string(response, "value") << std::endl;
      }
      else if(op == "error")
      {
        std::cerr << "Error: " << get_json_string(response, "error") << std::endl;
      }
      else
      {
        std::cout << "Response: " << response << std::endl;
      }
    }

    std::cout << "Goodbye!" << std::endl;
    socket.close();
  }
  catch(std::exception const &e)
  {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}
