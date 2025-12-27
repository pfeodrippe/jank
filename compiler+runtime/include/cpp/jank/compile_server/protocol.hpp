#pragma once

// Compile Server Protocol
//
// Protocol for communication between iOS (client) and macOS (compilation server).
// iOS sends jank code, macOS compiles to ARM64 object file and returns it.
//
// JSON Protocol (newline-delimited):
//
// === Single Expression Compilation (for REPL eval) ===
//
// Compile Request:
//   {"op":"compile","id":1,"code":"(defn foo [] 42)","ns":"user","module":"user$foo"}
//
// Compile Response (success):
//   {"op":"compiled","id":1,"symbol":"_user_SLASH_foo_0","object":"<base64-encoded .o file>"}
//
// Compile Response (error):
//   {"op":"error","id":1,"error":"Syntax error at line 1","type":"compile"}
//
// === Namespace Loading (for require) ===
//
// Require Request (iOS sends namespace source):
//   {"op":"require","id":1,"ns":"vybe.sdf.ui","source":"(ns vybe.sdf.ui ...)..."}
//
// Require Response (success - may contain multiple modules):
//   {"op":"required","id":1,"modules":[{"name":"vybe.sdf.ui$loading__","symbol":"...","object":"..."}]}
//
// Need Source Request (server needs a dependency):
//   {"op":"need-source","id":1,"ns":"vybe.sdf.shader"}
//
// Source Response (client provides requested source):
//   {"op":"source","id":1,"ns":"vybe.sdf.shader","source":"..."}
//
// === Utility ===
//
// Ping/Pong (for keepalive):
//   {"op":"ping","id":1}
//   {"op":"pong","id":1}

#include <string>
#include <vector>
#include <cstdint>

namespace jank::compile_server
{
  // Compile request from iOS to macOS
  struct compile_request
  {
    int64_t id{ 0 };
    std::string code;         // jank source code
    std::string ns;           // namespace context
    std::string module;       // module name for generated code
  };

  // Compile response from macOS to iOS
  struct compile_response
  {
    int64_t id{ 0 };
    bool success{ false };

    // On success:
    std::vector<uint8_t> object_data;  // ARM64 object file bytes
    std::string entry_symbol;           // Symbol to call after loading

    // On error:
    std::string error;
    std::string error_type;  // "compile", "codegen", "cross-compile"
  };

  // Require request from iOS to macOS (load a namespace)
  struct require_request
  {
    int64_t id{ 0 };
    std::string ns;           // namespace to load (e.g., "vybe.sdf.ui")
    std::string source;       // full source code of the namespace
  };

  // A compiled module (part of require response)
  struct compiled_module
  {
    std::string name;                   // module name (e.g., "vybe.sdf.ui$loading__")
    std::string entry_symbol;           // symbol to call after loading
    std::vector<uint8_t> object_data;   // ARM64 object file bytes
  };

  // Require response from macOS to iOS
  struct require_response
  {
    int64_t id{ 0 };
    bool success{ false };

    // On success:
    std::vector<compiled_module> modules;  // compiled modules for this namespace

    // On error:
    std::string error;
    std::string error_type;
  };

  // Need source request from macOS to iOS (server needs dependency source)
  struct need_source_request
  {
    int64_t id{ 0 };
    std::string ns;  // namespace needed (e.g., "vybe.sdf.shader")
  };

  // Source response from iOS to macOS
  struct source_response
  {
    int64_t id{ 0 };
    std::string ns;
    std::string source;
    bool found{ true };  // false if source not found
  };

  // Native-source request - generate C++ source code for a form
  struct native_source_response
  {
    int64_t id{ 0 };
    bool success{ false };

    // On success:
    std::string source;  // Generated C++ source code

    // On error:
    std::string error;
  };

  // Default port for compilation service
  constexpr uint16_t default_compile_port = 5570;

} // namespace jank::compile_server
