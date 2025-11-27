// Example: nREPL WebSocket Server with Hot-Reload Support
// This implements Steps 3 & 4: WebSocket bridge + server compilation
//
// Location in jank: src/cpp/jank/nrepl/hot_reload_server.cpp
//
// Dependencies: websocketpp or uWebSockets
// Build: g++ -std=c++20 example_nrepl_server.cpp -lwebsocketpp -lpthread

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <cstdlib>

using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;
using websocketpp::lib::bind;

typedef websocketpp::server<websocketpp::config::asio> server;

class HotReloadServer {
public:
  HotReloadServer() {
    // Set up server
    m_server.set_message_handler(bind(&HotReloadServer::on_message, this, ::_1, ::_2));
    m_server.set_open_handler(bind(&HotReloadServer::on_open, this, ::_1));
    m_server.set_close_handler(bind(&HotReloadServer::on_close, this, ::_1));

    m_server.init_asio();
  }

  void run(uint16_t port) {
    m_server.listen(port);
    m_server.start_accept();

    std::cout << "[jank-nrepl] Hot-reload server listening on ws://localhost:" << port << "/repl\n";
    m_server.run();
  }

private:
  server m_server;

  void on_open(websocketpp::connection_hdl hdl) {
    std::cout << "[jank-nrepl] Client connected\n";
  }

  void on_close(websocketpp::connection_hdl hdl) {
    std::cout << "[jank-nrepl] Client disconnected\n";
  }

  void on_message(websocketpp::connection_hdl hdl, server::message_ptr msg) {
    std::string payload = msg->get_payload();
    std::cout << "[jank-nrepl] Received: " << payload << "\n";

    // Parse JSON (simplified - use a real JSON library in production)
    if (payload.find("\"type\":\"eval\"") != std::string::npos) {
      // Extract code (very naive parsing - use nlohmann/json in real code)
      size_t code_start = payload.find("\"code\":\"") + 8;
      size_t code_end = payload.find("\"", code_start);
      std::string code = payload.substr(code_start, code_end - code_start);

      std::cout << "[jank-nrepl] Eval request: " << code << "\n";

      // Compile and send patch
      std::vector<uint8_t> patch = compile_to_wasm_patch(code);

      if (!patch.empty()) {
        send_patch(hdl, patch);
      } else {
        send_error(hdl, "Compilation failed");
      }
    }
  }

  // Step 4: Server-side compilation
  std::vector<uint8_t> compile_to_wasm_patch(const std::string& code) {
    std::cout << "[jank-nrepl] Compiling: " << code << "\n";

    // STEP 1: Parse jank code
    // (This would call jank's parser - simplified here)
    // auto expr = jank::read::parse(code);

    // STEP 2: Compile to C++
    // (This would call jank's codegen - simplified here)
    std::string cpp_code = generate_cpp_from_jank(code);

    // STEP 3: Write to temp file
    std::string temp_cpp = "/tmp/patch_" + generate_uuid() + ".cpp";
    std::ofstream cpp_file(temp_cpp);
    cpp_file << cpp_code;
    cpp_file.close();

    // STEP 4: Compile with emcc
    std::string temp_wasm = "/tmp/patch_" + generate_uuid() + ".wasm";
    std::string cmd = "source ~/emsdk/emsdk_env.sh && emcc " + temp_cpp +
                      " -o " + temp_wasm +
                      " -sSIDE_MODULE=1 -O2 -fPIC 2>/dev/null";

    std::cout << "[jank-nrepl] Running: " << cmd << "\n";

    int result = system(cmd.c_str());
    if (result != 0) {
      std::cerr << "[jank-nrepl] Compilation failed!\n";
      return {};
    }

    // STEP 5: Read binary
    std::ifstream wasm_file(temp_wasm, std::ios::binary);
    std::vector<uint8_t> wasm_data(
      (std::istreambuf_iterator<char>(wasm_file)),
      std::istreambuf_iterator<char>()
    );

    std::cout << "[jank-nrepl] Compiled patch: " << wasm_data.size() << " bytes\n";

    // Clean up temp files
    std::remove(temp_cpp.c_str());
    std::remove(temp_wasm.c_str());

    return wasm_data;
  }

