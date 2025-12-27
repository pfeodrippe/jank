# Dynamic Namespace Synchronization Issue - iOS Remote Compile

## Problem Statement

When using the iOS JIT-only mode with remote compilation, dynamically created namespaces and aliases are not synchronized between the compile server (macOS) and the iOS runtime.

### Reproduction

```clojure
;; Step 1: Create a namespace with a var
(ns my.test.namespace)
(def x 42)
;; => Works - creates namespace on compile server, sends compiled code to iOS

;; Step 2: Try to require with alias from user namespace
(in-ns 'user)
(require '[my.test.namespace :as mtn])
;; => Works - compiles and iOS executes, registering alias on iOS

;; Step 3: Try to use the alias
mtn/x
;; => ERROR: analyze/unresolved-symbol: Unable to resolve symbol 'mtn/x'
```

### Root Cause Analysis

The issue is a **split-brain problem** between two separate runtimes:

1. **Compile Server (macOS)** - Where code analysis and compilation happens
2. **iOS Runtime** - Where code execution happens

When `(require '[my.test.namespace :as mtn])` is processed:

1. Compile server **compiles** the require call to object code
2. Object code is sent to iOS
3. iOS **executes** the code, which:
   - Calls `require` function
   - Registers alias `mtn -> my.test.namespace` in iOS's `user` namespace

But the compile server **never executes** the require - it only compiles it. So the alias `mtn` is only registered on iOS, not on the compile server.

