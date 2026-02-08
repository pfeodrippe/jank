# Cross-Compilation and Persistent Clang Server Research

## Date
2025-12-26

## Summary
Comprehensive research on cross-compilation approaches for Clang/LLVM, persistent compilation servers, and remote JIT execution. This research explores approaches for enabling jank's nREPL to compile code on a host machine (e.g., macOS x86_64) while executing on a different target (e.g., iOS ARM64).

## Research Topics

### 1. CppInterOp Cross-Compilation

**Findings:**
- CppInterOp is built on top of clang-repl, which uses LLVM's ORC JIT infrastructure
- Clang/LLVM is natively a cross-compiler - can compile to all targets by setting the `-target` option
- The basic approach is to use `-target <triple>` where triple has format `<arch><sub>-<vendor>-<sys>-<env>`
- For iOS ARM64: `arm64-apple-ios<version>` (e.g., `arm64-apple-ios15.5`)
- CppInterOp itself doesn't have specific documentation about cross-compilation to different targets
- clang-repl's JIT execution targets the underlying device architecture by default

**Key Quote:**
> "Clang-REPL is an interpreter that allows for incremental compilation. It supports interactive programming for C++ in a read-evaluate-print-loop (REPL) style. It uses Clang as a library to compile the high level programming language into LLVM IR. Then the LLVM IR is executed by the LLVM just-in-time (JIT) infrastructure."

**Limitation:**
The standard clang-repl approach assumes compilation and execution happen on the same architecture. For true cross-compilation with execution on a different device, you need out-of-process execution.

