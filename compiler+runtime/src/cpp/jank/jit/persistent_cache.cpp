#include <jank/jit/persistent_cache.hpp>
#include <jank/util/environment.hpp>
#include <jank/util/fmt/print.hpp>
#include <jank/util/clang.hpp>

#include <iomanip>
#include <locale>
#include <sstream>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>

namespace jank::jit
{
  /* Helper to format hash as 16-character zero-padded hex string. */
  static std::string format_hash(u64 const hash)
  {
    std::ostringstream ss;
    ss.imbue(std::locale::classic()); /* Use C locale to avoid thousand separators */
    ss << std::hex << std::setw(16) << std::setfill('0') << hash;
    return ss.str();
  }
  /* Thread-local JIT cache context. */
  static thread_local jit_cache_context tl_cache_context;

  jit_cache_context &get_jit_cache_context()
  {
    return tl_cache_context;
  }

  persistent_cache::persistent_cache(jtl::immutable_string const &binary_version)
    : cache_dir_{}
    , binary_version_{ binary_version }
  {
    try
    {
      cache_dir_ = std::filesystem::path{ util::user_cache_dir(binary_version).c_str() } / "jit_cache";
      std::filesystem::create_directories(cache_dir_);
    }
    catch(std::exception const &)
    {
      /* If we can't create the cache directory, just leave it empty.
       * The has_cached_source and save_source methods will handle this gracefully. */
      cache_dir_.clear();
    }
  }

  std::filesystem::path const &persistent_cache::cache_dir() const
  {
    return cache_dir_;
  }

  bool persistent_cache::has_cached_source(u64 const hash) const
  {
    if(cache_dir_.empty())
    {
      return false;
    }
    auto const cpp_path{ cache_dir_ / (format_hash(hash) + ".cpp") };
    auto const meta_path{ cache_dir_ / (format_hash(hash) + ".meta") };
    return std::filesystem::exists(cpp_path) && std::filesystem::exists(meta_path);
  }

  /* TODO: Make cache writing asynchronous to reduce overhead.
   * Current implementation: synchronous file I/O adds ~25-80% overhead.
   * Future improvement: use a background thread with a queue to write cache files.
   * This would allow eval to return immediately while cache is written in background. */
  void persistent_cache::save_source(u64 const hash,
                                     jtl::immutable_string const &cpp_source,
                                     jtl::immutable_string const &qualified_name,
                                     jtl::immutable_string const &unique_name) const
  {
    if(cache_dir_.empty())
    {
      return;
    }
    auto const cpp_path{ cache_dir_ / (format_hash(hash) + ".cpp") };
    auto const meta_path{ cache_dir_ / (format_hash(hash) + ".meta") };

    /* Save C++ source. */
    {
      std::ofstream out{ cpp_path };
      if(out)
      {
        out.write(cpp_source.data(), static_cast<std::streamsize>(cpp_source.size()));
      }
    }

    /* Save metadata (format: qualified_name\nunique_name\n). */
    {
      std::ofstream out{ meta_path };
      if(out)
      {
        out << qualified_name.c_str() << '\n' << unique_name.c_str() << '\n';
      }
    }
  }

  void persistent_cache::save_expression(u64 const hash, jtl::immutable_string const &expr_str) const
  {
    if(cache_dir_.empty())
    {
      return;
    }
    auto const expr_path{ cache_dir_ / (format_hash(hash) + ".expr") };
    std::ofstream out{ expr_path };
    if(out)
    {
      out.write(expr_str.data(), static_cast<std::streamsize>(expr_str.size()));
    }
  }

  bool persistent_cache::has_compiled_object(u64 const hash) const
  {
    if(cache_dir_.empty())
    {
      return false;
    }
    return std::filesystem::exists(object_path(hash));
  }

  std::filesystem::path persistent_cache::object_path(u64 const hash) const
  {
    return cache_dir_ / (format_hash(hash) + ".o");
  }

  std::filesystem::path persistent_cache::source_path(u64 const hash) const
  {
    return cache_dir_ / (format_hash(hash) + ".cpp");
  }

  std::filesystem::path persistent_cache::expression_path(u64 const hash) const
  {
    return cache_dir_ / (format_hash(hash) + ".expr");
  }

  std::string persistent_cache::factory_name(u64 const hash)
  {
    return "jank_pcache_factory_" + format_hash(hash);
  }

