// Remote Compilation Configuration Implementation
//
// Provides global state for remote JIT compilation.

#include <jank/compile_server/remote_compile.hpp>

namespace jank::compile_server
{
  remote_compile_config remote_config;
  std::mutex remote_config_mutex;
  std::unique_ptr<client> remote_client;

} // namespace jank::compile_server
