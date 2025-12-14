#include <fstream>
#include <random>

#include <jank/profile/time.hpp>
#include <jank/util/fmt/print.hpp>
#include <jank/util/cli.hpp>

namespace jank::profile
{
  using util::cli::opts;

  static constexpr jtl::immutable_string_view tag{ "jank::profile" };
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
  static std::ofstream output;
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
  static thread_local std::minstd_rand rng{ std::random_device{}() };
  /* Track which calls are being sampled (for paired enter/exit).
   * We use a counter - increment on sampled enter, decrement on exit. */
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
  static thread_local i32 sampling_depth{ 0 };
  /* Track depth inside clojure.core to avoid profiling core-to-core calls. */
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
  static thread_local i32 core_depth{ 0 };

  static auto now()
  {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
  }

  /* Returns true if this event should be sampled (recorded).
   * When sample_rate is 0, all events are recorded.
   * When sample_rate is N, ~1/N events are recorded (random sampling). */
  static bool should_sample()
  {
    if(opts.profiler_sample_rate == 0)
    {
      return true;
    }
    return (rng() % opts.profiler_sample_rate) == 0;
  }

  void configure()
  {
    if(opts.profiler_enabled)
    {
      output.open(opts.profiler_file.data());
      if(!output.is_open())
      {
        opts.profiler_enabled = false;
        util::println(stderr,
                      "Unable to open profile file: {}\nProfiling is now disabled.",
                      opts.profiler_file);
      }
    }
  }

  bool is_enabled()
  {
    return opts.profiler_enabled;
  }

  void enter(jtl::immutable_string_view const &region)
  {
    if(opts.profiler_enabled)
    {
      /* When sampling: if we're already in a sampled region, continue sampling.
       * Otherwise, randomly decide whether to start sampling this subtree. */
      if(opts.profiler_sample_rate > 0)
      {
        if(sampling_depth > 0 || should_sample())
        {
          ++sampling_depth;
          output << util::format("{} {} enter {}\n", tag, now(), region);
        }
      }
      else
      {
        output << util::format("{} {} enter {}\n", tag, now(), region);
      }
    }
  }

  void exit(jtl::immutable_string_view const &region)
  {
    if(opts.profiler_enabled)
    {
      /* When sampling: only output exit if we're in a sampled region. */
      if(opts.profiler_sample_rate > 0)
      {
        if(sampling_depth > 0)
        {
          output << util::format("{} {} exit {}\n", tag, now(), region);
          --sampling_depth;
        }
      }
      else
      {
        output << util::format("{} {} exit {}\n", tag, now(), region);
      }
    }
  }

  void enter_core(jtl::immutable_string_view const &region)
  {
    /* Only profile clojure.core calls from non-core code.
     * This avoids profiling internal core-to-core calls which add noise. */
    if(core_depth == 0)
    {
      enter(region);
    }
    ++core_depth;
  }

  void exit_core(jtl::immutable_string_view const &region)
  {
    --core_depth;
    if(core_depth == 0)
    {
      exit(region);
    }
  }

  void report(jtl::immutable_string_view const &boundary)
  {
    if(opts.profiler_enabled)
    {
      output << util::format("{} {} report {}\n", tag, now(), boundary);
    }
  }

  timer::timer(jtl::immutable_string_view const &region)
    : region{ region }
  {
    enter(region);
  }

  timer::~timer()
  try
  {
    exit(region);
  }
  catch(...)
  {
    /* NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): I need to log without exceptions. */
    std::printf("Exception caught while destructing timer");
  }

  void timer::report(jtl::immutable_string_view const &boundary) const
  {
    jank::profile::report(boundary);
  }
}
