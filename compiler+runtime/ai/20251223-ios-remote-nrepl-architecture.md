# iOS Remote nREPL Architecture Research

**Date:** 2025-12-23
**Status:** Research & Architecture Design
**Problem:** nREPL server causes stack overflow on iOS due to deep template instantiation in `visit_object` and initialization complexity

---

## Executive Summary

Running the full nREPL server on iOS causes stack overflow due to:
1. Deep C++ template instantiation in `visit_object`, `get_in`, and hash operations
2. Complex nREPL middleware initialization
3. iOS's limited stack size (even with 8MB pthread stack)

This document explores **three architectural approaches** to enable interactive jank development on iOS devices:

1. **Minimal TCP Eval Server** - Bypass nREPL, direct eval on iOS (Recommended)
2. **Remote Proxy nREPL** - macOS handles nREPL protocol, iOS just evals
3. **Cross-Compile Hot Reload** - Compile on macOS, send compiled code to iOS

---

## Current Architecture

### How jank JIT Works on iOS

```
┌─────────────────────────────────────────────────────────────┐
│                     iOS Device (with Xcode)                  │
├─────────────────────────────────────────────────────────────┤
│  jank Code String                                           │
│       ↓                                                     │
│  Lexer (read::lex::processor)                               │
│       ↓                                                     │
│  Parser (read::parse::processor)                            │
│       ↓                                                     │
│  Analyzer (analyze::processor)                              │
│       ↓                                                     │
│  Optimizer (analyze::pass::optimize)                        │
│       ↓                                                     │
│  C++ Code Generator (codegen::processor)                    │
│       ↓                                                     │
│  CppInterOp/Clang JIT                                       │
│  (Cpp::Interpreter::ParseAndExecute)                        │
│       ↓                                                     │
│  Native ARM64 Code Execution                                │
└─────────────────────────────────────────────────────────────┘
```

**Key point:** JIT works on iOS when connected to Xcode debugger (grants JIT entitlement).

### Why nREPL Causes Stack Overflow

The stack trace shows:
```
___chkstk_darwin                    ← Stack overflow!
jank::runtime::visit_object<...>    ← Deep template instantiation
jank::hash::visit                   ← Hashing objects
persistent_hash_map::get            ← Map lookup
jank::runtime::get_in               ← get_in iteration
```

The problem is NOT recursion in `get_in` (it uses a loop), but the **C++ template machinery**:
- Each `visit_object` call creates multiple stack frames for template specialization
- Each `get` operation triggers hash computation with more template frames
- nREPL initialization calls many operations that compound this

**Even with 8MB stack**, the accumulated frames from nREPL's initialization overflow.

---

## Solution 1: Minimal TCP Eval Server (RECOMMENDED)

### Concept

Bypass the full nREPL protocol. Create a minimal TCP server on iOS that:
1. Accepts raw code strings
2. Calls `eval_string()` directly
3. Returns result

This avoids:
- Bencode encoding/decoding
- 18+ nREPL operations
- Middleware stack
- Session management
- Complex print functions

### Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                        macOS                                 │
├─────────────────────────────────────────────────────────────┤
│  Editor (Emacs/VSCode)                                      │
│       │                                                     │
│       ↓                                                     │
│  nREPL Client (CIDER/Calva)                                │
│       │                                                     │
│       ↓                                                     │
│  nREPL→Simple Protocol Adapter                             │
│  (converts nREPL messages to plain text)                   │
│       │                                                     │
│       └──────────── TCP ─────────────────┐                 │
└──────────────────────────────────────────│─────────────────┘
                                           │
┌──────────────────────────────────────────│─────────────────┐
│                     iOS Device           │                  │
├──────────────────────────────────────────│─────────────────┤
│                                          ↓                  │
│  ┌─────────────────────────────────────────┐               │
│  │  Minimal TCP Eval Server                │               │
│  │  ─────────────────────────────────────  │               │
│  │  • Accept connection                    │               │
│  │  • Read line (code string)              │               │
│  │  • __rt_ctx->eval_string(code)         │               │
│  │  • Send result string                   │               │
│  │  • Signal handler for crash recovery    │               │
│  └─────────────────────────────────────────┘               │
│                        ↓                                    │
│               jank Runtime (AOT + JIT)                     │
└─────────────────────────────────────────────────────────────┘
```

### Implementation (~250 lines)

```cpp
// minimal_eval_server.hpp
#pragma once
#include <boost/asio.hpp>
#include <jank/runtime/context.hpp>

