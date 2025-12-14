# jank Profiling Plan

## Current State

### Existing Infrastructure

jank already has several profiling mechanisms:

1. **`profile::timer`** (`profile/time.hpp`)
   - RAII-based timer logging enter/exit with nanosecond timestamps
   - Enabled via `--profile` CLI flag
   - Output file: `--profile-output` (default: `jank.profile`)
   - Currently instruments: module loading, JIT compilation, codegen, eval, var operations
   - Output format: `jank::profile <timestamp_ns> <enter|exit|report> <region_name>`

2. **nanobench integration** (`runtime/perf.cpp`)
   - Exposed as `jank.perf-native/benchmark`
   - Usage: `(benchmark {:label "test"} #(my-fn))`
   - Good for microbenchmarking individual functions

3. **`time` macro** (clojure.core line 4052)
   - Standard `(time expr)` macro printing elapsed ms
   - Uses `current-time` (nanosecond precision)

4. **`--perf` flag**
   - Generates debug symbols (`-g`) for JIT'd code
   - Sets up `JITEventPrinter` for Linux perf integration
   - Limited usefulness on macOS

---

## Profiling Questions to Answer

### Compile-Time vs Runtime
- How much time in lexing/parsing?
- How much time in analysis (semantic analysis, type inference)?
- How much time in codegen (C++ generation vs LLVM IR)?
- How much time in JIT compilation?

### Runtime Performance
- Where are hot paths (frequent function calls)?
- Where is boxing/unboxing overhead?
- GC pressure and allocation patterns?
- Lock contention (var lookups use synchronized maps)?

### Form-Level Profiling
- Which jank forms are slow?
- Macro expansion overhead?
- Lazy sequence realization?

---

## Tools Available on macOS

### System Tools
| Tool | Description | Use Case |
|------|-------------|----------|
| **xctrace** / **Instruments** | Apple's profiler | Full CPU profiling, Time Profiler, Allocations |
| **sample** | CLI sampling profiler | Quick CPU sampling |
| **dtrace** | System tracing | Limited by SIP, good for syscalls |
| **leaks** | Memory leak detection | Memory debugging |