  bool persistent_cache::compile_to_object(u64 const hash) const
  {
    if(cache_dir_.empty())
    {
      return false;
    }

    auto const src_path{ source_path(hash) };
    auto const obj_path{ object_path(hash) };
    auto const expr_path{ expression_path(hash) };

    if(!std::filesystem::exists(src_path))
    {
      return false;
    }

    /* Read the C++ source. */
    std::string cpp_source;
    {
      std::ifstream in{ src_path };
      if(!in)
      {
        return false;
      }
      std::stringstream buffer;
      buffer << in.rdbuf();
      cpp_source = buffer.str();
    }

    /* Read the expression to generate factory function. */
    std::string expr_str;
    if(std::filesystem::exists(expr_path))
    {
      std::ifstream in{ expr_path };
      if(in)
      {
        std::stringstream buffer;
        buffer << in.rdbuf();
        expr_str = buffer.str();
      }
    }

    /* Generate a factory function name based on the hash. */
    auto const fn_name{ factory_name(hash) };

    /* Create modified source with factory function. */
    std::string full_source = cpp_source;
    if(!expr_str.empty())
    {
      full_source += "\nextern \"C\" jank::runtime::object* " + fn_name + "() {\n";
      full_source += "  return " + expr_str + ";\n";
      full_source += "}\n";
    }

    /* Write the modified source to a temp file. */
    auto const temp_src_path{ cache_dir_ / (format_hash(hash) + "_full.cpp") };
    {
      std::ofstream out{ temp_src_path };
      if(!out)
      {
        return false;
      }
      out << full_source;
    }

    /* Build args vector for invoke_clang, similar to how the JIT processor does it. */
    std::vector<char const *> args;

    /* Parse JANK_JIT_FLAGS - these are the flags used to build jank and are needed for
     * compatible object files. */
    std::stringstream flags{ JANK_JIT_FLAGS };
    std::string flag;
    while(std::getline(flags, flag, ' '))
    {
      args.emplace_back(strdup(flag.c_str()));
    }

    /* Find clang and its directories. */
    auto const clang_opt{ util::find_clang() };
    if(clang_opt.is_none())
    {
      std::filesystem::remove(temp_src_path);
      return false;
    }
    auto const clang_dir{ std::filesystem::path{ clang_opt.unwrap().c_str() }.parent_path() };
    args.emplace_back("-I");
    args.emplace_back(strdup((clang_dir / "../include").c_str()));

    auto const clang_resource_opt{ util::find_clang_resource_dir() };
    if(clang_resource_opt.is_none())
    {
      std::filesystem::remove(temp_src_path);
      return false;
    }
    args.emplace_back("-resource-dir");
    args.emplace_back(clang_resource_opt.unwrap().c_str());

    /* Add jank resource directories. */
    auto const jank_resource_dir{ util::resource_dir() };
    args.emplace_back("-I");
    args.emplace_back(strdup(util::format("{}/include", jank_resource_dir).c_str()));
    args.emplace_back("-L");
    args.emplace_back(strdup(util::format("{}/lib", jank_resource_dir).c_str()));

    /* Include the PCH - essential for type definitions. */
    auto const pch_path{ util::find_pch(binary_version_) };
    if(pch_path.is_some())
    {
      args.emplace_back("-include-pch");
      args.emplace_back(strdup(pch_path.unwrap().c_str()));
    }

    /* Suppress warnings like the JIT does. */
    args.emplace_back("-w");
    args.emplace_back("-Wno-c++11-narrowing");

    /* Compile to object file, position-independent code for dynamic loading. */
    args.emplace_back("-c");
    args.emplace_back("-fPIC");
    args.emplace_back("-o");
    args.emplace_back(strdup(obj_path.string().c_str()));
    args.emplace_back(strdup(temp_src_path.string().c_str()));

    /* Redirect stderr to suppress error output for code using external types.
     * This is expected to fail silently for such code. */
    int const saved_stderr = dup(STDERR_FILENO);
    int const null_fd = open("/dev/null", O_WRONLY);
    if(null_fd >= 0)
    {
      dup2(null_fd, STDERR_FILENO);
      close(null_fd);
    }

    auto const result{ util::invoke_clang(args) };

    /* Restore stderr. */
    if(saved_stderr >= 0)
    {
      dup2(saved_stderr, STDERR_FILENO);
      close(saved_stderr);
    }

    /* Clean up temp file. */
    std::filesystem::remove(temp_src_path);

    return result.is_ok() && std::filesystem::exists(obj_path);
  }

  jtl::option<persistent_cache::cache_entry> persistent_cache::load_entry(u64 const hash) const
  {
    if(cache_dir_.empty())
    {
      return jtl::none;
    }
    auto const cpp_path{ cache_dir_ / (format_hash(hash) + ".cpp") };
    auto const meta_path{ cache_dir_ / (format_hash(hash) + ".meta") };
    auto const expr_path{ cache_dir_ / (format_hash(hash) + ".expr") };

    if(!std::filesystem::exists(cpp_path) || !std::filesystem::exists(meta_path))
    {
      return jtl::none;
    }

    cache_entry entry;

    /* Load metadata. */
    {
      std::ifstream in{ meta_path };
      if(!in)
      {
        return jtl::none;
      }
      std::string qualified_name, unique_name;
      std::getline(in, qualified_name);
      std::getline(in, unique_name);
      entry.qualified_name = jtl::immutable_string{ qualified_name.data(), qualified_name.size() };
      entry.unique_name = jtl::immutable_string{ unique_name.data(), unique_name.size() };
    }

    /* Load C++ source. */
    {
      std::ifstream in{ cpp_path };
      if(!in)
      {
        return jtl::none;
      }
      std::stringstream buffer;
      buffer << in.rdbuf();
      auto const str{ buffer.str() };
      entry.cpp_source = jtl::immutable_string{ str.data(), str.size() };
    }

    /* Load expression string (optional - may not exist for older cache entries). */
    if(std::filesystem::exists(expr_path))
    {
      std::ifstream in{ expr_path };
      if(in)
      {
        std::stringstream buffer;
        buffer << in.rdbuf();
        auto const str{ buffer.str() };
        entry.expression_str = jtl::immutable_string{ str.data(), str.size() };
      }
    }

    return entry;
  }

  void persistent_cache::clear() const
  {
    if(cache_dir_.empty())
    {
      return;
    }
    std::filesystem::remove_all(cache_dir_);
    std::filesystem::create_directories(cache_dir_);
  }

  persistent_cache::stats persistent_cache::get_stats() const
  {
    size_t entries{};
    if(!cache_dir_.empty() && std::filesystem::exists(cache_dir_))
    {
      for(auto const &entry : std::filesystem::directory_iterator{ cache_dir_ })
      {
        if(entry.path().extension() == ".meta")
        {
          ++entries;
        }
      }
    }
    return stats{ entries, disk_hits_, disk_misses_ };
  }

  void persistent_cache::record_disk_hit() const
  {
    ++disk_hits_;
  }

  void persistent_cache::record_disk_miss() const
  {
    ++disk_misses_;
  }
}
