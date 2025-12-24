# Deep Investigation: CppInterOp Extern Symbol Duplication

## Executive Summary

When CppInterOp (and clang's incremental compiler) parses headers containing `extern` declarations and inline functions, it can create **duplicate symbol definitions** at different memory addresses. This causes the classic "split-brain" bug where AOT and JIT code operate on different copies of global variables.

## Investigation Findings

### 1. The LLVM IR Generation Path

When code is parsed by clang's incremental compiler, the flow is:

1. **IncrementalParser::Parse()** - parses source code to AST
2. **HandleTopLevelDecl()** - for each declaration, calls CodeGen
3. **CodeGenModule::EmitTopLevelDecl()** - dispatches based on decl type
4. **CodeGenModule::EmitGlobal()** - for VarDecl, checks if it's a definition
5. **GetOrCreateLLVMGlobal()** - creates or reuses LLVM GlobalVariable

### 2. How Extern Declarations Are Supposed to Work

In `CodeGenModule::GetOrCreateLLVMGlobal()` (CodeGenModule.cpp:5277-5335):

```cpp
auto *GV = new llvm::GlobalVariable(
    getModule(), Ty, false, llvm::GlobalValue::ExternalLinkage, nullptr,
    MangledName, nullptr, llvm::GlobalVariable::NotThreadLocal,
    getContext().getTargetAddressSpace(DAddrSpace));
```

Key observation: The `nullptr` for initializer means this creates an **external reference**, NOT a definition. The IR should be:
```llvm
@_GImGui = external global ptr
```

This is **correct behavior** - the JIT linker should resolve this to an existing symbol.

### 3. The Real Problem: Inline Function Compilation

The issue is NOT with the extern declaration itself, but with **inline function bodies** that reference these externals.

When you include `imgui.h`:
```cpp
// In imgui.h
inline void ImGui::NewFrame() {
    ImGuiContext& g = *GImGui;  // References the extern global
    g.WithinFrameScope = true;
    // ...
}
```

**What happens:**
1. The inline function definition is parsed
2. CodeGen **compiles the function body** into the JIT module
3. The compiled IR references `@_GImGui`

The problem occurs when the **JIT-compiled inline function** is used instead of the AOT-compiled one.

### 4. Why DynamicLibrarySearchGenerator Alone Doesn't Work

The `DynamicLibrarySearchGenerator::GetForCurrentProcess('_')` adds a symbol generator that uses `dlsym` to find symbols. However:

1. **iOS Restrictions**: `dlsym(RTLD_DEFAULT, ...)` may not find symbols in the main executable on iOS due to sandboxing and different dynamic loader behavior.

2. **Symbol Visibility**: On iOS, symbols in the main executable may not be exported in a way that `dlsym` can find them unless explicitly marked with visibility attributes.

3. **Search Order**: Even if found, there's a race condition in how the JIT processes external references vs. how the linker resolves them.

### 5. Why Manual Registration Works

Using `absoluteSymbols` pre-registers symbols with LLVM ORC **before** any module is loaded:

```cpp
auto &ES = Jit->getExecutionSession();
auto &JD = Jit->getMainJITDylib();
auto SymbolAddr = llvm::orc::ExecutorAddr::fromPtr(ptr);
auto SymbolFlags = llvm::JITSymbolFlags::Exported;
if (callable)
    SymbolFlags |= llvm::JITSymbolFlags::Callable;

JD.define(llvm::orc::absoluteSymbols({
    {ES.intern(name), {SymbolAddr, SymbolFlags}}
}));
```

This works because:
1. The symbol is registered with the exact memory address of the AOT symbol
2. When the JIT module references this symbol, it resolves to the pre-registered address
3. No `dlsym` call is needed - the symbol is already in the JIT symbol table

### 6. Root Cause Analysis

The fundamental issue is:

```
For inline functions defined in headers, both AOT and JIT create
function BODIES that reference globals. When JIT code calls the
JIT-compiled version of an inline function, it uses whatever
address the JIT resolved for the global - which may differ from AOT.
```

This is **by design** in how C++ inline functions work - each translation unit that includes the header gets its own copy of the function body.

## Implementation Plans

### Plan A: Comprehensive Symbol Pre-Registration (Current Approach - Works)

**How it works:**
- iOS app explicitly registers all symbols that inline functions might reference
- Must be done BEFORE loading JIT modules
- Requires knowing which symbols will be referenced

**Pros:**
- Works reliably
- No dlsym dependency
- Full control over symbol addresses

**Cons:**
- Manual maintenance of symbol lists
- Tedious for large APIs like ImGui
- Symbols might be missed

**Improvements possible:**
- Auto-generate symbol registration code from headers
- Use nm/objdump to extract symbol lists
- Create a macro-based registration helper

### Plan B: Enhanced DynamicLibrarySearchGenerator

**Implementation:**
```cpp
// Instead of GetForCurrentProcess, create custom generator
class MainExecutableGenerator : public DefinitionGenerator {
public:
    llvm::Error tryToGenerate(...) override {
        for (auto &name : names) {
            // Use _dyld_lookup_and_bind or similar iOS-specific API
            // Or maintain a static map of known symbol addresses
            void *addr = findSymbolInMainExecutable(name);
            if (addr) {
                // Register with absoluteSymbols
            }
        }
    }
};
```

**Pros:**
- Automatic - no manual registration needed
- Works with any symbols

**Cons:**
- iOS-specific implementation needed
- May still have visibility issues
- Requires reliable symbol lookup mechanism

### Plan C: Suppress Inline Function JIT Compilation

**Concept:** Configure CppInterOp/clang to NOT compile inline function bodies, only parse their declarations.

**How it might work:**
1. Use `-fno-inline` or similar flag
2. Or use a custom CodeGen configuration that skips inline function bodies
3. Rely on the linker to find the AOT versions

**Research needed:**
- Does clang support this mode?
- Would it break cases where inline functions ARE defined only in headers?
- How to differentiate "inline from header we're parsing" vs "inline we need to compile"?

### Plan D: Link-Time Symbol Deduplication

**Concept:** After JIT compilation, but before execution, scan the module for symbols that already exist in AOT and redirect references.

**Implementation:**
```cpp
void deduplicateWithAOTSymbols(llvm::Module &M) {
    for (auto &GV : M.globals()) {
        if (GV.isDeclaration()) {
            // Find in AOT
            void *aotAddr = dlsym(RTLD_DEFAULT, GV.getName());
            if (aotAddr) {
                // Replace all uses with constant address
                // Or pre-register with absoluteSymbols
            }
        }
    }
}
```

**Pros:**
- Automatic
- Works with existing module loading

**Cons:**
- dlsym reliability issues on iOS
- Must be done at the right time
- Doesn't help if definitions exist (not just declarations)

## Recommended Approach: Compiler-Driven Automatic Registration (Plan E)

**Key Insight**: The jank compiler already knows which C++ symbols are referenced during AOT compilation. We can leverage this to automatically generate registration code.

### How It Works

1. **During Analysis/Codegen**: Track all C++ functions and variables referenced
   - `expr::cpp_call` contains the `Cpp::TCppScope_t` function pointer
   - `expr::cpp_member_call`, `expr::cpp_value` (for globals) also have this info

2. **Collect Mangled Names**: Use CppInterOp to get mangled symbol names
   ```cpp
   // In codegen, for each C++ call:
   auto mangled = Cpp::GetMangledName(func_scope);
   auto qualified = Cpp::GetQualifiedName(func_scope);
   cpp_symbols_used.insert({mangled, qualified, is_function});
   ```

3. **Generate Registration Code in gen_entrypoint()**:
   ```cpp
   // Generated in the AOT entrypoint:
   extern ImGuiContext* GImGui;
   extern "C" void _ZN5ImGui8NewFrameEv();  // Declare to get address

   void jank_register_aot_cpp_symbols() {
       jank_jit_register_symbol("_GImGui", (void*)&GImGui, 0);
       jank_jit_register_symbol("__ZN5ImGui8NewFrameEv", (void*)&_ZN5ImGui8NewFrameEv, 1);
       // ... all symbols used by jank code
   }
   ```

4. **Call at iOS Init**:
   ```cpp
   // In jank_ios_register_native_modules or jank_init_with_pch:
   jank_register_aot_cpp_symbols();
   ```

### Implementation Steps

**Step 1: Add symbol collection to codegen processor**

In `include/cpp/jank/codegen/processor.hpp`:
```cpp
struct cpp_symbol_info {
    jtl::immutable_string mangled_name;
    jtl::immutable_string qualified_name;
    bool is_function;
    bool is_member;
};

// Add to processor or runtime context:
static native_set<cpp_symbol_info> collected_cpp_symbols;
```

**Step 2: Collect symbols during codegen**

In `codegen/processor.cpp` in `gen(expr::cpp_call_ref)`, `gen(expr::cpp_member_call_ref)`, etc.:
```cpp
// After generating the call:
if(target == compilation_target::module) {
    auto const mangled = Cpp::GetMangledName(match);
    auto const qualified = cpp_util::get_qualified_name(match);
    collected_cpp_symbols.insert({mangled, qualified, true, is_member});
}
```

**Step 3: Generate registration in gen_entrypoint()**

In `aot/processor.cpp`:
```cpp
// In gen_entrypoint(), after generating module loads:
sb("\n// Auto-generated C++ symbol registration for iOS JIT\n");
sb("void jank_register_aot_cpp_symbols() {\n");

for(auto const& sym : collected_cpp_symbols) {
    // Emit extern declaration
    if(sym.is_function) {
        util::format_to(sb, "  extern \"C\" void {}();\n", sym.mangled_name);
    } else {
        util::format_to(sb, "  extern void* {};\n", sym.mangled_name);
    }

    // Emit registration call
    util::format_to(sb,
        "  jank_jit_register_symbol(\"{}\", (void*)&{}, {});\n",
        sym.mangled_name, sym.mangled_name, sym.is_function ? 1 : 0);
}

sb("}\n");
```

**Step 4: Call registration at init**

In `c_api.cpp` in `jank_ios_register_native_modules()` or `jank_init_with_pch()`:
```cpp
#ifdef JANK_IOS_JIT
extern "C" void jank_register_aot_cpp_symbols();

// In init:
jank_register_aot_cpp_symbols();
#endif
```

### Why This Works

1. **No dlsym needed**: By using `extern` declarations in generated C++, the AOT compiler resolves the addresses at link time
2. **Automatic**: No manual maintenance - the compiler tracks what's needed
3. **Complete**: Captures ALL C++ symbols used by jank code, not just ImGui
4. **No overhead**: Registration happens once at init, before any JIT code runs

### Advantages Over Manual Registration

| Aspect | Manual | Automatic |
|--------|--------|-----------|
| Maintenance | High - must update when code changes | Zero - compiler handles it |
| Coverage | Incomplete - easy to miss symbols | Complete - all referenced symbols |
| Correctness | Error-prone - mangling mistakes | Correct - compiler knows exact names |
| Works with | Only known libraries | Any C++ code jank calls |

## iOS-Specific Considerations

### Symbol Lookup APIs

On iOS, alternatives to `dlsym`:
1. `_dyld_lookup_and_bind` - more direct access
2. `dladdr` - can find symbol info from address
3. Static symbol table parsing using `<mach-o/nlist.h>`

### Visibility Requirements

For symbols to be findable:
1. Must not be stripped
2. Must be in dynamic symbol table (not just local)
3. Consider `-exported_symbols_list` linker flag

## Files Referenced

- `clang/lib/Interpreter/Interpreter.cpp` - main interpreter logic
- `clang/lib/Interpreter/IncrementalParser.cpp` - parsing flow
- `clang/lib/Interpreter/IncrementalAction.cpp` - codegen triggering
- `clang/lib/Interpreter/IncrementalExecutor.cpp` - JIT execution
- `clang/lib/CodeGen/CodeGenModule.cpp` - IR generation
- `clang/lib/CodeGen/ModuleBuilder.cpp` - handles top-level decls
- `cppinterop/lib/Interpreter/CppInterOpInterpreter.h` - CppInterOp wrapper

## Conclusion

The "duplicate symbol" issue is fundamentally about how C++ inline functions work with JIT compilation. Each compilation (AOT and JIT) creates its own copies of inline function bodies that reference globals. The solution is to ensure the JIT's symbol resolution points to the same addresses as AOT.

Manual symbol pre-registration with `absoluteSymbols` is the most reliable current solution. Future work could automate this process or modify how inline functions are handled during JIT compilation.