  void send_patch(websocketpp::connection_hdl hdl, const std::vector<uint8_t>& patch) {
    // Convert to base64
    std::string base64 = base64_encode(patch.data(), patch.size());

    // Build JSON response (simplified - use a real JSON library)
    std::string response = "{\"type\":\"patch\",\"data\":\"" + base64 + "\"}";

    m_server.send(hdl, response, websocketpp::frame::opcode::text);
    std::cout << "[jank-nrepl] Sent patch (" << patch.size() << " bytes)\n";
  }

  void send_error(websocketpp::connection_hdl hdl, const std::string& error) {
    std::string response = "{\"type\":\"error\",\"error\":\"" + error + "\"}";
    m_server.send(hdl, response, websocketpp::frame::opcode::text);
  }

  // Helper: Generate C++ from jank code
  std::string generate_cpp_from_jank(const std::string& code) {
    // This is a simplified example - real implementation would use jank's codegen
    //
    // For example, if code is: (defn ggg [v] (+ v 49))
    // Generate:
    std::stringstream cpp;

    cpp << "// Auto-generated from jank code: " << code << "\n";
    cpp << "extern \"C\" {\n";
    cpp << "__attribute__((visibility(\"default\")))\n";
    cpp << "int jank_user_ggg(int v) {\n";
    cpp << "    return v + 49;  // Compiled from jank\n";
    cpp << "}\n";
    cpp << "\n";
    cpp << "// Patch metadata for var registry\n";
    cpp << "__attribute__((visibility(\"default\")))\n";
    cpp << "struct patch_symbol {\n";
    cpp << "    const char* name;\n";
    cpp << "    const char* signature;\n";
    cpp << "    void* fn_ptr;\n";
    cpp << "};\n";
    cpp << "\n";
    cpp << "__attribute__((visibility(\"default\")))\n";
    cpp << "patch_symbol* jank_patch_symbols(int* count) {\n";
    cpp << "    static patch_symbol symbols[] = {\n";
    cpp << "        {\"user/ggg\", \"ii\", (void*)jank_user_ggg}\n";
    cpp << "    };\n";
    cpp << "    *count = 1;\n";
    cpp << "    return symbols;\n";
    cpp << "}\n";
    cpp << "}\n";

    return cpp.str();
  }

  // Helpers
  std::string generate_uuid() {
    return std::to_string(std::rand());
  }

  std::string base64_encode(const uint8_t* data, size_t len) {
    // Simplified base64 encoding - use a real library in production
    static const char* base64_chars =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string ret;
    int i = 0;
    uint8_t char_array_3[3];
    uint8_t char_array_4[4];

    while (len--) {
      char_array_3[i++] = *(data++);
      if (i == 3) {
        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        char_array_4[3] = char_array_3[2] & 0x3f;

        for (i = 0; i < 4; i++)
          ret += base64_chars[char_array_4[i]];
        i = 0;
      }
    }

    if (i) {
      for (int j = i; j < 3; j++)
        char_array_3[j] = '\0';

      char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
      char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
      char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);

      for (int j = 0; j < i + 1; j++)
        ret += base64_chars[char_array_4[j]];

      while (i++ < 3)
        ret += '=';
    }

    return ret;
  }
};

// Main
int main() {
  HotReloadServer server;
  server.run(7888);  // Default hot-reload port
  return 0;
}

// INTEGRATION into jank nREPL:
//
// The existing jank nREPL server would start this WebSocket server
// as a background thread when --hot-reload flag is passed:
//
//   jank nrepl --hot-reload
//
// Then it listens for eval requests and compiles patches in real-time.