### Third-Party Tools
| Tool | Description | Integration Effort |
|------|-------------|-------------------|
| **[Tracy](https://github.com/wolfpld/tracy)** | Real-time frame profiler | Medium - requires macro instrumentation |
| **[FlameGraph](https://github.com/brendangregg/FlameGraph)** | Stack trace visualization | Low - works with existing data |
| **[heaptrack](https://github.com/KDE/heaptrack)** | Heap memory profiler | Low - external tool |
| **gperftools** | Google's CPU+heap profiler | Medium - needs linking |

---

## Proposed Profiling Strategy

### Phase 1: Leverage Existing Infrastructure

#### 1.1 Profile Data Visualization
The existing `profile::timer` outputs timestamped events. Create a tool to:
- Parse `jank.profile` output
- Generate flame graphs or timeline views
- Calculate aggregated statistics per region

```bash
# Run with profiling enabled
./build/jank run --profile --profile-output profile.txt -e "(+ 1 2)"

# Parse and visualize (to be created)
./bin/profile-analyze profile.txt
```

#### 1.2 Add More Profile Points
Current instrumentation covers high-level phases. Add granular points:

```cpp
// In analyze/processor.cpp
profile::timer const timer{ "analyze:def" };
profile::timer const timer{ "analyze:fn" };
profile::timer const timer{ "analyze:let" };

// In codegen
profile::timer const timer{ "codegen:fn_body" };
profile::timer const timer{ "codegen:type_check" };
```

### Phase 2: Instruments Integration (macOS)

#### 2.1 Using xctrace
```bash
# Record CPU profile
xctrace record --template 'Time Profiler' --launch -- ./build/jank run -e "(dotimes [i 1000000] (+ i i))"

# Open in Instruments
open *.trace
```

#### 2.2 Flame Graph from Sample
```bash
# Sample the running process
sample jank 10 -file jank.sample

# Convert to flame graph (requires scripts)
stackcollapse-sample.pl jank.sample | flamegraph.pl > jank-flame.svg
```

### Phase 3: Tracy Integration (Optional, Higher Effort)

Tracy provides real-time profiling visualization. Integration steps:

1. Add Tracy as third-party dependency
2. Create jank Tracy macros:
   ```cpp
   // In profile/tracy.hpp
   #ifdef JANK_TRACY
   #include <tracy/Tracy.hpp>
   #define JANK_ZONE_SCOPED ZoneScoped
   #define JANK_ZONE_NAMED(name) ZoneScopedN(name)
   #else
   #define JANK_ZONE_SCOPED
   #define JANK_ZONE_NAMED(name)
   #endif
   ```
3. Instrument hot paths

### Phase 4: jank-Level Profiling ✅ IMPLEMENTED

The `profile` macro is now available in `clojure.core`:

```clojure
;; Check if profiling is enabled
(profile-enabled?)  ; => true/false based on --profile flag

;; Profile a form - timing appears in profile output file
(profile "my-operation" (do-something))

;; Example: Profile your -main function
(defn -main []
  (profile "init" (initialize!))
  (profile "load-data" (load-data))
  (profile "process" (process-data))
  (profile "render" (render-output)))
```

#### Implementation Details

The `profile` macro:
- Only adds overhead when `--profile` is enabled
- Integrates with the same `jank.profile` output file
- Uses enter/exit semantics for proper nesting with C++ profiling

C++ functions exposed in `clojure::core_native`:
- `profile_enter(label)` - record entry into a profiled region
- `profile_exit(label)` - record exit from a profiled region
- `profile_enabled()` - check if profiling is enabled

#### 4.2 Compilation Phase Breakdown
Expose compilation stats via nREPL or REPL:

```clojure
(jank.profiler/last-compile-stats)
;; => {:read-ms 0.5
;;     :analyze-ms 2.3
;;     :codegen-ms 15.2
;;     :jit-ms 45.1
;;     :total-ms 63.1}
```

---

## Immediate Actions

### Quick Wins (Low Effort)
1. **Parse existing profile output** - Create a simple parser script
2. **Add `--profile` to your test runs** - Baseline the compilation pipeline
3. **Use `time` macro** - Profile specific jank expressions

### Short-Term (Medium Effort)
1. **Instruments workflow** - Document how to use xctrace with jank
2. **Add more profile points** - Instrument analyze/codegen more granularly
3. **Create `jank.profiler` namespace** - jank-level profiling API

### Long-Term (Higher Effort)
1. **Tracy integration** - Real-time visualization
2. **Flame graph generation** - From profile::timer data
3. **Memory profiling** - GC and allocation tracking

---

## Example Profiling Workflow

```bash
# 1. Run with built-in profiling
cd compiler+runtime
./build/jank run --profile --profile-output perf.txt -e "
(defn fib [n]
  (if (<= n 1)
    n
    (+ (fib (- n 1)) (fib (- n 2)))))
(time (fib 30))
"

# 2. Analyze profile output
cat perf.txt | grep -E "enter|exit" | head -50

# 3. Use Instruments for deep CPU analysis
xctrace record --template 'Time Profiler' --launch -- \
  ./build/jank run -e "(dotimes [i 100000] (+ i i))"

# 4. Sample a running REPL
./build/jank repl &
PID=$!
sample $PID 10 -file repl.sample
kill $PID
```

---

## Build Flags for Profiling

Ensure frame pointers are preserved for accurate stack traces:

```cmake
# In CMakeLists.txt for profile builds
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-omit-frame-pointer")
```

Current jank build should already support this with `--debug` mode.

---

## References

- [Flame Graphs](https://www.brendangregg.com/flamegraphs.html)
- [Tracy Profiler](https://github.com/wolfpld/tracy)
- [nanobench](https://github.com/martinus/nanobench) (already integrated)
- [C++ Profiling Tools](https://hackingcpp.com/cpp/tools/profilers.html)
- [macOS Profiling with Flame Graphs](https://medium.com/@techhara/profiling-visualize-program-bottleneck-with-flamegraph-macos-4a2b3598df8a)