**Sources:**
- [Cross-compilation using Clang Documentation](https://clang.llvm.org/docs/CrossCompilation.html)
- [Clang-Repl Documentation](https://clang.llvm.org/docs/ClangRepl.html)
- [CppInterOp GitHub](https://github.com/compiler-research/CppInterOp)

---

### 2. Clang CompilerInstance Cross-Compilation for iOS ARM64

**Findings:**
- For iOS ARM64, the target triple is `arm64-apple-ios<version>` (NOT `aarch64-apple-ios`)
- Must use the version of clang bundled with Xcode.app for iOS compilation
- Required flags:
  - `-target arm64-apple-ios15.5` (or appropriate iOS version)
  - `-isysroot /Applications/Xcode.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk`
  - `-fembed-bitcode-marker` (optional, for bitcode)

**CMake Configuration:**
```cmake
CMAKE_{C,CXX}_COMPILER_TARGET # Sets --target argument to clang
CMAKE_SYSROOT # Path to sysroot with headers/libraries for target
LLVM_HOST_TRIPLE # Target triple of system the built LLVM will run on
```

**Example iOS Build:**
```bash
cmake -G "Ninja" \
  -DCMAKE_OSX_ARCHITECTURES="armv7;armv7s;arm64" \
  -DCMAKE_TOOLCHAIN_FILE=<PATH_TO_LLVM>/cmake/platforms/iOS.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_BUILD_RUNTIME=Off \
  -DLLVM_INCLUDE_TESTS=Off \
  -DLLVM_INCLUDE_EXAMPLES=Off \
  -DLLVM_ENABLE_BACKTRACES=Off \
  <PATH_TO_LLVM>
```

**Key Quote:**
> "The second part to the cross compiling is to provide a sysroot - a place where clang can find the headers, libraries for the target platform."

**Sources:**
- [Cross-compilation using Clang](https://clang.llvm.org/docs/CrossCompilation.html)
- [Cross Compiling C Library for iOS and macOS](https://branchout.dev/c/macos/iphoneos/2022/09/05/cross_compiling_c_for_macos_and_ios.html)
- [How to cross-compile Clang/LLVM](https://llvm.org/docs/HowToCrossCompileLLVM.html)

---

### 3. Persistent Clang Compilation Server Approaches

**clangd Architecture:**

clangd is the most successful persistent Clang compilation server, designed as a Language Server Protocol (LSP) implementation.

**Key Architecture Details:**
- Runs clang parser in a loop for each open file
- **TUScheduler** manages a collection of **ASTWorkers**, each running on its own thread
- Maintains persistent AST for answering queries (go-to-definition, completion, etc.)
- Uses **compilation database** (compile_commands.json) to determine compile commands
- Implements background indexing across the entire project
- Supports remote index system via RPC server

**Performance Optimizations:**
- Skips parsing function bodies in included headers for faster response
- Caches parsed results per-file
- Uses HeaderIncluderCache for header-to-source mapping

**Key Quote:**
> "Clangd is based on the clang compiler, and at its core runs the clang parser in a loop. The parser produces diagnostics as it encounters problems, and the end result is a clang AST. The AST is saved to answer queries like 'what kind of symbol is under the cursor'."

**Alternative Designs:**

1. **Clang Service Proposal** (chandlerc/llvm-designs)
   - Historical design proposal for a persistent caching service
   - Combines libclang/ASTUnit translation unit caching with lib/Tooling
   - Restartable, long-lived background process
   - IPC protocol for command line tools to communicate with service
   - **Status:** Design proposal, not fully implemented

2. **clang-cache** (sas/clang-cache)
   - Caching daemon for clang compilation
   - Similar concept to ccache but as a daemon

3. **clang-server** (annulen/clang-server)
   - Compilation server for clang
   - Minimal documentation available

**Fallback Behavior:**
> "If no compilation database is found, a very simple command like clang foo.cc is used. For a real project this will often fail to find #includes, but it allows clangd to work on toy examples without configuration."

**Sources:**
- [clangd Design Documentation](https://clangd.llvm.org/design/)
- [Clang Service Design Proposal](https://github.com/chandlerc/llvm-designs/blob/master/ClangService.rst)
- [clang-cache GitHub](https://github.com/sas/clang-cache)
- [clang-server GitHub](https://github.com/annulen/clang-server)

---

### 4. LLVM ORC JIT Cross-Compilation

**Key Capabilities:**

LLVM ORC JIT supports **true cross-process and cross-architecture** JIT compilation:

> "ORC provides APIs to link relocatable object files (COFF, ELF, MachO) into a target process at runtime. The target process may be the same process that contains the JIT session object and jit-linker, or may be another process (even one running on a different machine or architecture) that communicates with the JIT via RPC."

**JITLink Features:**
- Supports cross-process and cross-architecture linking
- Links relocatable objects into a target executor process
- Memory manager handles transferring memory to target address space
- Executor process can be:
  - Same process (in-process JIT)
  - Different process on same machine (out-of-process JIT)
  - Different machine/architecture (remote JIT)

**Cross-Compilation Use Case:**
> "The LLVM debugger, LLDB, uses a cross-compiling JIT for expression evaluation. In this use case, cross compilation allows expressions compiled in the debugger process to be executed on the debug target process, which may be on a different device/architecture."

**ORC RPC:**
- ORC-RPC-based implementation enables out-of-process JITing via file descriptors/sockets
- MapperJITLinkMemoryManager can use shared memory or ORC-RPC communication
- Communication channels: Unix sockets, TCP, file descriptors

**Format/Architecture Support:**
- MachO and ELF have better support than COFF
- Can specify multiple targets at build time: `-DLLVM_TARGETS_TO_BUILD="host;NVPTX"`

**Security Benefits:**
> "JITLink's ability to link JIT'd code for a separate executor process can be used to improve the security of a JIT system: The executor process can be sandboxed, run within a VM, or even run on a fully separate machine."

**Testing Cross-Architecture:**
The `-noexec` option in llvm-jitlink allows linking for other targets without executing:
> "The -noexec option tells llvm-jitlink to stop after looking up the entry point, and before attempting to execute it. Since the linked code is not executed, this can be used to link for other targets even if you do not have access to the target being linked."

**Sources:**
- [ORC Design and Implementation](https://llvm.org/docs/ORCv2.html)
- [JITLink and ORC's ObjectLinkingLayer](https://llvm.org/docs/JITLink.html)

---

### 5. Out-of-Process Execution for Clang-Repl (GSoC 2024)

**Major Development:**

In 2024, Google Summer of Code project added **out-of-process execution** to clang-repl, enabling exactly the kind of remote execution we need.

**New Flags:**
- `--oop-executor=path/to/llvm-jitlink-executor` - Starts separate JIT executor process
- `--oop-executor-connect=<address>` - Connects to existing executor process

**Usage Example:**
```bash
clang-repl --oop-executor=path/to/llvm-jitlink-executor \
           --orc-runtime=path/to/liborc_rt.a
```

**Communication Modes:**
- Pipe-based communication (default with --oop-executor)
- Socket-based communication (with --oop-executor-connect)

**Platform Support:**
- Supports ELF (Linux) and Mach-O (macOS/iOS)
- Only works on Unix platforms
- Does NOT support COFF (Windows)

**Benefits:**
> "By executing user code in a separate process, resource usage is reduced and crashes no longer affect the main session. This solution significantly enhances both the efficiency and stability of Clang-Repl, making it more reliable and suitable for a broader range of use cases, especially on resource-constrained systems."

**Implementation Details:**
- Uses llvm-jitlink-executor as the execution process
- Leverages ORC JIT's remote execution capabilities
- Requires building: `clang`, `clang-repl`, `llvm-jitlink-executor`, `orc_rt` runtime

**Key Quote:**
> "To enable OOP execution in Clang-Repl, the llvm-jitlink-executor is utilized, allowing Clang-Repl to offload code execution to a dedicated executor process. This setup introduces a layer of isolation between Clang-Repl's main process and the code execution environment."

**Sources:**
- [GSoC 2024: Out-Of-Process Execution For Clang-Repl](https://blog.llvm.org/posts/2024-10-23-out-of-process-execution-for-clang-repl/)
- [Out Of Process execution for Clang-Repl](https://compiler-research.org/blogs/gsoc24_sahil_introduction_blog/)
- [Wrapping Up GSoC 2024: Out-Of-Process Execution for Clang-REPL](https://compiler-research.org/blogs/gsoc24_sahil_wrapup_blog/)

---

### 6. ez-clang: Remote REPL for Embedded Devices

**Project Overview:**

ez-clang demonstrates a working implementation of remote C++ REPL execution on embedded devices with extremely limited resources.

**GitHub:** https://github.com/echtzeit-dev/ez-clang

**Architecture:**
- **Host side:** Clang frontend + JIT backend (ORCv2 + JITLink)
- **Device side:** Minimal firmware stub
- **Communication:** Serial connection
- **Compilation model:** All compilation happens on host, only execution on device

**Supported Hardware:**
- Arduino Due
- Raspberry Pi 4
- Other bare-metal embedded devices

**Key Difference from MicroPython:**
> "Unlike MicroPython which uses a reduced Python dialect and runs an interpreter on the device, ez-clang uses standard C++ and is compiled with the toolchain on the host, running only a minimal stub on the device."

**Implementation Approach:**
- Compiled execution model (not interpreted)
- Toolchain runs on host machine
- Minimal stub on device for value reporting
- Uses `__ez_clang_report_value()` for REPL to print expression values

**Related Projects:**
- ez-clang-pycfg: Python device configuration layer
- ez-clang-linux: Socket-based remote hosts on Linux (for less-constrained devices)
- ez-clang-arduino: Arduino firmware reference implementation

**Presentation:**
Presented at European LLVM Developers' Meeting in May 2022

**Key Insight:**
This demonstrates that remote compilation + execution is a proven approach for resource-constrained targets, which directly applies to iOS JIT restrictions.

**Sources:**
- [ez-clang GitHub](https://github.com/echtzeit-dev/ez-clang)
- [ez-clang Project Page](https://echtzeit.dev/ez-clang/)
- [Remote C++ live coding on Raspberry Pi](https://weliveindetail.github.io/blog/post/2023/02/06/ez-clang-linux.html)

---

### 7. iOS JIT Limitations and Workarounds

**The Problem:**

> "Apps should NOT be expected to work on real iPhone due to iOS security preventing Just-In-Time (JIT) Execution. By pulling out the device crash logs, the reason turns out to be the fact the code generated in-memory by LLVM/Clang wasn't signed and so the app was terminated with SIGTERM CODESIGN."

**iOS Security Constraints:**
- Cannot execute unsigned JIT code on physical devices
- No `fork()` system call support
- Cannot create new processes on iOS
- Even if you have a compiler for iOS, no way to run it as separate process

**Possible Workarounds:**

1. **AOT Compilation:**
   > "To make the app work on real iPhone untethered from Xcode, one possibility is to use compilation into binary, somehow sign it and use system()."

2. **LLVM Bytecode Interpreter:**
   > "Another possibility would be to use the slower LLVM bytecode interpreter instead of ORC JIT."

3. **Remote Compilation (ez-clang approach):**
   - Compile on host machine (macOS)
   - Transfer compiled code to device
   - Execute on device
   - This is what ez-clang demonstrates for embedded devices

4. **Hybrid AOT + Remote JIT:**
   - Pre-compile core runtime with AOT
   - Use remote compilation server for REPL/hot-reload
   - Transfer compiled object files to device
   - Link dynamically on device

**iOS Simulator:**
The iOS simulator does NOT have the same JIT restrictions as physical devices, making it useful for development but not representative of production constraints.

**Sources:**
- [LLVM-On-iOS GitHub](https://github.com/light-tech/LLVM-On-iOS)
- [How to build clang and libc++ for iOS](https://discourse.llvm.org/t/how-to-build-clang-and-libc-for-macos-and-ios-devices/70134)

---

## Recommended Approaches for jank

Based on this research, here are recommended approaches for jank's cross-compilation needs:

### Approach 1: Out-of-Process Clang-Repl (Most Promising)

**Description:**
Use clang-repl's new `--oop-executor` flag to run compilation on host and execution on device.

**Advantages:**
- Official LLVM feature (GSoC 2024)
- Actively maintained
- Supports socket-based communication
- Can work across different architectures
- Works with CppInterOp

**Disadvantages:**
- Requires LLVM 18+ (with GSoC 2024 patches)
- Only supports Unix platforms (ELF/Mach-O)
- May need iOS-specific executor implementation

**Implementation Steps:**
1. Build LLVM with clang-repl, llvm-jitlink-executor, and orc_rt
2. Set up socket communication between host and iOS device
3. Configure target triple for iOS ARM64 on host
4. Use `--oop-executor-connect` to connect to executor on iOS
5. Integrate with CppInterOp's nREPL interface

### Approach 2: Custom Remote JIT using ORC APIs (More Control)

**Description:**
Implement custom remote JIT using LLVM's ORC RPC APIs directly, similar to what LLDB does.

**Advantages:**
- Full control over compilation and execution
- Can optimize for jank's specific needs
- Can customize memory management
- Proven approach (LLDB uses this)

**Disadvantages:**
- More implementation work
- Need to maintain custom code
- More complex to debug

**Implementation Steps:**
1. Use ORC JIT APIs to create ExecutionSession on host
2. Implement TargetProcessControl for iOS device communication
3. Use MapperJITLinkMemoryManager for cross-process memory
4. Set up ORC-RPC communication via sockets
5. Configure target triple for iOS ARM64

### Approach 3: ez-clang-inspired Custom Server (Simplest)

**Description:**
Create a simple compilation server inspired by ez-clang's architecture.

**Advantages:**
- Proven approach for resource-constrained devices
- Simple architecture: compile on host, execute on device
- Can use existing jank compilation infrastructure
- Easier to debug

**Disadvantages:**
- Need to handle serialization of compilation results
- May be slower due to serialization overhead
- Need to implement custom protocol

**Implementation Steps:**
1. Create compilation server on host (macOS)
2. Accept jank code over socket
3. Compile to object file with iOS ARM64 target
4. Send compiled object to device
5. Device loads and executes

### Approach 4: Hybrid AOT + Remote JIT

**Description:**
Combine AOT for core libraries with remote JIT for REPL/hot-reload.

**Advantages:**
- Fast startup (AOT for core)
- Interactive development (remote JIT for REPL)
- Works on physical iOS devices
- Can sign AOT libraries

**Disadvantages:**
- Complex build system
- Need to maintain both AOT and JIT paths
- May have namespace/symbol resolution issues

---

## Key Technical Insights

### 1. Target Triple Configuration

For iOS ARM64 cross-compilation from macOS:
```cpp
// Set these in CompilerInstance
triple = "arm64-apple-ios15.5"  // Use arm64, not aarch64
sysroot = "/Applications/Xcode.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk"
```

### 2. ORC JIT Cross-Process Setup

```cpp
// Pseudo-code for ORC cross-process JIT
auto ES = std::make_unique<ExecutionSession>();
auto TPC = createRemoteTargetProcessControl(socketFD);
auto DL = TPC->getDataLayout();
auto JTMB = JITTargetMachineBuilder("arm64-apple-ios15.5");
```

### 3. Clang-Repl OOP Usage

```bash
# On host (macOS)
clang-repl --oop-executor-connect=ios-device.local:12345 \
           --orc-runtime=path/to/liborc_rt.a

# On device (iOS)
llvm-jitlink-executor listen=*:12345
```

---

## Open Questions

1. **Does CppInterOp work with clang-repl's --oop-executor flag?**
   - Need to test if CppInterOp can be configured to use out-of-process execution
   - May need to modify CppInterOp initialization

2. **Can llvm-jitlink-executor run on iOS?**
   - Need to verify if it can be built for iOS ARM64
   - May need special entitlements or signing

3. **What's the performance overhead of remote JIT?**
   - Network latency for code transfer
   - Serialization/deserialization overhead
   - Memory transfer costs

4. **How to handle PCH/modules in cross-compilation?**
   - PCH needs to be target-specific
   - May need to generate separate PCH for iOS target
   - How to share type information across host/device boundary

5. **Can we use shared memory instead of sockets for simulator?**
   - iOS simulator runs on same machine
   - Shared memory would be faster than sockets
   - ORC MapperJITLinkMemoryManager supports shared memory

---

## Next Steps

1. **Test clang-repl --oop-executor locally**
   - Build LLVM with OOP support
   - Test with simple examples on macOS
   - Verify cross-architecture works (x86_64 -> arm64)

2. **Prototype iOS executor**
   - Build llvm-jitlink-executor for iOS
   - Test socket communication
   - Verify code execution works

3. **Integrate with CppInterOp**
   - Modify jank's nREPL to use OOP mode
   - Configure target triple for iOS
   - Test with jank REPL session

4. **Benchmark performance**
   - Measure compilation time
   - Measure execution time
   - Compare with local JIT
   - Identify bottlenecks

---

## References

### Documentation
- [Cross-compilation using Clang](https://clang.llvm.org/docs/CrossCompilation.html)
- [Clang-Repl Documentation](https://clang.llvm.org/docs/ClangRepl.html)
- [ORC Design and Implementation](https://llvm.org/docs/ORCv2.html)
- [JITLink and ORC's ObjectLinkingLayer](https://llvm.org/docs/JITLink.html)
- [clangd Design Documentation](https://clangd.llvm.org/design/)
- [How to cross-compile LLVM](https://llvm.org/docs/HowToCrossCompileLLVM.html)

### Blog Posts and Articles
- [GSoC 2024: Out-Of-Process Execution For Clang-Repl](https://blog.llvm.org/posts/2024-10-23-out-of-process-execution-for-clang-repl/)
- [Cross Compiling C Library for iOS and macOS](https://branchout.dev/c/macos/iphoneos/2022/09/05/cross_compiling_c_for_macos_and_ios.html)
- [Remote C++ live coding on Raspberry Pi](https://weliveindetail.github.io/blog/post/2023/02/06/ez-clang-linux.html)
- [Cross compiling made easy, using Clang and LLVM](https://mcilloni.ovh/2021/02/09/cxx-cross-clang/)

### GitHub Projects
- [CppInterOp](https://github.com/compiler-research/CppInterOp)
- [ez-clang](https://github.com/echtzeit-dev/ez-clang)
- [Clang Service Design](https://github.com/chandlerc/llvm-designs/blob/master/ClangService.rst)
- [clang-cache](https://github.com/sas/clang-cache)
- [clang-server](https://github.com/annulen/clang-server)
- [LLVM-On-iOS](https://github.com/light-tech/LLVM-On-iOS)

### LLVM Mailing List Discussions
- [arm64-apple-ios vs aarch64-apple-ios](https://groups.google.com/g/llvm-dev/c/PIBNR1EE9R0)

---

## Conclusion

The research reveals that **out-of-process JIT execution is not only possible but already implemented** in LLVM through:

1. **ORC JIT's cross-process capabilities** - The foundation for remote execution
2. **Clang-repl's --oop-executor flag** (GSoC 2024) - Ready-to-use implementation
3. **Proven real-world uses** - LLDB (cross-compilation), ez-clang (embedded devices)

For jank's iOS development, the most promising approach is to leverage clang-repl's new out-of-process execution capabilities, potentially with a custom executor implementation optimized for iOS. This would enable:

- Compilation on macOS (x86_64 or ARM64)
- Execution on iOS device or simulator (ARM64)
- Interactive REPL experience
- Hot-reload capabilities
- Works around iOS JIT restrictions

The key technical challenge will be integrating this with CppInterOp and handling the cross-process type information and symbol resolution that jank's nREPL requires.
