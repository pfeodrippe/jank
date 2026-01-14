# Pure Rust Clojure Runtime: Comprehensive Research & Implementation Plan

**Date**: 2026-01-13
**Status**: Exhaustive Research Document
**Purpose**: Detailed analysis and plan for building a **pure Rust** Clojure-like runtime with JIT capabilities, replacing C++ entirely

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [The Core Challenge: Replacing CppInterOp in Rust](#2-the-core-challenge-replacing-cppinterop-in-rust)
3. [JIT Compilation Options in Pure Rust](#3-jit-compilation-options-in-pure-rust)
4. [Cranelift Deep Dive: The Recommended JIT Backend](#4-cranelift-deep-dive-the-recommended-jit-backend)
5. [Runtime Reflection & Type Introspection](#5-runtime-reflection--type-introspection)
6. [Building a "Rust Interpreter as a Service"](#6-building-a-rust-interpreter-as-a-service)
7. [Memory Management: gc-arena Architecture](#7-memory-management-gc-arena-architecture)
8. [Persistent Data Structures](#8-persistent-data-structures)
9. [Real-World Examples: Pure Rust Language Runtimes](#9-real-world-examples-pure-rust-language-runtimes)
10. [Complete Architecture Design](#10-complete-architecture-design)
11. [Performance Analysis](#11-performance-analysis)
12. [Implementation Phases](#12-implementation-phases)
13. [Challenges and Mitigations](#13-challenges-and-mitigations)
14. [Conclusions](#14-conclusions)

---

## 1. Executive Summary

### Vision

Build a **pure Rust Clojure runtime** that provides:
- JIT compilation via Cranelift (no C++/LLVM dependency required)
- Runtime type introspection via bevy_reflect + custom traits
- Garbage collection via gc-arena (proven in Ruffle/Piccolo)
- Persistent data structures via imbl
- REPL/eval capability via dynamic compilation

### Key Findings

| Capability | C++ (CppInterOp) | **Pure Rust Equivalent** | Feasibility |
|------------|------------------|--------------------------|-------------|
| JIT compilation | Clang-REPL + ORC JIT | **Cranelift** | ✅ Excellent |
| Type reflection | Clang AST | **bevy_reflect + Any** | ✅ Good |
| Dynamic code exec | `Cpp::Process()` | **evcxr-style + dlopen** | ✅ Good |
| Template instantiation | On-demand | **Generics at compile time** | ⚠️ Different |
| FFI to native | Full C++ access | **FFI to C + Rust libs** | ✅ Good |

### Bottom Line

**A pure Rust Clojure runtime is entirely feasible** using:
- **Cranelift** for JIT (10x faster compile than LLVM, ~14% slower code)
- **gc-arena** for GC (proven by Ruffle, Piccolo)
- **bevy_reflect** for runtime introspection
- **imbl** for persistent data structures
- **linkme/inventory** for global registries

The trade-off: No C++ interop, but instead **Rust ecosystem interop** and native performance.

---

## 2. The Core Challenge: Replacing CppInterOp in Rust

### What CppInterOp Actually Does

CppInterOp provides these capabilities that we need to replicate:

```cpp
// 1. Runtime code execution (REPL)
Cpp::Process("auto x = 42;");          // Execute C++ code
Cpp::Evaluate("x + 1", &result);       // Evaluate and get result

// 2. Type introspection (reflection)
Cpp::IsClass(type);                    // Check if type is class
Cpp::GetClassMethods(type, methods);   // Get method list
Cpp::GetQualifiedName(type);           // Get type name

// 3. Dynamic function creation
Cpp::MakeFunctionCallable(func);       // Create callable wrapper

// 4. Runtime object manipulation
Cpp::Construct(type, arena);           // Create object of type
Cpp::Destruct(obj, type, true);        // Destroy object
```

### Pure Rust Equivalents

| CppInterOp Feature | Rust Solution | Library/Approach |
|-------------------|---------------|------------------|
| `Cpp::Process()` | JIT compile & execute | **Cranelift JIT** |
| `Cpp::Evaluate()` | Compile to fn, call, return | **Cranelift + dlopen** |
| `Cpp::IsClass()` | TypeId + Reflect trait | **bevy_reflect** |
| `Cpp::GetClassMethods()` | Reflect trait inspection | **bevy_reflect** |
| `Cpp::MakeFunctionCallable()` | DynamicFunction | **bevy_reflect** |
| `Cpp::Construct()` | FromReflect + registry | **bevy_reflect + inventory** |

---

## 3. JIT Compilation Options in Pure Rust

### 3.1 Option Comparison

| Library | Approach | Compile Speed | Code Quality | Dependencies | Best For |
|---------|----------|---------------|--------------|--------------|----------|
| **Cranelift** | Pure Rust | **10x faster** | ~14% slower | None | **Recommended** |
| **Inkwell** | LLVM wrapper | Slow | Best | LLVM 11-21 | Max perf needed |
| **llvm-sys** | Raw LLVM FFI | Slow | Best | LLVM | Low-level control |
| **dynasm-rs** | Direct asm | Fastest | Manual | None | Hot loops only |

### 3.2 Why Cranelift is the Answer

**Cranelift** is a pure-Rust compiler backend that provides:

```
┌─────────────────────────────────────────────────────────────────┐
│                    Cranelift Architecture                        │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  Your IR (Clojure AST)                                          │
│         │                                                        │
│         ▼                                                        │
│  Cranelift IR (CLIR)                                            │
│         │                                                        │
│         ▼                                                        │
│  cranelift-codegen                                               │
│         │                                                        │
│         ▼                                                        │
│  Machine Code (x86-64, aarch64, s390x, riscv64)                 │
│         │                                                        │
│         ▼                                                        │
│  cranelift-jit (memory allocation + relocation)                 │
│         │                                                        │
│         ▼                                                        │
│  Executable Function Pointer                                    │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

**Performance Characteristics** (from official benchmarks):
- **Compile time**: 10x faster than LLVM
- **Runtime**: ~2% slower than V8 TurboFan, ~14% slower than LLVM
- **Size**: 200k LOC vs LLVM's 20M LOC
- **Platforms**: x86-64, ARM64, s390x, RISC-V

**Production Use**:
- **Wasmtime**: WebAssembly runtime (Bytecode Alliance)
- **rustc_codegen_cranelift**: Alternative Rust compiler backend
- **Ruffle**: Flash emulator (planned for ActionScript 3 JIT)

**Key Innovation**: Uses **e-graphs** for unified optimization framework.

---

## 4. Cranelift Deep Dive: The Recommended JIT Backend

### 4.1 Core API

```rust
use cranelift_jit::{JITBuilder, JITModule};
use cranelift_codegen::{Context, settings};
use cranelift_frontend::{FunctionBuilder, FunctionBuilderContext};
use cranelift_module::Module;

// Step 1: Create JIT module
let mut flag_builder = settings::builder();
flag_builder.set("opt_level", "speed").unwrap();
let isa_builder = cranelift_native::builder().unwrap();
let isa = isa_builder.finish(settings::Flags::new(flag_builder)).unwrap();

let builder = JITBuilder::with_isa(isa, cranelift_module::default_libcall_names());
let mut module = JITModule::new(builder);

// Step 2: Define a function
let mut sig = module.make_signature();
sig.params.push(AbiParam::new(types::I64));  // Input: i64
sig.returns.push(AbiParam::new(types::I64)); // Output: i64

let func_id = module.declare_function("my_func", Linkage::Export, &sig)?;

// Step 3: Build function body
let mut ctx = module.make_context();
ctx.func.signature = sig;

let mut func_ctx = FunctionBuilderContext::new();
let mut builder = FunctionBuilder::new(&mut ctx.func, &mut func_ctx);

let entry_block = builder.create_block();
builder.append_block_params_for_function_params(entry_block);
builder.switch_to_block(entry_block);

// Get parameter
let arg = builder.block_params(entry_block)[0];

// Add 1 to it
let one = builder.ins().iconst(types::I64, 1);
let result = builder.ins().iadd(arg, one);

// Return
builder.ins().return_(&[result]);
builder.seal_all_blocks();
builder.finalize();

// Step 4: Compile and get function pointer
module.define_function(func_id, &mut ctx)?;
module.clear_context(&mut ctx);
module.finalize_definitions()?;

let code_ptr = module.get_finalized_function(func_id);
let func: fn(i64) -> i64 = unsafe { std::mem::transmute(code_ptr) };

// Step 5: Call it!
assert_eq!(func(41), 42);
```

### 4.2 Cranelift IR for Clojure

Here's how Clojure concepts map to Cranelift IR:

```rust
// Clojure: (defn add-one [x] (+ x 1))
// Cranelift IR:

function %add_one(i64) -> i64 {
block0(v0: i64):
    v1 = iconst.i64 1
    v2 = iadd v0, v1
    return v2
}

// Clojure: (if (> x 0) x (- x))
// Cranelift IR:

function %abs(i64) -> i64 {
block0(v0: i64):
    v1 = iconst.i64 0
    v2 = icmp sgt v0, v1
    brif v2, block1, block2

block1:
    return v0

block2:
    v3 = ineg v0
    return v3
}

// Clojure: (loop [i 0 sum 0] (if (< i n) (recur (inc i) (+ sum i)) sum))
// Cranelift IR:

function %sum_to_n(i64) -> i64 {
block0(v0: i64):          ; v0 = n
    v1 = iconst.i64 0     ; i = 0
    v2 = iconst.i64 0     ; sum = 0
    jump block1(v1, v2)

block1(v3: i64, v4: i64): ; loop(i, sum)
    v5 = icmp slt v3, v0  ; i < n
    brif v5, block2, block3

block2:                    ; loop body
    v6 = iconst.i64 1
    v7 = iadd v3, v6       ; i + 1
    v8 = iadd v4, v3       ; sum + i
    jump block1(v7, v8)    ; recur

block3:
    return v4              ; return sum
}
```

### 4.3 Calling Conventions & Runtime Integration

```rust
/// Value representation for Clojure runtime
#[repr(C)]
pub struct ClojureValue {
    tag: u64,      // Type tag
    data: u64,     // Inline data or pointer
}

/// JIT function signature for Clojure
type ClojureFn = extern "C" fn(*const ClojureValue, usize) -> ClojureValue;

// In Cranelift, define signature:
let mut sig = module.make_signature();
sig.params.push(AbiParam::new(types::I64));  // *const ClojureValue (args ptr)
sig.params.push(AbiParam::new(types::I64));  // usize (arg count)
sig.returns.push(AbiParam::new(types::I64)); // ClojureValue.tag
sig.returns.push(AbiParam::new(types::I64)); // ClojureValue.data
```

### 4.4 Crate Dependencies

```toml
[dependencies]
cranelift = "0.115"           # Umbrella crate
cranelift-jit = "0.115"       # JIT module
cranelift-codegen = "0.115"   # Code generator
cranelift-frontend = "0.115"  # IR builder
cranelift-module = "0.115"    # Module abstraction
cranelift-native = "0.115"    # Native target detection
target-lexicon = "0.12"       # Target triple handling
```

---

## 5. Runtime Reflection & Type Introspection

### 5.1 The Rust Reflection Landscape

Rust doesn't have built-in runtime reflection, but several libraries provide it:

| Library | Runtime Introspection | Dynamic Dispatch | Type Registry | Production Ready |
|---------|----------------------|------------------|---------------|------------------|
| **bevy_reflect** | Yes | Yes | Yes | Yes (Bevy engine) |
| **std::any::Any** | Basic | No | No | Yes (stdlib) |
| **inventory** | No | Link-time | Yes | Yes |
| **linkme** | No | Link-time | Yes | Yes |

### 5.2 bevy_reflect: Full Runtime Reflection

```rust
use bevy_reflect::{Reflect, TypeRegistry, DynamicStruct, FromReflect};

// 1. Define reflectable types
#[derive(Reflect, Default)]
pub struct PersistentVector {
    len: usize,
    #[reflect(ignore)]
    data: Vec<ClojureValue>,  // Internal, not reflected
}

#[derive(Reflect)]
pub struct ClojureFunction {
    name: String,
    arity: usize,
}

// 2. Create type registry
let mut registry = TypeRegistry::default();
registry.register::<PersistentVector>();
registry.register::<ClojureFunction>();

// 3. Runtime type inspection
fn inspect_value(value: &dyn Reflect, registry: &TypeRegistry) {
    // Get type info
    let type_info = value.get_represented_type_info().unwrap();
    println!("Type: {}", type_info.type_path());

    // Check type
    if let Some(struct_info) = type_info.as_struct() {
        for field in struct_info.iter() {
            println!("  Field: {} ({})", field.name(), field.type_path());
        }
    }
}

// 4. Dynamic function calls
use bevy_reflect::func::{DynamicFunction, FunctionRegistry};

fn create_add_function() -> DynamicFunction {
    (|a: i64, b: i64| a + b).into_function()
}

let mut func_registry = FunctionRegistry::default();
func_registry.register("add", create_add_function());

// Call by name
let result = func_registry
    .call("add", vec![1i64.into_partial_reflect(), 2i64.into_partial_reflect()])
    .unwrap();
```

### 5.3 std::any for Basic Type Checking

```rust
use std::any::{Any, TypeId};

/// Check if a value is a specific type at runtime
fn value_type_name(value: &dyn Any) -> &'static str {
    if value.is::<i64>() { "Integer" }
    else if value.is::<f64>() { "Float" }
    else if value.is::<String>() { "String" }
    else if value.is::<PersistentVector>() { "Vector" }
    else { "Unknown" }
}

/// Downcast to specific type
fn as_integer(value: &dyn Any) -> Option<&i64> {
    value.downcast_ref::<i64>()
}

/// Type ID comparison (computed at compile time!)
fn is_same_type<A: 'static, B: 'static>() -> bool {
    TypeId::of::<A>() == TypeId::of::<B>()
}
```

### 5.4 linkme/inventory for Global Registration

```rust
use linkme::distributed_slice;

// Define a distributed slice for all registered types
#[distributed_slice]
pub static CLOJURE_TYPES: [fn() -> TypeDescriptor] = [..];

#[derive(Clone)]
pub struct TypeDescriptor {
    pub name: &'static str,
    pub type_id: TypeId,
    pub create_default: fn() -> Box<dyn Any>,
}

// Register a type (from anywhere in the codebase)
#[distributed_slice(CLOJURE_TYPES)]
fn register_vector() -> TypeDescriptor {
    TypeDescriptor {
        name: "clojure.core/PersistentVector",
        type_id: TypeId::of::<PersistentVector>(),
        create_default: || Box::new(PersistentVector::new()),
    }
}

// At runtime, iterate all registered types
fn list_all_types() {
    for get_desc in CLOJURE_TYPES {
        let desc = get_desc();
        println!("Type: {} ({:?})", desc.name, desc.type_id);
    }
}
```

---

## 6. Building a "Rust Interpreter as a Service"

### 6.1 Approaches to Dynamic Code Execution

| Approach | Description | Compile Time | Best For |
|----------|-------------|--------------|----------|
| **Cranelift JIT** | Compile AST to machine code | ~1ms | Hot paths |
| **Bytecode VM** | Interpret bytecode | N/A | Cold paths |
| **evcxr-style** | Compile to .so, dlopen | ~100ms+ | REPL/dev |
| **hot-lib-reloader** | Watch & reload dylibs | ~1s | Live coding |

### 6.2 Cranelift JIT Runtime

```rust
pub struct JitRuntime {
    module: JITModule,
    compiled_functions: HashMap<Symbol, FunctionPtr>,
    context: Context,
    func_ctx: FunctionBuilderContext,
}

impl JitRuntime {
    pub fn new() -> Self {
        let builder = JITBuilder::new(cranelift_module::default_libcall_names());
        let module = JITModule::new(builder);

        JitRuntime {
            module,
            compiled_functions: HashMap::new(),
            context: module.make_context(),
            func_ctx: FunctionBuilderContext::new(),
        }
    }

    /// Compile a Clojure function to native code
    pub fn compile_function(&mut self, name: &str, ast: &ClojureExpr) -> Result<FunctionPtr> {
        // 1. Generate Cranelift IR from AST
        let ir = self.ast_to_ir(ast)?;

        // 2. Compile to machine code
        let func_id = self.module.declare_function(name, Linkage::Local, &ir.signature)?;
        self.module.define_function(func_id, &mut self.context)?;
        self.module.finalize_definitions()?;

        // 3. Get function pointer
        let ptr = self.module.get_finalized_function(func_id);
        self.compiled_functions.insert(name.into(), ptr);

        Ok(ptr)
    }

    /// Execute compiled function
    pub fn call(&self, name: &str, args: &[ClojureValue]) -> Result<ClojureValue> {
        let ptr = self.compiled_functions.get(name)
            .ok_or_else(|| Error::FunctionNotFound(name.into()))?;

        // Cast to appropriate function type and call
        let func: extern "C" fn(*const ClojureValue, usize) -> ClojureValue =
            unsafe { std::mem::transmute(*ptr) };

        Ok(func(args.as_ptr(), args.len()))
    }
}
```

### 6.3 Tiered Execution (Interpreter + JIT)

```rust
/// Tiered execution strategy
pub struct TieredRuntime {
    interpreter: BytecodeInterpreter,
    jit: JitRuntime,
    call_counts: HashMap<Symbol, usize>,
}

impl TieredRuntime {
    const JIT_THRESHOLD: usize = 100; // Compile after 100 calls

    pub fn call(&mut self, name: &str, args: &[ClojureValue]) -> Result<ClojureValue> {
        let count = self.call_counts.entry(name.into()).or_insert(0);
        *count += 1;

        if *count >= Self::JIT_THRESHOLD {
            // Hot function - compile with JIT
            if !self.jit.is_compiled(name) {
                let ast = self.get_function_ast(name)?;
                self.jit.compile_function(name, &ast)?;
            }
            self.jit.call(name, args)
        } else {
            // Cold function - interpret
            self.interpreter.call(name, args)
        }
    }
}
```

### 6.4 evcxr-Style Dynamic Compilation

For REPL scenarios where you want to compile arbitrary Rust code:

```rust
use evcxr::EvalContext;

/// Dynamic Rust code execution (for REPL)
pub struct ReplContext {
    eval_ctx: EvalContext,
}

impl ReplContext {
    pub fn new() -> Result<Self> {
        let eval_ctx = EvalContext::new()?;
        Ok(ReplContext { eval_ctx })
    }

    /// Evaluate arbitrary Rust expression
    pub fn eval(&mut self, code: &str) -> Result<String> {
        match self.eval_ctx.eval(code)? {
            EvalResult::Output { output } => Ok(output),
            EvalResult::Error { error } => Err(error.into()),
        }
    }
}

// Usage:
let mut repl = ReplContext::new()?;
repl.eval("let x = 42;")?;
repl.eval("x + 1")?; // Returns "43"
```

### 6.5 Hot Reloading for Development

```rust
use hot_lib_reloader::*;

// In development mode, enable hot reloading
#[hot_lib_reloader_macro::hot_module(
    dylib = "jank_stdlib",
    file_watch_debounce = 300
)]
pub mod stdlib {
    #[hot_function]
    pub fn core_map(f: ClojureFn, coll: ClojureSeq) -> ClojureSeq;

    #[hot_function]
    pub fn core_filter(pred: ClojureFn, coll: ClojureSeq) -> ClojureSeq;
}

// Functions can be updated while running!
```

---

## 7. Memory Management: gc-arena Architecture

### 7.1 Why gc-arena?

gc-arena is designed specifically for language VMs in Rust:

- **Zero-cost GC pointers**: `Gc<'gc, T>` is `Copy` and pointer-sized
- **Incremental collection**: Based on Lua 5.4 algorithm
- **Low pause times**: Designed for responsive applications
- **Safe**: Uses Rust's borrow checker to prevent dangling pointers
- **Production proven**: Used by Ruffle (Flash) and Piccolo (Lua)

### 7.2 Core Concepts

```rust
use gc_arena::{Arena, Gc, Collect, lock::RefLock};

// 1. Define GC-managed types
#[derive(Collect)]
#[collect(no_drop)]  // Cannot have Drop if GC-managed
pub struct ClojureValue<'gc> {
    tag: ValueTag,
    data: ValueData<'gc>,
}

#[derive(Collect)]
#[collect(no_drop)]
pub enum ValueData<'gc> {
    Nil,
    Boolean(bool),
    Integer(i64),
    Float(f64),
    Symbol(Gc<'gc, Symbol<'gc>>),
    Keyword(Gc<'gc, Keyword<'gc>>),
    String(Gc<'gc, String>),
    Vector(Gc<'gc, PersistentVector<'gc>>),
    Map(Gc<'gc, PersistentHashMap<'gc>>),
    Fn(Gc<'gc, ClojureFn<'gc>>),
    Var(Gc<'gc, Var<'gc>>),
}

// 2. Create arena with root type
type ClojureArena = Arena<Rootable![ClojureRoot<'_>]>;

#[derive(Collect)]
#[collect(no_drop)]
struct ClojureRoot<'gc> {
    namespaces: RefLock<HashMap<Symbol<'gc>, Gc<'gc, Namespace<'gc>>>>,
    current_ns: RefLock<Gc<'gc, Namespace<'gc>>>,
}

// 3. Use the arena
let mut arena: ClojureArena = Arena::new(|mc| {
    ClojureRoot {
        namespaces: RefLock::new(HashMap::new()),
        current_ns: RefLock::new(Gc::new(mc, Namespace::new("user"))),
    }
});

// 4. Mutate inside callbacks
arena.mutate_root(|mc, root| {
    // Allocate new GC objects
    let sym = Gc::new(mc, Symbol::new("my-symbol"));
    let val = Gc::new(mc, ClojureValue {
        tag: ValueTag::Symbol,
        data: ValueData::Symbol(sym),
    });

    // Gc<T> is Copy - no borrow issues!
    let val2 = val;
    let val3 = val;
});

// 5. Collection happens outside mutate
arena.collect_debt();  // Incremental collection
```

### 7.3 Integrating with Cranelift JIT

```rust
/// Runtime state containing both GC arena and JIT
pub struct ClojureRuntime {
    arena: ClojureArena,
    jit: JitRuntime,
}

impl ClojureRuntime {
    /// Compile and execute expression
    pub fn eval(&mut self, expr: &str) -> Result<String> {
        // 1. Parse
        let ast = parse(expr)?;

        // 2. Analyze (inside arena for symbol resolution)
        let analyzed = self.arena.mutate_root(|mc, root| {
            analyze(mc, root, &ast)
        })?;

        // 3. Compile to JIT if complex, else interpret
        if analyzed.should_jit() {
            let func = self.jit.compile(&analyzed)?;

            // 4. Execute JIT code, passing arena context
            self.arena.mutate_root(|mc, root| {
                unsafe {
                    // JIT code receives mutation context
                    func(mc, root)
                }
            })
        } else {
            // 5. Interpret simple expressions
            self.arena.mutate_root(|mc, root| {
                interpret(mc, root, &analyzed)
            })
        }

        // 6. Collect garbage
        self.arena.collect_debt();
    }
}
```

### 7.4 Finalization Support

gc-arena v0.5+ supports finalization:

```rust
// Mark finalizable objects
impl<'gc> ClojureFn<'gc> {
    fn should_finalize(&self) -> bool {
        // Return true if needs cleanup
        self.has_native_resources
    }
}

// In collection cycle
arena.finalize(|dead_objects| {
    for obj in dead_objects {
        if let Some(func) = obj.downcast::<ClojureFn>() {
            func.cleanup_native_resources();
        }
    }
});
```

---

## 8. Persistent Data Structures

### 8.1 imbl: The Recommended Library

```rust
use imbl::{Vector, HashMap, HashSet, OrdMap};

// Vector (RRB-tree based)
let v1: Vector<ClojureValue> = Vector::new();
let v2 = v1.push_back(value1);  // O(log n), v1 unchanged
let v3 = v2.update(0, value2);  // O(log n), structural sharing

// HashMap (HAMT based)
let m1: HashMap<ClojureValue, ClojureValue> = HashMap::new();
let m2 = m1.update(key, value); // O(log n), structural sharing

// Clone is O(1) - just refcount increment
let v_clone = v3.clone();

// Iteration
for item in &v3 {
    // ...
}
```

### 8.2 Performance Characteristics

| Operation | imbl::Vector | imbl::HashMap | std::Vec | std::HashMap |
|-----------|--------------|---------------|----------|--------------|
| push_back | O(log n) | - | O(1) amort | - |
| get | O(log n) | O(log n) | O(1) | O(1) avg |
| update | O(log n) | O(log n) | O(1) | O(1) avg |
| clone | **O(1)** | **O(1)** | O(n) | O(n) |
| iterate | O(n) | O(n) | O(n) | O(n) |

### 8.3 Integration with gc-arena

```rust
// Option 1: GC-managed collections
#[derive(Collect)]
#[collect(no_drop)]
struct GcVector<'gc> {
    // imbl::Vector with GC pointers
    inner: Vector<Gc<'gc, ClojureValue<'gc>>>,
}

// Option 2: Rc-based collections (no GC needed for structure)
struct RcVector {
    // imbl already uses Rc internally for structural sharing
    inner: Vector<RcValue>,
}

// Recommended: Hybrid approach
// - GC for mutable runtime objects (vars, atoms, refs)
// - Rc for immutable persistent structures
#[derive(Collect)]
#[collect(no_drop)]
enum ClojureValue<'gc> {
    // Immutable - Rc-based
    Vector(Arc<PersistentVector>),
    Map(Arc<PersistentHashMap>),

    // Mutable - GC-managed
    Var(Gc<'gc, Var<'gc>>),
    Atom(Gc<'gc, Atom<'gc>>),
    Ref(Gc<'gc, Ref<'gc>>),
}
```

### 8.4 Metadata Support

```rust
/// Clojure value with optional metadata
#[derive(Clone)]
pub struct WithMeta<T> {
    value: T,
    meta: Option<Arc<PersistentHashMap>>,
}

impl<T> WithMeta<T> {
    pub fn new(value: T) -> Self {
        WithMeta { value, meta: None }
    }

    pub fn with_meta(value: T, meta: PersistentHashMap) -> Self {
        WithMeta {
            value,
            meta: Some(Arc::new(meta)),
        }
    }

    pub fn meta(&self) -> Option<&PersistentHashMap> {
        self.meta.as_ref().map(|m| m.as_ref())
    }
}

// Equality ignores metadata (Clojure semantics)
impl<T: PartialEq> PartialEq for WithMeta<T> {
    fn eq(&self, other: &Self) -> bool {
        self.value == other.value
    }
}
```

---

## 9. Real-World Examples: Pure Rust Language Runtimes

### 9.1 Piccolo (Lua in Rust)

**Architecture**: Stackless VM + gc-arena + optional Cranelift JIT

```
Repository: https://github.com/kyren/piccolo
Features:
- Stackless execution (can pause anywhere)
- Incremental GC via gc-arena
- Fuel-based execution control
- Full Lua semantics
```

**Lessons for jank**:
- Stackless design enables incremental GC
- "Fuel" system controls execution quanta
- gc-arena works well for dynamic languages

### 9.2 Ruffle (Flash in Rust)

**Architecture**: ActionScript VM + gc-arena + (future) Cranelift

```
Repository: https://github.com/ruffle-rs/ruffle
Features:
- ActionScript 1/2/3 support
- gc-arena for all runtime values
- Planning Cranelift JIT for AVM2
- Runs in browser via WASM
```

**Lessons for jank**:
- gc-arena scales to production
- Can target WASM alongside native
- Verifier enables safe optimizations

### 9.3 Lust (Lisp with Cranelift JIT)

**Architecture**: Lisp interpreter + Cranelift compiler

```
Repository: https://github.com/wintermute-motherbrain/lust
Features:
- Both interpreter and JIT compiler
- Cranelift backend
- Simple but complete Lisp
```

**Performance**: `(fib 40)` in 2.3 seconds vs Python's 35 seconds

**Lessons for jank**:
- Cranelift works well for Lisp
- Can have both interpreter and JIT
- Compilation is fast enough for REPL

### 9.4 ClojureRS (Clojure in Rust)

**Architecture**: Tree-walking interpreter

```
Repository: https://github.com/clojure-rs/ClojureRS
Features:
- Clojure interpreter in Rust
- Aims for Clojure compatibility
- No JIT currently
```

**Lessons for jank**:
- Clojure semantics implementable in Rust
- Could benefit from JIT addition

---

## 10. Complete Architecture Design

### 10.1 High-Level Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                     Pure Rust Clojure Runtime                        │
├─────────────────────────────────────────────────────────────────────┤
│                                                                      │
│  ┌────────────┐    ┌────────────┐    ┌─────────────────────────┐    │
│  │  Reader    │───▶│  Analyzer  │───▶│  Codegen                │    │
│  │  (Parser)  │    │            │    │  ┌─────────┐ ┌────────┐ │    │
│  └────────────┘    └────────────┘    │  │Bytecode │ │Cranelift│ │   │
│                                       │  │ Emitter │ │IR Gen   │ │    │
│                                       │  └────┬────┘ └───┬────┘ │    │
│                                       └───────┼──────────┼──────┘    │
│                                               │          │           │
│                                               ▼          ▼           │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │                       Runtime                                │    │
│  │  ┌──────────────┐  ┌───────────────┐  ┌─────────────────┐   │    │
│  │  │   Bytecode   │  │  Cranelift    │  │   gc-arena      │   │    │
│  │  │  Interpreter │  │     JIT       │  │   (GC Arena)    │   │    │
│  │  └──────┬───────┘  └───────┬───────┘  └────────┬────────┘   │    │
│  │         │                  │                    │            │    │
│  │         └──────────────────┼────────────────────┘            │    │
│  │                            ▼                                 │    │
│  │  ┌─────────────────────────────────────────────────────┐    │    │
│  │  │              Value System (ClojureValue)             │    │    │
│  │  │  ┌────────┐ ┌────────┐ ┌──────┐ ┌────┐ ┌─────────┐  │    │    │
│  │  │  │Integers│ │Strings │ │Symbols│ │Vars│ │Functions│  │    │    │
│  │  │  └────────┘ └────────┘ └──────┘ └────┘ └─────────┘  │    │    │
│  │  └─────────────────────────────────────────────────────┘    │    │
│  │                                                              │    │
│  │  ┌─────────────────────────────────────────────────────┐    │    │
│  │  │        Persistent Data Structures (imbl)             │    │    │
│  │  │  ┌──────┐ ┌────────┐ ┌────────┐ ┌───────┐ ┌──────┐  │    │    │
│  │  │  │Vector│ │HashMap │ │HashSet │ │OrdMap │ │OrdSet│  │    │    │
│  │  │  └──────┘ └────────┘ └────────┘ └───────┘ └──────┘  │    │    │
│  │  └─────────────────────────────────────────────────────┘    │    │
│  │                                                              │    │
│  │  ┌─────────────────────────────────────────────────────┐    │    │
│  │  │           Type Registry (bevy_reflect + linkme)      │    │    │
│  │  │  • Runtime type inspection                           │    │    │
│  │  │  • Dynamic function dispatch                         │    │    │
│  │  │  • Protocol implementation lookup                    │    │    │
│  │  └─────────────────────────────────────────────────────┘    │    │
│  └──────────────────────────────────────────────────────────────┘    │
│                                                                      │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │                      FFI Layer                               │    │
│  │  ┌───────────────┐  ┌───────────────┐  ┌─────────────────┐  │    │
│  │  │ C FFI (libc)  │  │ Rust Crates   │  │  libloading     │  │    │
│  │  │               │  │  (ecosystem)  │  │  (dynamic libs) │  │    │
│  │  └───────────────┘  └───────────────┘  └─────────────────┘  │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                                                                      │
└─────────────────────────────────────────────────────────────────────┘
```

### 10.2 Component Dependencies

```toml
# Cargo.toml
[package]
name = "jank-rust"
version = "0.1.0"
edition = "2024"

[dependencies]
# JIT Compilation
cranelift = "0.115"
cranelift-jit = "0.115"
cranelift-codegen = "0.115"
cranelift-frontend = "0.115"
cranelift-module = "0.115"
cranelift-native = "0.115"

# Garbage Collection
gc-arena = "0.6"
gc-arena-derive = "0.6"

# Persistent Data Structures
imbl = "3.0"

# Reflection
bevy_reflect = "0.16"

# Global Registry
linkme = "0.3"

# Dynamic Loading
libloading = "0.8"

# String Interning
lasso = "0.7"

# Utilities
thiserror = "2.0"
parking_lot = "0.12"
hashbrown = "0.15"
```

### 10.3 Module Structure

```
jank-rust/
├── Cargo.toml
├── src/
│   ├── lib.rs                    # Crate root
│   ├── main.rs                   # CLI entry point
│   │
│   ├── reader/                   # Lexer & Parser
│   │   ├── mod.rs
│   │   ├── lexer.rs
│   │   └── parser.rs
│   │
│   ├── analyzer/                 # Semantic Analysis
│   │   ├── mod.rs
│   │   ├── scope.rs
│   │   └── type_check.rs
│   │
│   ├── codegen/                  # Code Generation
│   │   ├── mod.rs
│   │   ├── bytecode.rs           # Bytecode emission
│   │   └── cranelift.rs          # Cranelift IR generation
│   │
│   ├── runtime/                  # Runtime System
│   │   ├── mod.rs
│   │   ├── value.rs              # ClojureValue definition
│   │   ├── interpreter.rs        # Bytecode interpreter
│   │   ├── jit.rs                # Cranelift JIT runtime
│   │   ├── gc.rs                 # gc-arena integration
│   │   └── tiered.rs             # Tiered execution
│   │
│   ├── types/                    # Clojure Types
│   │   ├── mod.rs
│   │   ├── persistent_vector.rs
│   │   ├── persistent_map.rs
│   │   ├── persistent_set.rs
│   │   ├── symbol.rs
│   │   ├── keyword.rs
│   │   ├── var.rs
│   │   └── function.rs
│   │
│   ├── core/                     # Core Library
│   │   ├── mod.rs
│   │   ├── seq.rs                # Sequence functions
│   │   ├── coll.rs               # Collection functions
│   │   ├── io.rs                 # I/O functions
│   │   └── math.rs               # Math functions
│   │
│   ├── reflect/                  # Reflection System
│   │   ├── mod.rs
│   │   ├── registry.rs           # Type registry
│   │   └── protocol.rs           # Protocol dispatch
│   │
│   ├── ffi/                      # Foreign Function Interface
│   │   ├── mod.rs
│   │   ├── c_ffi.rs              # C interop
│   │   └── rust_ffi.rs           # Rust crate interop
│   │
│   └── nrepl/                    # nREPL Server
│       ├── mod.rs
│       ├── server.rs
│       └── handler.rs
```

---

## 11. Performance Analysis

### 11.1 Cranelift vs LLVM Trade-offs

| Metric | Cranelift | LLVM (via Inkwell) | Winner |
|--------|-----------|-------------------|--------|
| Compile time | ~1ms/function | ~10ms/function | **Cranelift** |
| Runtime perf | ~86% of LLVM | 100% (baseline) | LLVM |
| Binary size | Small | Large (LLVM dep) | **Cranelift** |
| Dependencies | Pure Rust | LLVM 11-21 | **Cranelift** |
| REPL responsiveness | Excellent | Good | **Cranelift** |
| Optimization level | Basic | Aggressive | LLVM |

**Recommendation**: Use Cranelift for the following reasons:
1. REPL responsiveness is critical for Clojure
2. 14% slowdown is acceptable for most code
3. No LLVM version coordination headaches
4. Pure Rust = easier builds

### 11.2 Tiered Execution Strategy

```
┌─────────────────────────────────────────────────────────────────┐
│                    Tiered Execution Strategy                     │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  Call Count    Execution Mode       Optimization Level           │
│  ─────────────────────────────────────────────────────────────  │
│  0 - 10        Bytecode Interpreter  None                       │
│  10 - 100      Bytecode Interpreter  None (collecting profile)  │
│  100+          Cranelift JIT         Basic optimizations        │
│  1000+         Cranelift JIT         With speculative opts      │
│                                                                  │
│  Fallback: Deoptimization on type guard failure                 │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 11.3 Expected Performance

Based on similar implementations:

| Benchmark | Pure Rust (Cranelift) | C++ jank (CppInterOp) | Clojure JVM |
|-----------|----------------------|----------------------|-------------|
| fib(40) | ~2-3s | ~2-3s | ~1s |
| map/filter 1M | ~200ms | ~200ms | ~150ms |
| REPL startup | ~50ms | ~500ms | ~3s |
| First eval | ~1ms | ~10ms | ~100ms |

**Key Insight**: Cranelift's fast compilation makes REPL much more responsive, even if runtime is slightly slower.

---

## 12. Implementation Phases

### Phase 1: Foundation (Months 1-3)

**Goals**:
- Basic reader/parser
- Value system with gc-arena
- Bytecode interpreter
- Core data types (integers, strings, vectors, maps)

**Deliverables**:
```rust
// Can evaluate basic expressions
runtime.eval("(+ 1 2)")           // => 3
runtime.eval("(first [1 2 3])")   // => 1
runtime.eval("(assoc {:a 1} :b 2)") // => {:a 1 :b 2}
```

### Phase 2: Cranelift JIT (Months 4-6)

**Goals**:
- Cranelift IR generator
- JIT compilation for functions
- Tiered execution
- Performance baseline

**Deliverables**:
```rust
// Functions compile to native code
runtime.eval("(defn fib [n]
               (if (<= n 1) n
                 (+ (fib (- n 1)) (fib (- n 2)))))")
runtime.eval("(fib 40)")  // Runs in ~2s
```

### Phase 3: Core Library (Months 7-9)

**Goals**:
- clojure.core functions
- Sequence abstractions
- Lazy sequences
- Transducers

**Deliverables**:
```rust
// Full core library
runtime.eval("(map inc (range 10))")
runtime.eval("(filter even? (range 100))")
runtime.eval("(reduce + (range 1000))")
```

### Phase 4: Namespaces & Interop (Months 10-12)

**Goals**:
- Namespace system
- Var system
- Rust FFI
- C FFI

**Deliverables**:
```rust
// Namespaces work
runtime.eval("(ns my.app (:require [clojure.string :as str]))")
runtime.eval("(str/upper-case \"hello\")")

// Rust FFI
runtime.eval("(rust/call \"regex::Regex/new\" \"\\\\d+\")")
```

### Phase 5: nREPL & Tooling (Months 13-15)

**Goals**:
- nREPL server
- Editor integration
- Hot reloading
- Debugging support

**Deliverables**:
- Full nREPL protocol support
- Emacs/VS Code integration
- `(source fn)`, `(doc fn)` working

---

## 13. Challenges and Mitigations

### 13.1 Technical Challenges

| Challenge | Impact | Mitigation |
|-----------|--------|------------|
| Macro system | High | Port from Clojure, careful hygiene |
| Lazy sequences | Medium | Thunks in gc-arena |
| Multimethods | Medium | bevy_reflect + HashMap |
| Protocols | Medium | Trait objects + registry |
| STM | High | Consider removing or parking_lot |
| Spec | Low | Future enhancement |

### 13.2 No C++ Interop

**Impact**: Cannot call C++ libraries directly

**Mitigations**:
1. **C FFI**: Can call any C library via FFI
2. **Rust ecosystem**: Access to crates.io libraries
3. **Wrappers**: Create Rust wrappers for needed C++ libs

```rust
// Instead of C++ interop:
// (cpp/value "std::vector<int>")

// Use Rust:
// (rust/call "Vec::new")
// (rust/call "Vec::push" vec 42)
```

### 13.3 Ecosystem Fragmentation

**Risk**: Two Clojures (JVM, this) with different capabilities

**Mitigation**:
- Focus on core Clojure compatibility
- Document differences clearly
- Position as "Rust-native Clojure" rather than replacement

---

## 14. Conclusions

### 14.1 Key Takeaways

1. **Pure Rust Clojure is feasible** using:
   - Cranelift for JIT (10x faster compile, 14% slower runtime)
   - gc-arena for GC (proven by Ruffle, Piccolo)
   - imbl for persistent data structures
   - bevy_reflect for runtime introspection

2. **Trade-offs vs C++ approach**:
   - ❌ No C++ interop
   - ✅ No LLVM dependency
   - ✅ Pure Rust ecosystem
   - ✅ Faster REPL startup
   - ✅ Easier builds & deployment

3. **Performance will be competitive**:
   - Cranelift code is ~86% of LLVM speed
   - REPL responsiveness will be better
   - Tiered execution handles hot/cold paths

### 14.2 Comparison: Pure Rust vs Hybrid C++/Rust

| Aspect | Pure Rust | Hybrid C++/Rust |
|--------|-----------|-----------------|
| C++ interop | ❌ None | ✅ Full (CppInterOp) |
| Rust interop | ✅ Native | ✅ Via FFI |
| Build complexity | Low | High |
| LLVM dependency | None | Required |
| REPL startup | ~50ms | ~500ms |
| Runtime perf | ~86% LLVM | ~100% LLVM |
| Maintainability | Single language | Two languages |

### 14.3 Recommended Path Forward

If abandoning C++ interop is acceptable:

1. **Start fresh** with pure Rust implementation
2. **Use Cranelift** for JIT (not LLVM)
3. **Use gc-arena** for memory management
4. **Use imbl** for persistent data structures
5. **Use bevy_reflect** for runtime introspection
6. **Follow piccolo/Ruffle** patterns for VM design

### 14.4 Final Recommendation

**For a pure Rust approach**: Build a new Clojure runtime from scratch using the Rust ecosystem. This will be cleaner, faster to build, and leverage Rust's strengths fully.

**Key libraries**:
- `cranelift` - JIT compilation
- `gc-arena` - Garbage collection
- `imbl` - Persistent data structures
- `bevy_reflect` - Runtime reflection
- `linkme` - Global registries
- `lasso` - String interning
- `libloading` - Dynamic library loading

**Estimated effort**: 12-18 months for a functional implementation, 24-36 months for production quality.

---

## References

### JIT & Compilation
- [Cranelift Website](https://cranelift.dev/)
- [Cranelift JIT Demo](https://github.com/bytecodealliance/cranelift-jit-demo)
- [Cranelift vs LLVM](https://github.com/bytecodealliance/wasmtime/blob/main/cranelift/docs/compare-llvm.md)
- [YJIT Rust Port (Shopify)](https://shopify.engineering/porting-yjit-ruby-compiler-to-rust)

### Memory Management
- [gc-arena](https://github.com/kyren/gc-arena)
- [Piccolo Blog Post](https://kyju.org/blog/piccolo-a-stackless-lua-interpreter/)

### Reflection & Type Systems
- [bevy_reflect](https://docs.rs/bevy_reflect)
- [linkme](https://docs.rs/linkme)
- [inventory](https://docs.rs/inventory)
- [std::any](https://doc.rust-lang.org/std/any/)

### Persistent Data Structures
- [imbl](https://docs.rs/imbl)
- [rpds](https://docs.rs/rpds)

### Language Runtimes in Rust
- [Ruffle Flash Emulator](https://ruffle.rs/)
- [Piccolo Lua](https://github.com/kyren/piccolo)
- [Lust Lisp](https://github.com/wintermute-motherbrain/lust)
- [ClojureRS](https://github.com/clojure-rs/ClojureRS)

### Dynamic Execution
- [evcxr](https://github.com/evcxr/evcxr)
- [hot-lib-reloader](https://github.com/rksm/hot-lib-reloader-rs)
- [libloading](https://docs.rs/libloading)

### VM Design
- [Writing Interpreters in Rust](https://rust-hosted-langs.github.io/book/)
- [Create Your Own Programming Language](https://createlang.rs/)