namespace jank::ios {

using boost::asio::ip::tcp;

class minimal_eval_server {
public:
    minimal_eval_server(uint16_t port = 5558)
        : acceptor_(io_context_, tcp::endpoint(tcp::v4(), port))
    {}

    void start() {
        std::cout << "[ios-eval] Minimal eval server on port "
                  << acceptor_.local_endpoint().port() << std::endl;
        do_accept();

        // Run on separate thread with large stack
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 16 * 1024 * 1024);
        pthread_create(&thread_, &attr, [](void* arg) -> void* {
            auto* self = static_cast<minimal_eval_server*>(arg);
            self->io_context_.run();
            return nullptr;
        }, this);
        pthread_attr_destroy(&attr);
    }

    void stop() {
        io_context_.stop();
        pthread_join(thread_, nullptr);
    }

private:
    void do_accept() {
        acceptor_.async_accept([this](auto ec, tcp::socket socket) {
            if (!ec) {
                handle_connection(std::move(socket));
            }
            do_accept();
        });
    }

    void handle_connection(tcp::socket socket) {
        // Simple line-based protocol:
        // Client sends: code\n
        // Server sends: result\n\n  (double newline = end of response)

        boost::asio::streambuf buf;
        while (true) {
            boost::system::error_code ec;
            auto n = boost::asio::read_until(socket, buf, '\n', ec);
            if (ec) break;

            std::istream is(&buf);
            std::string code;
            std::getline(is, code);

            std::string response = eval_with_recovery(code);
            response += "\n\n";  // End marker

            boost::asio::write(socket, boost::asio::buffer(response), ec);
            if (ec) break;
        }
    }

    std::string eval_with_recovery(std::string const& code) {
        // Setup signal handler for crash recovery
        static sigjmp_buf jmp_buf;
        static volatile sig_atomic_t signal_received = 0;

        struct sigaction sa_new, sa_old_segv, sa_old_bus;
        sa_new.sa_handler = [](int sig) {
            signal_received = sig;
            siglongjmp(jmp_buf, sig);
        };
        sa_new.sa_flags = SA_ONSTACK;
        sigemptyset(&sa_new.sa_mask);

        sigaction(SIGSEGV, &sa_new, &sa_old_segv);
        sigaction(SIGBUS, &sa_new, &sa_old_bus);

        std::string result;
        int jmp_result = sigsetjmp(jmp_buf, 1);

        if (jmp_result == 0) {
            // Normal path
            try {
                auto obj = runtime::__rt_ctx->eval_string(code);
                result = runtime::to_std_string(runtime::to_code_string(obj));
            } catch (jtl::ref<error::base> const& e) {
                result = "Error: " + e->message;
            } catch (std::exception const& e) {
                result = "Error: " + std::string(e.what());
            }
        } else {
            // Recovered from crash
            result = "Error: Signal " + std::to_string(signal_received) +
                     " (stack overflow or segfault)";
        }

        sigaction(SIGSEGV, &sa_old_segv, nullptr);
        sigaction(SIGBUS, &sa_old_bus, nullptr);

        return result;
    }

    boost::asio::io_context io_context_;
    tcp::acceptor acceptor_;
    pthread_t thread_;
};

} // namespace jank::ios
```

### Editor Integration

For Emacs, create a simple adapter:

```elisp
;; ios-eval.el - Simple iOS eval integration
(defvar ios-eval-host "192.168.1.xxx")
(defvar ios-eval-port 5558)

(defun ios-eval-string (code)
  "Send CODE to iOS device for evaluation."
  (let ((proc (open-network-stream "ios-eval" "*ios-eval*"
                                   ios-eval-host ios-eval-port)))
    (process-send-string proc (concat code "\n"))
    ;; Read until double newline
    (with-current-buffer "*ios-eval*"
      (while (not (looking-at ".*\n\n"))
        (accept-process-output proc 1))
      (buffer-string))))