For subsequent compilation of `mtn/x`:
1. Compile server tries to analyze `mtn/x`
2. Looks up alias `mtn` in `user` namespace - **NOT FOUND** (it's only on iOS)
3. Error: Unable to resolve symbol

### Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                     nREPL Client (Emacs/VSCode)                 │
└───────────────────────────────┬─────────────────────────────────┘
                                │ eval: (require '[foo :as f])
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│                    iOS Runtime (port 5558)                      │
│  ┌─────────────────────────────────────────────────────────────┐│
│  │ nREPL Server                                                ││
│  │  - Receives code                                            ││
│  │  - Sends to compile server                                  ││
│  │  - Executes returned object                                 ││
│  └──────────────────────┬──────────────────────────────────────┘│
│                         │                                       │
│  ┌──────────────────────▼──────────────────────────────────────┐│
│  │ Namespace State (EXECUTION SIDE)                            ││
│  │  - user: {aliases: {f -> foo}}  ← Alias registered here!    ││
│  └─────────────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────────┘
                                │
                                │ compile request
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│              Compile Server (macOS, port 5570)                  │
│  ┌─────────────────────────────────────────────────────────────┐│
│  │ Namespace State (COMPILE SIDE)                              ││
│  │  - user: {aliases: {}}  ← NO alias! Out of sync!            ││
│  └─────────────────────────────────────────────────────────────┘│
│                                                                 │
│  When analyzing f/bar:                                          │
│  1. Look up alias 'f' in user namespace                         │
│  2. Not found → ERROR                                           │
└─────────────────────────────────────────────────────────────────┘
```

## Affected Forms

These forms modify namespace state and need synchronization:

| Form | Effect | Priority |
|------|--------|----------|
| `require` | Loads namespace, adds alias/refers | HIGH |
| `in-ns` | Switches/creates namespace | HIGH |
| `ns` | Already handled (creates ns, processes requires) | DONE |
| `use` | Like require + refer :all | MEDIUM |
| `refer` | Adds refers to current namespace | MEDIUM |
| `alias` | Adds namespace alias | MEDIUM |
| `refer-clojure` | Refers from clojure.core | LOW |
| `import` | Imports Java/native classes | LOW |
| `create-ns` | Creates a namespace | LOW |

## Solution Options

### Option A: Execute Namespace-Affecting Forms on Compile Server

**Approach**: Before compiling code, detect forms that affect namespace state and execute them on the compile server too.

**Pros**:
- Keeps both runtimes in sync automatically
- No protocol changes needed
- Works for all namespace-affecting forms

**Cons**:
- Need to identify all namespace-affecting forms
- Some forms have side effects (loading files, etc.)
- `require` on compile server will try to load source files

**Implementation**:
```cpp
// In compile_code(), before analyzing:
for(auto const &form : all_forms)
{
  if(is_namespace_affecting_form(form))
  {
    // Execute on compile server to register aliases/refers
    runtime::__rt_ctx->eval(form);
  }
}
// Then analyze all forms
```

### Option B: Sync Namespace State from iOS Back to Server

**Approach**: After iOS executes code, send namespace state changes back.

**Pros**:
- Accurate reflection of iOS state
- No need to re-execute forms

**Cons**:
- Requires protocol changes
- Bidirectional communication needed
- Latency for sync

### Option C: Include Namespace State in Compile Requests

**Approach**: iOS sends its current namespace aliases/refers with each compile request.

**Pros**:
- Server always has current state
- No execution on server side

**Cons**:
- Larger request payloads
- Need to serialize namespace state
- Protocol changes needed

## Recommended Solution: Option A with Refinements

Execute namespace-affecting forms on the compile server, but with careful handling:

1. **Detect** forms that affect namespace state:
   - `(require ...)` - top-level call to require
   - `(in-ns ...)` - namespace switch
   - `(use ...)` - require + refer
   - `(refer ...)` - add refers
   - `(alias ...)` - add alias

2. **Execute** them on the compile server before analyzing:
   - This registers aliases/refers in the compile server's namespace
   - The forms are still compiled and sent to iOS for execution there too

3. **Handle edge cases**:
   - Source-only namespaces: Compile server may not have source for dynamically created namespaces
   - Need to handle the case where `require` can't find the source (namespace was created via REPL)

### Implementation Plan

1. Create helper `is_namespace_affecting_form()` to detect relevant forms
2. In `compile_code()`, iterate forms and execute namespace-affecting ones
3. Handle exceptions gracefully (namespace might already exist, source might not exist)
4. Test with:
   - Dynamic namespace creation
   - Require with alias
   - In-ns switching
   - Nested requires

### Key Insight

For dynamically created namespaces (via REPL), the compile server already has the namespace in memory because we execute the `ns` form. The issue is specifically with **aliases** created by `require`.

When we do:
```clojure
(ns my.test.namespace)  ;; Compile server executes this, has the namespace
(def x 42)              ;; Compile server compiles, var exists on both sides

(in-ns 'user)
(require '[my.test.namespace :as mtn])  ;; Only compiled, not executed on server!
mtn/x  ;; ERROR - alias not on server
```

The namespace `my.test.namespace` exists on both sides. Only the alias is missing.

## Implemented Fix

The fix was implemented in `compiler+runtime/include/cpp/jank/compile_server/server.hpp`:

### 1. Added `is_namespace_affecting_form()` helper (after `is_ns_form()`):

```cpp
static bool is_namespace_affecting_form(runtime::object_ref form)
{
  auto const list = runtime::dyn_cast<runtime::obj::persistent_list>(form);
  if(list.is_nil() || list->data.empty())
  {
    return false;
  }
  auto const first = list->data.first();
  if(first.is_none())
  {
    return false;
  }
  auto const sym = runtime::dyn_cast<runtime::obj::symbol>(first.unwrap());
  if(sym.is_nil())
  {
    return false;
  }
  return sym->name == "require" || sym->name == "use" ||
         sym->name == "refer" || sym->name == "alias" ||
         sym->name == "in-ns";
}
```

### 2. Added Step 3 in `compile_code()` to execute namespace-affecting forms:

After parsing and before analysis, execute any namespace-affecting forms on the compile server:

- Handles `require`, `use`, `refer`, `alias`, `in-ns`
- For `in-ns`, also updates `eval_ns` to the new namespace
- Catches exceptions gracefully (namespace source might not exist for REPL-created namespaces)

### Test Results

```clojure
;; Create dynamic namespace
(ns my.dynamic.namespace)
(def foo 123)
;; => #'my.dynamic.namespace/foo

;; Switch back to user namespace
(in-ns 'user)
;; => nil

;; Require with alias
(require '[my.dynamic.namespace :as mdn])
;; => nil

;; Use the alias - NOW WORKS!
mdn/foo
;; => 123

;; Inline case also works
(in-ns 'user) (require '[my.dynamic.namespace :as mdn2]) mdn2/foo
;; => 123
```

### Compile Server Logs

```
[compile-server] Compiling code (id=3) in ns=vybe.sdf.abc: (in-ns 'vybe.sdf.ui) (require '[vybe.sdf.abc :as a...
[compile-server] Executing namespace-affecting form on server...
[compile-server] Switched to namespace via in-ns: vybe.sdf.ui
[compile-server] Executing namespace-affecting form on server...

[compile-server] Compiling code (id=4) in ns=vybe.sdf.ui: (abc/get-greeting)
```

Key log lines:
- `Executing namespace-affecting form on server...` - shows forms being executed
- `Switched to namespace via in-ns: vybe.sdf.ui` - shows the in-ns correctly updates the namespace context

### Bug Fix: in-ns Return Value

Initial implementation checked if `in-ns` returns the namespace, but `in-ns` returns `nil`.
Fixed by extracting the namespace name from the form arguments:

```cpp
// Get the namespace name from the second argument: (in-ns 'ns-name)
auto const second = list->data.rest().first();
// The argument could be 'ns-name (which is (quote ns-name)) or just ns-name
auto ns_sym = runtime::dyn_cast<runtime::obj::symbol>(second.unwrap());
if(ns_sym.is_nil())
{
  // Check if it's a quoted symbol
  auto const quote_list = runtime::dyn_cast<runtime::obj::persistent_list>(second.unwrap());
  if(!quote_list.is_nil() && runtime::sequence_length(quote_list) >= 2)
  {
    ns_sym = runtime::dyn_cast<runtime::obj::symbol>(quote_list->data.rest().first().unwrap());
  }
}
// Look up the namespace by name
auto const new_ns = runtime::__rt_ctx->find_ns(ns_sym);
```

### Cross-Namespace Usage Test

Successfully tested using a dynamically created namespace from another namespace:

```clojure
;; Create dynamic namespace
(ns vybe.sdf.abc)
(defn get-greeting [] "Hello from dynamic namespace!")

;; Use it from vybe.sdf.ui
(in-ns 'vybe.sdf.ui)
(require '[vybe.sdf.abc :as abc])
(abc/get-greeting)
;; => "Hello from dynamic namespace!"

;; Works in subsequent requests too
(println "Testing:" (abc/get-greeting))
;; Testing: Hello from dynamic namespace!
```
