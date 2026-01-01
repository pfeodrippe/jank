// macOS Compile Server for iOS Remote JIT
//
// This server receives jank code from iOS, compiles it to ARM64 object files,
// and sends them back. Run this on macOS while developing iOS apps.
//
// Usage:
//   ./compile-server [--port PORT] [--target sim|device]
//
// The server will listen on the specified port (default: 5559) and cross-compile
// incoming jank code to ARM64 object files for the specified iOS target.

#include <iostream>
#include <string>
#include <vector>
#include <csignal>
#include <atomic>

#include <jank/c_api.h>
#include <jank/compile_server/server.hpp>
#include <jank/runtime/context.hpp>
#include <jank/util/cli.hpp>
#include <jank/util/environment.hpp>
#include <jank/profile/time.hpp>

// Native loaders
#include <clojure/core_native.hpp>
#include <jank/compiler_native.hpp>
#include <jank/perf_native.hpp>

// nREPL server asio native module (no header - declare extern)
extern "C" void *jank_load_jank_nrepl_server_asio();

namespace
{
  std::atomic<bool> running{ true };

  void signal_handler(int)
  {
    running = false;
  }

  // Command line options for compile server (parsed manually before jank_init)
  uint16_t g_port = jank::compile_server::default_compile_port;
  std::string g_target; // Required - must be "sim" or "device"
  std::string g_clang_path;
  std::string g_module_path; // Path to jank source files
  std::vector<std::string> g_include_paths; // Additional include paths for app headers
  std::vector<std::string> g_defines; // Preprocessor defines for app code
  std::vector<std::string> g_jit_libs; // Libraries to load into JIT for symbol resolution
}

int compile_server_main(int const /* argc */, char const ** /* argv */)
{
  using namespace jank;

  std::cout << "=== jank Compile Server ===" << std::endl;
  std::cout << "Target: iOS " << (g_target == "device" ? "Device" : "Simulator") << std::endl;
  std::cout << "Port: " << g_port << std::endl;

  // Load native libraries
  std::cout << "Loading native libraries..." << std::endl;
  {
    profile::timer const timer{ "load natives" };
    jank_load_clojure_core_native();
    jank_load_jank_compiler_native();
    jank_load_jank_perf_native();
    jank_load_jank_nrepl_server_asio();
  }

  // Load clojure.core for compilation
  std::cout << "Loading clojure.core..." << std::endl;
  {
    profile::timer const timer{ "load clojure.core" };
    runtime::__rt_ctx->load_module("/clojure.core", runtime::module::origin::latest).expect_ok();
  }

  // Create compile server configuration
  auto const jank_resource_dir = util::resource_dir();
  auto config = compile_server::make_ios_simulator_config(jank_resource_dir, g_clang_path);

  // Adjust for device target
  if(g_target == "device")
  {
    config.target_triple = "arm64-apple-ios17.0";
    config.ios_sdk_path = "/Applications/Xcode.app/Contents/Developer/Platforms/"
                          "iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk";
    config.pch_path = jank_resource_dir + "/ios_device_incremental.pch";
  }

  // Add app-specific include paths
  // These need to be added to BOTH:
  // 1. config.include_paths - for cross-compilation to iOS
  // 2. util::cli::opts.include_dirs - for local JIT compilation of native headers
  for(auto const &inc : g_include_paths)
  {
    config.include_paths.push_back(inc);
    util::cli::opts.include_dirs.push_back(inc);
    std::cout << "[compile-server] Added include path: " << inc << std::endl;
  }

  // Add app-specific preprocessor defines
  for(auto const &def : g_defines)
  {
    config.flags.push_back("-D" + def);
    std::cout << "[compile-server] Added define: " << def << std::endl;
  }

  // Start compile server
  std::cout << "Starting compile server..." << std::endl;
  compile_server::server server(g_port, std::move(config));
  server.start();

  if(!server.is_running())
  {
    std::cerr << "Failed to start compile server" << std::endl;
    return 1;
  }

  // Handle SIGINT/SIGTERM for graceful shutdown
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  std::cout << "\nReady to accept connections. Press Ctrl+C to stop.\n" << std::endl;

  // Wait for shutdown signal
  while(running && server.is_running())
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  std::cout << "\nShutting down..." << std::endl;
  server.stop();

  std::cout << "Compile server stopped." << std::endl;
  return 0;
}

int main(int argc, char **argv)
{
  // Parse compile-server specific options before jank_init
  for(int i = 1; i < argc; i++)
  {
    std::string arg = argv[i];
    if(arg == "--port" && i + 1 < argc)
    {
      g_port = static_cast<uint16_t>(std::stoi(argv[++i]));
    }
    else if(arg == "--target" && i + 1 < argc)
    {
      g_target = argv[++i];
    }
    else if(arg == "--clang" && i + 1 < argc)
    {
      g_clang_path = argv[++i];
    }
    else if(arg == "--module-path" && i + 1 < argc)
    {
      g_module_path = argv[++i];
    }
    else if((arg == "--include" || arg == "-I") && i + 1 < argc)
    {
      g_include_paths.push_back(argv[++i]);
    }
    else if(arg == "--jit-lib" && i + 1 < argc)
    {
      g_jit_libs.push_back(argv[++i]);
    }
    else if(arg == "-D" && i + 1 < argc)
    {
      g_defines.push_back(argv[++i]);
    }
    else if(arg.substr(0, 2) == "-D" && arg.length() > 2)
    {
      // Handle -DFOO=bar style
      g_defines.push_back(arg.substr(2));
    }
    else if(arg == "--help" || arg == "-h")
    {
      std::cout << "Usage: " << argv[0] << " --target <sim|device> [options]\n"
                << "Options:\n"
                << "  --target TARGET       iOS target: sim or device (REQUIRED)\n"
                << "  --port PORT           Compile server port (default: 5559)\n"
                << "  --module-path PATH    Path to jank source files\n"
                << "  --clang PATH          Path to clang++ for cross-compilation\n"
                << "  --include PATH, -I    Additional include path (can be repeated)\n"
                << "  --jit-lib PATH        Library to load into JIT (can be repeated)\n"
                << "  -D DEFINE             Preprocessor define (can be repeated)\n"
                << "  --help, -h            Show this help message\n";
      return 0;
    }
  }

  // Validate required options
  if(g_target.empty())
  {
    std::cerr << "Error: --target is required. Use --target sim or --target device\n";
    std::cerr << "Use --help for usage information.\n";
    return 1;
  }
  if(g_target != "sim" && g_target != "device")
  {
    std::cerr << "Error: --target must be 'sim' or 'device', got: " << g_target << "\n";
    return 1;
  }

  // CRITICAL: Set cli options BEFORE jank_init()
  // The runtime context and JIT processor are initialized during jank_init() and read these at that time.

  // Set module path for finding jank source files
  if(!g_module_path.empty())
  {
    jank::util::cli::opts.module_path = g_module_path;
    std::cout << "[compile-server] Module path: " << g_module_path << std::endl;
  }

  // Add include paths for native header compilation
  for(auto const &inc : g_include_paths)
  {
    jank::util::cli::opts.include_dirs.push_back(inc);
  }

  // Add JIT libraries for symbol resolution
  for(auto const &lib : g_jit_libs)
  {
    jank::util::cli::opts.jit_libs.push_back(lib);
    std::cout << "[compile-server] JIT lib: " << lib << std::endl;
  }

  // Use jank_init to properly initialize GC, LLVM, and runtime context
  return jank_init(argc,
                   const_cast<char const **>(argv),
                   /*init_default_ctx=*/true,
                   compile_server_main);
}
