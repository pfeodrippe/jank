# iOS Remote Eval Flakiness Fix Plan

## Problem Statement

The iOS remote eval integration is flaky - sometimes it works, sometimes it returns "Unknown exception" errors. The issue appears to be related to the nREPL → iOS forwarding path, not the iOS eval server itself (since direct `nc` commands work reliably).

## Research Findings

### How Piggieback Works

Based on research from [nrepl/piggieback](https://github.com/nrepl/piggieback) and [CIDER docs](https://docs.cider.mx/cider/cljs/up_and_running.html):

1. **Session-Level Operation**: Piggieback operates at the nREPL session level. Once `(cider.piggieback/cljs-repl)` is called, the session is "switched" to ClojureScript mode.

2. **Middleware Intercept**: The `wrap-cljs-repl` middleware intercepts "eval" messages and routes them to the ClojureScript REPL environment.

3. **Session Binding**: Piggieback stores `*cljs-compiler-env*` in the session map. Other middleware (like cider-nrepl) detect ClojureScript mode by checking for this binding.

4. **Single Connection**: Piggieback maintains a single, persistent connection to the ClojureScript environment per session.

### How Shadow-cljs Does It

From [Shadow CLJS User's Guide](https://shadow-cljs.github.io/docs/UsersGuide.html) and [shadow-cljs nREPL issue](https://github.com/thheller/shadow-cljs/issues/62):

1. **Piggieback Emulation**: Shadow-cljs doesn't use actual Piggieback - it emulates it by providing the same session bindings.

2. **Build Selection**: User calls `(shadow/repl :build-id)` which sets the current session to forward evals to that build.

3. **WebSocket Connection**: Shadow-cljs maintains a WebSocket connection to the browser/Node.js runtime.

4. **Fake Piggieback Binding**: Shadow-cljs provides a fake `*cljs-compiler-env*` so cider-nrepl middleware works: https://github.com/thheller/shadow-cljs/blob/master/src/main/shadow/cljs/devtools/fake_piggieback.clj

### nREPL Session Mechanics

From [nREPL Middleware docs](https://nrepl.org/nrepl/design/middleware.html):

1. **Sessions are Persistent**: Sessions maintain state across multiple requests.

2. **Serialized Execution**: All requests within the same session are serialized.

3. **Transport Pattern**: Responses are sent via `(t/send transport (response-for msg ...))`.

## Root Cause Analysis

Our current implementation has several issues compared to Piggieback/shadow-cljs:

### Issue 1: Connection Per Request (LIKELY CAUSE)

**Current behavior**: We create a new connection OR reuse a potentially stale connection without proper health checks.

**Piggieback behavior**: Maintains a single, stable connection per session with proper lifecycle management.

**Fix needed**: Implement connection health checking and automatic reconnection.

### Issue 2: No Session State Tracking

**Current behavior**: We have a global `remote_target` but don't track it per nREPL session.

**Piggieback behavior**: Stores ClojureScript state in the nREPL session map.

**Impact**: Less critical for single-user scenarios, but could cause issues.

### Issue 3: Welcome Message Race Condition (LIKELY CAUSE)

**Current behavior**: We read the welcome message in `connect()`, but if timing is off, it might interfere with subsequent eval responses.

**Evidence**: "Works after nc is used once" - nc properly consumes any stale data.

**Fix needed**: More robust message framing and buffer management.

### Issue 4: Socket Timeout Not Working Reliably

**Current behavior**: We use `setsockopt(SO_RCVTIMEO)` which may not work correctly with Boost.Asio.

**Fix needed**: Use Boost.Asio's native async timeout mechanisms.

### Issue 5: No Ping/Health Check Before Eval

**Current behavior**: We assume connection is good if `connected_` flag is true.

**Piggieback behavior**: The ClojureScript REPL connection is managed by cljs.repl which handles reconnection.

**Fix needed**: Add a ping before first eval, or implement keepalive.

## Proposed Solution

### Phase 1: Connection Robustness (HIGH PRIORITY)

1. **Add ping before first eval**:
   ```cpp
   eval_result eval(std::string const &code, ...) {
     // Ping to verify connection is alive
     if(request_id_ == 0 || !ping()) {
       // Reconnect
       disconnect();
       if(!connect(host_, port_)) {
         return { false, "", "Failed to reconnect", "connection" };
       }
     }
     // ... rest of eval
   }
   ```

2. **Clear any stale data on connect**:
   ```cpp
   bool connect(...) {
     // ... connect ...

     // Read welcome message
     boost::asio::read_until(*socket_, buf, '\n');

     // Drain any extra data that might be buffered
     while(socket_->available() > 0) {
       char drain[1024];
       socket_->read_some(boost::asio::buffer(drain));
     }

     // Small delay for stability
     std::this_thread::sleep_for(std::chrono::milliseconds(100));
   }
   ```

3. **Use proper Boost.Asio timeout**:
   ```cpp
   // Instead of setsockopt, use async_read with deadline
   boost::asio::steady_timer timer(io_context_);
   timer.expires_after(std::chrono::seconds(30));

   bool got_response = false;
   boost::asio::async_read_until(*socket_, buf, '\n',
     [&](auto ec, auto n) { got_response = true; });
   timer.async_wait([&](auto) {
     if(!got_response) socket_->cancel();
   });
   io_context_.run();
   ```

### Phase 2: Protocol Improvements (MEDIUM PRIORITY)

1. **Add sequence numbers and validate responses**:
   ```cpp
   // Send
   {"op":"eval","id":42,"code":"..."}

   // Receive - verify id matches
   {"op":"result","id":42,"value":"..."}

   // If id doesn't match, we have a sync issue
   ```

2. **Add explicit ACK for connection establishment**:
   ```
   Server: {"op":"welcome","version":"0.1"}
   Client: {"op":"ready","id":0}
   Server: {"op":"ack","id":0}
   ```

### Phase 3: Session Integration (LOW PRIORITY - Future)

1. **Store iOS connection state in nREPL session** (like Piggieback):
   ```cpp
   // In the nREPL session map
   session["ios-connected"] = true;
   session["ios-client"] = client_ptr;
   ```

2. **Support multiple iOS targets per session** (future feature).

## Implementation Steps

### Step 1: Quick Fix for Flakiness

File: `include/cpp/jank/ios/eval_client.hpp`

```cpp
// Add connection verification method
bool verify_connection() {
  if(!is_connected()) return false;

  // Try a quick ping
  try {
    std::string request = R"({"op":"ping","id":0})" "\n";
    boost::asio::write(*socket_, boost::asio::buffer(request));

    // Non-blocking check for response
    boost::asio::streambuf buf;
    boost::system::error_code ec;

    // Wait briefly for pong
    socket_->non_blocking(false);
    struct timeval tv = {1, 0}; // 1 second
    setsockopt(socket_->native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    boost::asio::read_until(*socket_, buf, '\n', ec);
    if(ec) {
      connected_ = false;
      return false;
    }

    std::string response;
    std::istream is(&buf);
    std::getline(is, response);

    return response.find("pong") != std::string::npos;
  } catch(...) {
    connected_ = false;
    return false;
  }
}

// Modify eval to verify connection first
eval_result eval(...) {
  if(!verify_connection()) {
    // Try reconnecting once
    disconnect();
    if(!connect(host_, port_)) {
      return { false, "", "Connection lost, reconnect failed", "connection" };
    }
  }
  // ... rest of eval
}
```

### Step 2: Fix Welcome Message Handling

```cpp
bool connect(...) {
  // ... socket setup ...

  // Read welcome message with proper error handling
  boost::asio::streambuf buf;
  boost::system::error_code ec;
  boost::asio::read_until(*socket_, buf, '\n', ec);

  if(ec) {
    last_error_ = "Failed to read welcome: " + ec.message();
    return false;
  }

  std::string welcome;
  std::istream is(&buf);
  std::getline(is, welcome);

  // Verify it's actually a welcome message
  if(welcome.find("welcome") == std::string::npos) {
    last_error_ = "Unexpected response: " + welcome;
    return false;
  }

  std::cout << "[ios-client] Connected: " << welcome << std::endl;

  // IMPORTANT: Drain any extra buffered data
  while(socket_->available() > 0) {
    char drain[256];
    socket_->read_some(boost::asio::buffer(drain), ec);
  }

  // Delay to ensure iOS side is ready
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  connected_ = true;
  return true;
}
```

### Step 3: Response Validation

```cpp
eval_result parse_response(std::string const &json) {
  auto op = get_json_string(json, "op");
  auto resp_id = get_json_int(json, "id");

  // Validate response ID matches request
  if(resp_id != request_id_) {
    std::cerr << "[ios-client] Response ID mismatch! Expected "
              << request_id_ << ", got " << resp_id << std::endl;
    // This indicates a sync issue - we got someone else's response
    // or a stale response
    return { false, "", "Response sync error", "protocol" };
  }

  // ... rest of parsing
}
```

## Testing Plan

1. **Rapid fire evals**: Send 10 evals in quick succession
2. **Disconnect/reconnect**: Kill iproxy mid-session, restart, verify recovery
3. **Long idle**: Connect, wait 5 minutes, eval again
4. **Concurrent sessions**: Two CIDER connections to same nREPL
5. **Error recovery**: Send invalid code, then valid code

## References

- [nrepl/piggieback](https://github.com/nrepl/piggieback) - ClojureScript nREPL middleware
- [Shadow CLJS User's Guide](https://shadow-cljs.github.io/docs/UsersGuide.html) - nREPL integration docs
- [CIDER shadow-cljs docs](https://docs.cider.mx/cider/cljs/shadow-cljs.html) - CIDER integration
- [nREPL Middleware](https://nrepl.org/nrepl/design/middleware.html) - How middleware works
- [Building nREPL Middleware](https://nrepl.org/nrepl/building_middleware.html) - Middleware patterns

## Phase 4: AOT nREPL Engine for iOS (FUTURE - Major Feature)

### The Big Idea

Currently we use a split architecture:
- **macOS**: Full nREPL server (eval, completion, lookup, docs, etc.)
- **iOS**: Minimal eval server (just `eval_string()`)

**Question**: Could we AOT compile the full nREPL engine for iOS JIT?

### Why The Current Split Exists

The original reason was **stack overflow during JIT compilation** of the heavily-templated nREPL code on iOS. The nREPL engine in `engine.hpp` has deep template instantiation.

**Key Insight**: The stack overflow happens during **JIT compilation**, not during **AOT compilation**!

If we AOT compile the nREPL engine:
1. Templates are expanded at build time (on macOS with plenty of stack)
2. The compiled code runs on iOS without template instantiation
3. No stack overflow risk!

### Benefits of Full nREPL on iOS

1. **Zero Network Latency**: All evals are local, no TCP round-trips
2. **Full Feature Set**: Completion, documentation lookup, find-definition all work
3. **Works Offline**: No need for macOS proxy or USB connection
4. **Simpler Debugging**: One process, one runtime
5. **True Mobile Development**: Could connect directly from iPad to iPad

### Challenges to Solve

#### 1. Boost.Asio on iOS
The nREPL engine uses Boost.Asio for networking. Options:
- **Option A**: Bundle Boost.Asio in iOS build (adds ~2-3MB)
- **Option B**: Create iOS-specific nREPL using BSD sockets (like eval_server)
- **Option C**: Use a lightweight async library (libuv, etc.)

**Recommendation**: Option A is simplest. Boost is header-only for Asio.

#### 2. Binary Size
Full nREPL adds more code. Estimate:
- Current iOS JIT bundle: ~X MB
- nREPL engine addition: ~1-2 MB (rough estimate)
- Boost.Asio headers: Already used, minimal addition

**Mitigation**: Use LTO (Link-Time Optimization) to strip unused code.

#### 3. bencode Library
nREPL uses bencode for wire protocol. This is already in jank and should AOT fine.

#### 4. Thread Safety
nREPL spawns threads for handling connections. iOS has threading restrictions.

**Solution**: Use the same pthread approach as eval_server, with GC registration.

### Implementation Approach

#### Step 1: Create iOS nREPL Build Target

```cmake
# In CMakeLists.txt or iOS build script
add_library(jank_nrepl_ios STATIC
  src/cpp/jank/nrepl_server/asio.cpp
  # ... other nREPL sources
)

target_compile_definitions(jank_nrepl_ios PRIVATE
  JANK_IOS=1
  JANK_NREPL_AOT=1
)
```

#### Step 2: iOS-Specific Adaptations

```cpp
// In engine.hpp
#ifdef JANK_IOS
  // Use GC thread registration
  void start_server() {
    GC_allow_register_threads();
    // ... spawn with pthread like eval_server
  }
#else
  // Normal std::thread approach
#endif
```

#### Step 3: Remove/Stub Unused Features

Some nREPL features might not make sense on iOS:
- `load-file` from filesystem (sandboxed on iOS)
- Shell integration
- File watching

```cpp
#ifdef JANK_IOS
  // Return "not available on iOS" for these ops
  void handle_load_file(message const& msg) {
    send_error(msg, "load-file not available on iOS");
  }
#endif
```

#### Step 4: Test AOT Compilation

```bash
# Build nREPL for iOS target
./bin/build-ios-nrepl.sh

# Verify template instantiation completes
# Verify binary size is acceptable
# Verify it links into iOS app
```

### Migration Path

1. **Phase 1** (Current): Remote eval via minimal server
2. **Phase 2**: Fix flakiness (this plan)
3. **Phase 3**: AOT nREPL as optional feature
4. **Phase 4**: Make AOT nREPL the default

### Hybrid Approach (Intermediate Solution)

Even with AOT nREPL on iOS, we might want to keep remote eval for:
- **Debugging the debugger**: Connect from macOS CIDER to debug iOS nREPL itself
- **Multi-device**: Evaluate on multiple iOS devices from one editor
- **Resource-constrained devices**: Older iPads might prefer lightweight eval server

```cpp
// iOS could run BOTH:
// 1. Full nREPL server on port 5557
// 2. Lightweight eval server on port 5558

// User chooses which to connect to based on needs
```

### Architecture Comparison

```
CURRENT (Remote Eval):
┌─────────────────────────────────────────────────────────────┐
│                         macOS                                │
│  ┌─────────────┐     ┌─────────────────────────────────┐   │
│  │   Emacs     │────▶│      Full nREPL Server          │   │
│  │   CIDER     │     │  (eval, complete, lookup, etc.) │   │
│  └─────────────┘     └──────────────┬──────────────────┘   │
└─────────────────────────────────────┼──────────────────────┘
                                      │ TCP/USB
┌─────────────────────────────────────┼──────────────────────┐
│                         iPad        │                       │
│  ┌──────────────────────────────────▼───────────────────┐  │
│  │         Minimal Eval Server (just eval_string)       │  │
│  └──────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘

FUTURE (AOT nREPL on iOS):
┌─────────────────────────────────────────────────────────────┐
│                         iPad                                 │
│  ┌─────────────────────────────────────────────────────┐   │
│  │           Full nREPL Server (AOT compiled)          │   │
│  │     (eval, complete, lookup, docs - all local!)     │   │
│  └─────────────────────────────────────────────────────┘   │
│                           ▲                                 │
│                           │ localhost:5557                  │
│  ┌─────────────────────────────────────────────────────┐   │
│  │              iPad Editor / REPL Client               │   │
│  │           (or connect from Mac via USB)              │   │
│  └─────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

### Estimated Effort

- **Phase 1-2 (Fix flakiness)**: 1-2 days
- **Phase 3 (AOT nREPL)**: 1-2 weeks
  - Build system changes: 2-3 days
  - iOS adaptations: 2-3 days
  - Testing & debugging: 3-5 days
- **Phase 4 (Production ready)**: 1 week additional polish

## Summary

The flakiness is most likely caused by:
1. **Stale data in the socket buffer** after welcome message
2. **No connection health verification** before sending evals
3. **Response sync issues** if buffers get out of sync

The fix involves:
1. Adding connection verification (ping) before first eval
2. Properly draining socket buffers after connect
3. Validating response IDs match request IDs
4. Better error handling and automatic reconnection

**Future opportunity**: AOT compile the full nREPL engine for iOS, eliminating the need for remote eval entirely. This is feasible because the stack overflow issue occurs during JIT compilation of templates, not during AOT compilation or runtime execution.