(defun ios-eval-last-sexp ()
  "Evaluate the sexp before point on iOS."
  (interactive)
  (let* ((bounds (bounds-of-thing-at-point 'sexp))
         (code (buffer-substring-no-properties (car bounds) (cdr bounds))))
    (message "%s" (ios-eval-string code))))

(global-set-key (kbd "C-c C-i") 'ios-eval-last-sexp)
```

### Pros
- **Simplest implementation** (~250 lines)
- **Avoids all nREPL complexity**
- **Direct eval_string path** - no middleware overhead
- **Signal recovery** for crash protection
- **Works with any editor** via TCP

### Cons
- No completion, lookup, eldoc (IDE features)
- No session management
- No output streaming (just final result)
- Custom editor integration needed

---

## Solution 2: Remote Proxy nREPL

### Concept

Run the full nREPL server on macOS, which handles all protocol complexity.
iOS runs only a minimal eval endpoint.

```
┌─────────────────────────────────────────────────────────────┐
│                        macOS                                 │
├─────────────────────────────────────────────────────────────┤
│  Editor (CIDER/Calva)                                       │
│       │                                                     │
│       ↓ nREPL Protocol                                      │
│  ┌─────────────────────────────────────────┐               │
│  │  Full nREPL Server (jank)               │               │
│  │  ─────────────────────────────────────  │               │
│  │  • All 18+ operations                   │               │
│  │  • Session management                   │               │
│  │  • Completion/lookup/eldoc              │               │
│  │  • Print middleware                     │               │
│  │                                         │               │
│  │  eval op intercept:                     │               │
│  │    code → Forward to iOS →              │───┐           │
│  │    result ← Receive from iOS ←          │   │           │
│  └─────────────────────────────────────────┘   │           │
└────────────────────────────────────────────────│───────────┘
                                                 │
                                                 │ TCP
                                                 │
┌────────────────────────────────────────────────│───────────┐
│                     iOS Device                 │            │
├────────────────────────────────────────────────│───────────┤
│                                                ↓            │
│  ┌─────────────────────────────────────────┐               │
│  │  Minimal Eval Endpoint                  │               │
│  │  (same as Solution 1)                   │               │
│  └─────────────────────────────────────────┘               │
└─────────────────────────────────────────────────────────────┘
```

### How It Works

1. Editor connects to nREPL on macOS (localhost:5555)
2. macOS nREPL handles completion, lookup, eldoc **locally** (using macOS jank)
3. For **eval** operations, macOS forwards code to iOS
4. iOS evals and returns result
5. macOS nREPL formats response for editor

### Implementation

**macOS side** - Modify nREPL eval handler:

```cpp
// In engine.hpp, handle_eval()
std::string eval_on_ios(std::string const& code) {
    // Connect to iOS device
    tcp::socket socket(io_context_);
    socket.connect(tcp::endpoint(
        boost::asio::ip::make_address(ios_device_ip_),
        ios_eval_port_
    ));

    // Send code
    boost::asio::write(socket, boost::asio::buffer(code + "\n"));

    // Read result (until double newline)
    boost::asio::streambuf buf;
    boost::asio::read_until(socket, buf, "\n\n");

    std::istream is(&buf);
    std::string result;
    std::getline(is, result);

    return result;
}

void handle_eval(message const& msg, connection& conn) {
    auto code = msg.get_string("code");

    if (forward_to_ios_) {
        // Forward eval to iOS device
        auto result = eval_on_ios(code);
        send_eval_response(conn, msg, result);
    } else {
        // Normal local eval
        // ... existing code ...
    }
}
```

**iOS side** - Same minimal eval server as Solution 1.

### Pros
- **Full IDE features** (completion, lookup, eldoc work on macOS)
- **Session management** handled by macOS
- **Standard nREPL protocol** - works with existing editors
- **No stack overflow** - complex operations run on macOS

### Cons
- **Requires macOS running** alongside iOS device
- **Network dependency** - latency between macOS and iOS
- **Completion uses macOS state** - may differ from iOS runtime
- **More complex setup** - two processes to manage

---

## Solution 3: Cross-Compile Hot Reload

### Concept

Inspired by jank's WASM hot-reload, but adapted for iOS:

1. macOS compiles jank code to C++
2. macOS cross-compiles C++ to iOS ARM64 object file
3. Send compiled object to iOS
4. iOS loads via dlopen (JIT mode only)

### Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                        macOS                                 │
├─────────────────────────────────────────────────────────────┤
│  jank Code "(defn foo [x] (+ x 1))"                        │
│       ↓                                                     │
│  jank Compiler                                              │
│       ↓                                                     │
│  C++ Code (with patch metadata)                             │
│       ↓                                                     │
│  Cross-compiler (clang -target arm64-apple-ios17.0)        │
│       ↓                                                     │
│  ARM64 Object File (patch_123.o)                           │
│       │                                                     │
│       └──────────── Network ─────────────────┐             │
└──────────────────────────────────────────────│─────────────┘
                                               │
┌──────────────────────────────────────────────│─────────────┐
│                     iOS Device               │              │
├──────────────────────────────────────────────│─────────────┤
│                                              ↓              │
│  ┌─────────────────────────────────────────┐               │
│  │  Hot Reload Loader                      │               │
│  │  ─────────────────────────────────────  │               │
│  │  1. Receive object file                 │               │
│  │  2. dlopen (JIT mode only!)             │               │
│  │  3. Call jank_patch_symbols()           │               │
│  │  4. Register symbols in var registry    │               │
│  └─────────────────────────────────────────┘               │
│                        ↓                                    │
│               jank Runtime                                  │
│               (vars updated with new functions)             │
└─────────────────────────────────────────────────────────────┘
```

### Critical Limitation: iOS Code Signing

**This approach has a MAJOR limitation:**

iOS does NOT allow loading unsigned code at runtime:
- `dlopen` for unsigned `.dylib`/`.o` files is **rejected**
- App Store apps cannot load dynamic code
- Only exception: **JIT entitlement** (granted by Xcode debugger)

**When connected to Xcode debugger:**
- JIT compilation is allowed
- But we're already using this for CppInterOp!
- So we can just use Solution 1 (direct eval via JIT)

**This solution only makes sense if:**
- You want faster eval (skip JIT compilation on iOS)
- You're willing to require Xcode connection anyway

### Pros
- **Faster eval** - iOS just loads pre-compiled code
- **More reliable** - compilation happens on powerful macOS

### Cons
- **Still requires Xcode connection** for dlopen to work
- **Complex infrastructure** - cross-compilation pipeline
- **Same limitation as JIT** - won't work without debugger
- **Overkill** - if JIT works, just use direct eval

---

## Comparison Matrix

| Aspect | Solution 1 (Minimal Eval) | Solution 2 (Proxy nREPL) | Solution 3 (Cross-Compile) |
|--------|---------------------------|--------------------------|---------------------------|
| **Complexity** | Low (~250 lines) | Medium (~500 lines) | High (~1000+ lines) |
| **IDE Features** | None | Full (on macOS) | None |
| **Latency** | Lowest | Medium | Medium |
| **Requirements** | iOS device only | macOS + iOS | macOS + iOS + cross-compiler |
| **Works without Xcode** | No (needs JIT) | No (iOS needs JIT) | No (needs dlopen) |
| **Stack Overflow Risk** | None | None | None |
| **Implementation Time** | 1-2 days | 3-5 days | 1-2 weeks |

---

## Recommendation

### For Immediate Use: Solution 1 (Minimal TCP Eval Server)

**Rationale:**
1. **Simplest to implement** - just ~250 lines of C++ code
2. **Directly addresses the problem** - bypasses all nREPL complexity
3. **Already have the pieces** - Boost.Asio is already in the codebase
4. **Works with current JIT** - eval_string is battle-tested

**Implementation steps:**
1. Create `minimal_eval_server.hpp` (based on code above)
2. Add to `sdf_viewer_ios.mm` after jank initialization
3. Create simple Emacs/VSCode integration
4. Test with iOS device connected to Xcode

### For Full IDE Experience: Solution 2 (Later)

If you need completion/lookup/eldoc later:
1. Implement Solution 1 first (as the eval backend)
2. Add proxy support to macOS nREPL server
3. Configure macOS to forward eval to iOS

---

## Implementation Plan

### Phase 1: Minimal Eval Server (1-2 days)

```
Day 1:
- [ ] Create minimal_eval_server.hpp
- [ ] Integrate into sdf_viewer_ios.mm
- [ ] Test basic eval via netcat

Day 2:
- [ ] Add signal recovery for crashes
- [ ] Create Emacs integration (ios-eval.el)
- [ ] Test with real jank code
```

### Phase 2: Editor Polish (Optional, 1 day)

```
- [ ] Add multi-line input support
- [ ] Add pretty-printed output
- [ ] Add namespace tracking
- [ ] VSCode extension
```

### Phase 3: Proxy nREPL (Optional, 3-5 days)

```
- [ ] Modify macOS nREPL to support remote eval
- [ ] Add device discovery (Bonjour?)
- [ ] Test with CIDER
```

---

## Appendix: Key Code Locations

| Component | Path |
|-----------|------|
| nREPL Server | `src/cpp/jank/nrepl_server/asio.cpp` |
| nREPL Engine | `include/cpp/jank/nrepl_server/engine.hpp` |
| eval_string | `src/cpp/jank/runtime/context.cpp:202` |
| Hot Reload | `include/cpp/jank/runtime/hot_reload.hpp` |
| iOS App | `/Users/pfeodrippe/dev/something/SdfViewerMobile/sdf_viewer_ios.mm` |

---

---

## Part 2: ClojureScript REPL Architecture Research

This section documents how ClojureScript (shadow-cljs, Figwheel) handles remote REPL evaluation,
which is directly applicable to jank on iOS.

### The ClojureScript REPL Model

**Key insight:** ClojureScript compilation happens on the JVM, but execution happens in a separate JavaScript runtime (browser, Node.js, React Native). This is exactly analogous to what we need for jank on iOS:

```
┌─────────────────────────────────────────────────────────────┐
│              ClojureScript Architecture                      │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│   JVM (Clojure)                    JavaScript Runtime       │
│   ─────────────                    ──────────────────       │
│                                                             │
│   Editor (CIDER)                                            │
│       │                                                     │
│       ↓ nREPL                                               │
│   Piggieback Middleware ──────────────────┐                │
│       │                                   │                 │
│       ↓                                   │                 │
│   ClojureScript Compiler                  │                 │
│   (cljs.analyzer → cljs.compiler)         │                 │
│       │                                   │                 │
│       ↓                                   │ WebSocket       │
│   JavaScript String  ─────────────────────┼──────►  Browser │
│                                           │         eval()  │
│                                           │           │     │
│   Result String  ◄────────────────────────┼───────────┘     │
│       │                                   │                 │
│       ↓                                   │                 │
│   nREPL Response                                            │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

**Mapped to jank on iOS:**

```
┌─────────────────────────────────────────────────────────────┐
│              jank iOS Architecture (Proposed)                │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│   macOS (jank)                         iOS Device           │
│   ────────────                         ──────────           │
│                                                             │
│   Editor (CIDER)                                            │
│       │                                                     │
│       ↓ nREPL                                               │
│   jank nREPL Server ──────────────────────┐                │
│       │                                   │                 │
│       ↓                                   │                 │
│   jank Compiler                           │                 │
│   (lex → parse → analyze → codegen)       │                 │
│       │                                   │                 │
│       ↓                                   │ TCP/WebSocket   │
│   C++ or eval instructions ───────────────┼──────►  iOS     │
│                                           │         JIT     │
│                                           │           │     │
│   Result String  ◄────────────────────────┼───────────┘     │
│       │                                   │                 │
│       ↓                                   │                 │
│   nREPL Response                                            │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### The IJavaScriptEnv Protocol

ClojureScript defines a clean protocol for evaluation environments:

```clojure
(defprotocol IJavaScriptEnv
  (-setup [repl-env opts] "initialize the environment")
  (-evaluate [repl-env filename line js] "evaluate a javascript string")
  (-load [repl-env provides url] "load code at url into the environment")
  (-tear-down [repl-env] "dispose of the environment"))
```

**This is the key abstraction!** The compiler doesn't know or care where code runs - it just calls `-evaluate` with compiled JavaScript.

**For jank, we could define:**

```cpp
// IJankEnv protocol (C++ equivalent)
class IJankEnv {
public:
    virtual void setup(options const& opts) = 0;
    virtual object_ref evaluate(std::string const& code) = 0;
    virtual void load(std::string const& module_path) = 0;
    virtual void tear_down() = 0;
};

// Local JIT implementation
class local_jit_env : public IJankEnv {
    object_ref evaluate(std::string const& code) override {
        return __rt_ctx->eval_string(code);
    }
};

// Remote iOS implementation
class remote_ios_env : public IJankEnv {
    object_ref evaluate(std::string const& code) override {
        // Send to iOS device, receive result
        return send_to_ios_and_wait(code);
    }
};
```

### shadow-cljs Message Protocol

shadow-cljs uses a relay architecture with EDN messages over WebSocket (transit encoded):

**Connection handshake:**
```clojure
;; Server → Client
{:op :welcome :client-id 123}

;; Client → Server (required)
{:op :hello :client-info {:type :browser :build-id :app}}
```

**REPL eval flow:**
```clojure
;; Server → Runtime (via relay)
{:op :cljs-eval
 :to runtime-id
 :input {:code "(+ 1 2)"
         :ns "user"
         :repl true}}

;; Runtime → Server
{:op :eval-result-ref
 :from runtime-id
 :oid "result-uuid"
 :warnings [...]}

;; Server requests actual value
{:op :obj-edn :to runtime-id :oid "result-uuid"}

;; Runtime → Server
{:op :obj-result :result "3"}
```

**Key observation:** The runtime receives SOURCE CODE (or compiled JavaScript), not machine code. Compilation happens on the powerful server, the runtime just evals the result.

### Browser Client Eval Implementation

In shadow-cljs browser client:

```clojure
;; Core eval function - just calls JavaScript's eval!
(defn script-eval [code]
  (js/goog.globalEval code))

;; Or for avoiding module scope issues
(defn global-eval [js]
  (if (not= "undefined" (js* "typeof(module)"))
    (js/eval js)
    (js* "(0,eval)(~{});" js)))
```

**The client-side is trivially simple!** It receives JavaScript strings and calls `eval()`.

### Figwheel WebSocket Protocol

Figwheel follows a similar pattern:

1. Server compiles ClojureScript → JavaScript
2. Sends JavaScript over WebSocket
3. Browser calls `goog.globalEval(code)`
4. Result sent back over WebSocket

```
Editor → nREPL → Piggieback → CLJS Compiler → WebSocket → Browser eval() → WebSocket → Result
```

### React Native / Krell

For mobile (React Native), the architecture is identical:
- Krell/re-natal compile on JVM
- Send JavaScript to React Native runtime
- Runtime evaluates via its JS engine

**This proves the model works for mobile development!**

---

## Part 3: Recommended Architecture for jank iOS

Based on the ClojureScript research, here's the recommended architecture:

### Option A: Full Proxy (Like Piggieback) - RECOMMENDED

```
┌─────────────────────────────────────────────────────────────┐
│                     macOS                                    │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  CIDER/Calva ──nREPL──► jank nREPL Server                  │
│                              │                              │
│                              ├─► completion (local)         │
│                              ├─► lookup (local)             │
│                              ├─► eldoc (local)              │
│                              │                              │
│                              └─► eval ──────┐               │
│                                             │               │
│                         ┌───────────────────┘               │
│                         │                                   │
│                         ↓                                   │
│                   ┌─────────────────┐                       │
│                   │ iOS Eval Router │                       │
│                   │ ─────────────── │                       │
│                   │ if ios_device:  │                       │
│                   │   forward(code) │──────┐                │
│                   │ else:           │      │                │
│                   │   local_eval()  │      │                │
│                   └─────────────────┘      │                │
│                                            │ TCP            │
└────────────────────────────────────────────│────────────────┘
                                             │
┌────────────────────────────────────────────│────────────────┐
│                     iOS Device             │                 │
├────────────────────────────────────────────│────────────────┤
│                                            ↓                 │
│   ┌────────────────────────────────────────────────┐        │
│   │  Minimal Eval Server (like browser client)     │        │
│   │  ──────────────────────────────────────────    │        │
│   │                                                │        │
│   │  while connected:                              │        │
│   │    code = receive()                            │        │
│   │    result = __rt_ctx->eval_string(code)        │        │
│   │    send(to_string(result))                     │        │
│   │                                                │        │
│   └────────────────────────────────────────────────┘        │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

**This is exactly what ClojureScript does!**
- Full IDE features (completion, lookup) work on macOS
- Only eval is forwarded to iOS
- iOS client is trivially simple (~100 lines)

### Option B: Direct Eval (Like Krell Simple Mode)

For simplest possible setup:

```
┌─────────────────────────────────────────────────────────────┐
│                     macOS                                    │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  Emacs/VSCode ──TCP──► Simple send(code) ───────┐          │
│                                                  │          │
└──────────────────────────────────────────────────│──────────┘
                                                   │
┌──────────────────────────────────────────────────│──────────┐
│                     iOS Device                   │           │
├──────────────────────────────────────────────────│──────────┤
│                                                  ↓           │
│   ┌────────────────────────────────────────────────┐        │
│   │  Minimal TCP Server                            │        │
│   │  code = read_line()                            │        │
│   │  result = eval_string(code)                    │        │
│   │  write(result)                                 │        │
│   └────────────────────────────────────────────────┘        │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

No nREPL complexity, no IDE features, just eval.

### Message Protocol (Inspired by shadow-cljs)

```cpp
// Message format (JSON or EDN)
struct eval_request {
    std::string op = "eval";
    std::string code;
    std::string ns = "user";
    uint64_t id;  // For matching responses
};

struct eval_response {
    std::string op = "result";
    uint64_t id;
    std::string value;       // Success case
    std::string error;       // Error case
    std::string out;         // Captured stdout
};

// Protocol flow
// 1. Client connects
// 2. Server sends: {"op": "welcome", "runtime": "ios", "version": "0.1"}
// 3. Client sends: {"op": "eval", "code": "(+ 1 2)", "id": 1}
// 4. Server sends: {"op": "result", "id": 1, "value": "3"}
```

### Why This Works (Avoiding Stack Overflow)

**The key insight from ClojureScript:**

1. **Compilation is separate from execution**
   - Heavy compilation happens on macOS (unlimited resources)
   - iOS just evaluates simple forms

2. **No complex initialization on target**
   - ClojureScript browser client is ~100 lines
   - No middleware, no sessions, no complex data structures
   - Just `eval()` and send result

3. **IDE features stay on host**
   - Completion, lookup, eldoc don't need the target
   - They can use the macOS jank instance

**For jank on iOS:**
- nREPL server with all its complexity runs on macOS
- iOS just needs: receive code → eval_string → send result
- No `get_in`, no middleware, no session management
- Stack overflow avoided!

---

## Implementation Plan (Revised)

### Phase 1: Minimal iOS Eval Client (Day 1)

```cpp
// ios_eval_client.hpp - ~150 lines
class ios_eval_client {
public:
    void start(uint16_t port);
    void stop();

private:
    void handle_connection(tcp::socket socket) {
        // Simple JSON protocol
        while (connected) {
            auto msg = read_json(socket);
            if (msg["op"] == "eval") {
                auto result = safe_eval(msg["code"]);
                send_json(socket, {
                    {"op", "result"},
                    {"id", msg["id"]},
                    {"value", result}
                });
            }
        }
    }

    std::string safe_eval(std::string const& code) {
        // With signal recovery
        try {
            auto obj = __rt_ctx->eval_string(code);
            return runtime::to_code_string(obj);
        } catch (...) {
            return "Error: ...";
        }
    }
};
```

### Phase 2: macOS Proxy Integration (Day 2-3)

Modify jank nREPL to support remote eval targets:

```clojure
;; In nREPL server config
{:port 5555
 :eval-target {:type :remote
               :host "192.168.1.xxx"  ; iOS device IP
               :port 5558}}
```

### Phase 3: Full Piggieback-style Integration (Day 4-5)

- Auto-discover iOS devices (Bonjour)
- Switch between local/remote eval
- Proper error propagation
- Output streaming

---

## References

- jank WASM hot-reload: `include/cpp/jank/runtime/hot_reload.hpp`
- nREPL protocol: https://nrepl.org/nrepl/design/overview.html
- iOS JIT entitlement: Requires debugger attachment or enterprise signing
- Boost.Asio: https://www.boost.org/doc/libs/release/doc/html/boost_asio.html

### ClojureScript References

- [Shadow CLJS User's Guide](https://shadow-cljs.github.io/docs/UsersGuide.html)
- [ClojureScript REPLs - Lambda Island](https://lambdaisland.com/guides/clojure-repls/clojurescript-repls)
- [ClojureScript REPL and Evaluation](https://clojurescript.org/reference/repl)
- [Piggieback - nREPL support for ClojureScript](https://github.com/nrepl/piggieback)
- [Weasel - WebSocket ClojureScript REPL](https://github.com/nrepl/weasel)
- [shadow-cljs REPL Implementation](https://github.com/thheller/shadow-cljs/blob/master/src/main/shadow/cljs/devtools/server/repl_impl.clj)
- [shadow-cljs Browser Client](https://github.com/thheller/shadow-cljs/blob/master/src/main/shadow/cljs/devtools/client/browser.cljs)
- [shadow-cljs Remote Protocol](https://github.com/thheller/shadow-cljs/blob/master/doc/remote.md)
- [Krell - React Native ClojureScript](https://github.com/vouch-opensource/krell)
- [ClojureScript + React Native](https://cljsrn.org/)

---

## Part 4: Implementation Summary (Completed)

This section documents the actual implementation that was completed.

### Files Created/Modified

| File | Description |
|------|-------------|
| `include/cpp/jank/ios/eval_server.hpp` | Minimal TCP eval server for iOS (~400 lines) |
| `include/cpp/jank/ios/eval_client.hpp` | macOS client for connecting to iOS eval server |
| `include/cpp/jank/nrepl_server/ios_remote_eval.hpp` | nREPL utility functions for remote eval |
| `include/cpp/jank/nrepl_server/ops/ios_eval.hpp` | nREPL operation handlers for iOS |
| `include/cpp/jank/nrepl_server/ops/describe.hpp` | Updated to advertise iOS ops |
| `include/cpp/jank/nrepl_server/ops/eval.hpp` | Modified to forward eval to iOS when connected |
| `include/cpp/jank/nrepl_server/engine.hpp` | Added iOS operation routing |
| `tools/ios-eval.el` | Emacs integration for iOS eval |
| `tools/ios-eval-cli.cpp` | CLI tool for testing iOS eval |

### Architecture Implemented

The final implementation follows the **Piggieback-style architecture** (Option A):

```
┌─────────────────────────────────────────────────────────────┐
│                     macOS                                    │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  CIDER/Calva ──nREPL──► jank nREPL Server                  │
│                              │                              │
│                              ├─► completion (local)         │
│                              ├─► lookup (local)             │
│                              ├─► eldoc (local)              │
│                              │                              │
│                              └─► eval ──────┐               │
│                                  │          │               │
│                          if is_remote_eval_active():        │
│                            forward to iOS                   │
│                          else:                              │
│                            local eval                       │
│                                             │ TCP (5558)    │
└─────────────────────────────────────────────│───────────────┘
                                              │
┌─────────────────────────────────────────────│───────────────┐
│                     iOS Device              │                │
├─────────────────────────────────────────────│───────────────┤
│                                             ↓                │
│   ┌─────────────────────────────────────────────────┐       │
│   │  iOS Eval Server (jank::ios::eval_server)       │       │
│   │  ───────────────────────────────────────────    │       │
│   │  • JSON-over-TCP protocol                       │       │
│   │  • {"op":"eval","id":1,"code":"...","ns":"..."}│       │
│   │  • Signal recovery for crash protection         │       │
│   │  • result = __rt_ctx->eval_string(code)         │       │
│   └─────────────────────────────────────────────────┘       │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

### Usage

**From nREPL client (CIDER, Calva, etc.):**

```clojure
;; Connect to iOS device
(nrepl {:op "ios-connect" :host "192.168.1.100" :port 5558})

;; Now all eval requests are forwarded to iOS!
(+ 1 2)  ; => 3 (executed on iOS!)

;; Check connection status
(nrepl {:op "ios-status"})

;; Disconnect
(nrepl {:op "ios-disconnect"})
```

**From Emacs (using ios-eval.el):**

```elisp
;; Load the integration
(load "/path/to/jank/compiler+runtime/tools/ios-eval.el")

;; Connect to iOS
M-x ios-eval-connect RET 192.168.1.100 RET

;; Eval commands
C-c C-i  ; Eval sexp at point
C-c C-d  ; Eval defun at point
C-c C-r  ; Eval region
C-c C-b  ; Eval buffer
```

**From command line (for testing):**

```bash
# Build and run the CLI tool
./ios-eval-cli 192.168.1.100 5558

# Simple REPL interface
ios> (+ 1 2)
=> 3
ios> (defn hello [] "world")
=> #'user/hello
ios> quit
```

### JSON Protocol

The iOS eval server uses a simple JSON protocol:

**Request:**
```json
{"op":"eval","id":1,"code":"(+ 1 2)","ns":"user"}
```

**Success Response:**
```json
{"op":"result","id":1,"value":"3"}
```

**Error Response:**
```json
{"op":"error","id":1,"error":"Divide by zero","type":"runtime"}
```

**Ping (for keepalive):**
```json
{"op":"ping","id":1}
```
→ `{"op":"pong","id":1}`

### Key Implementation Details

1. **Signal Recovery:** The iOS eval server uses `sigsetjmp`/`siglongjmp` to recover from crashes (SIGSEGV, SIGBUS, SIGABRT), allowing the server to continue after a crash.

2. **Non-blocking Timeout:** The macOS client uses non-blocking I/O with a configurable timeout (default 30s) to avoid hanging on slow or unresponsive iOS devices.

3. **Global State:** The remote eval target is stored as a global `unique_ptr` to avoid optional assignment issues with non-copyable socket types.

4. **Conditional Compilation:** All iOS-specific code is guarded with `#ifndef __EMSCRIPTEN__` to maintain WASM compatibility.

5. **Described Operations:** The iOS operations (`ios-connect`, `ios-disconnect`, `ios-status`) are advertised in the nREPL `describe` response so clients can discover them.

### Testing

1. Start jank on iOS with `JANK_IOS_JIT` defined
2. Note the iOS device IP address
3. On macOS, connect via nREPL:
   ```
   (nrepl {:op "ios-connect" :host "<ios-ip>"})
   ```
4. Evaluate code - it runs on iOS!

### Future Improvements

- [ ] Bonjour/mDNS device discovery
- [ ] Multiple iOS device support
- [ ] Output streaming (stdout/stderr)
- [ ] Namespace synchronization
- [ ] File loading support
- [ ] Source map support for stack traces
