# jank-rs: Seamless Rust Integration Plan

**Date**: 2026-01-13
**Status**: COMPLETE - 122 tests passing!

## Goal

Make calling Rust functions from jank-rs as seamless as jank calling C++. No FFI overhead - just direct native calls.

## How jank Does It (C++ Interop)

```cpp
// jank registers core functions like this:
intern_fn("sqrt", static_cast<f64 (*)(object_ref const)>(&runtime::sqrt));
intern_fn("println", &runtime::println);
intern_fn("+", &runtime::add);

// When you write (sqrt 16) in jank:
// 1. Compiler sees "sqrt" symbol
// 2. Looks up registered function
// 3. Emits direct C++ call: sqrt(16)
// → Zero overhead, native speed!
```

## jank-rs Architecture

### Phase 1: Core Function Registry (like jank's intern_fn)

```rust
// In Compiler::new(), register core Rust functions:
compiler.register_core("sqrt", native_sqrt as *const u8, 1);
compiler.register_core("abs", native_abs as *const u8, 1);
compiler.register_core("println", native_println as *const u8, 1);
compiler.register_core("rand", native_rand as *const u8, 0);

// Native functions are just regular Rust functions!
extern "C" fn native_sqrt(x: i64) -> i64 {
    ((x as f64).sqrt()) as i64
}
```

### Phase 2: JIT Call Generation

When compiling `(sqrt x)`:
1. Check if "sqrt" is a registered core function
2. If yes → emit Cranelift `call` instruction to that address
3. No boxing/unboxing for numeric types (i64 → i64)

```rust
// In compile_expr, when we see (sqrt x):
Value::List(list) => {
    if let Value::Symbol(sym) = list.head() {
        // Check if it's a registered function
        if let Some(func_ref) = ctx.native_refs.get(sym.name()) {
            // Compile arguments
            let args = compile_args(builder, &list, env, loop_ctx)?;
            // Emit direct call!
            return Ok(builder.ins().call(func_ref, &args));
        }
    }
}
```

### Phase 3: Auto-Boxing for Mixed Types

For functions that work with jank Values (not just i64):
- Tagged values (NaN-boxed) pass through as u64
- Rust function receives/returns u64 tagged values
- Zero-copy, zero-overhead

```rust
// For numeric functions: i64 → i64 (no boxing)
extern "C" fn native_sqrt(x: i64) -> i64 { ... }

// For polymorphic functions: u64 tagged → u64 tagged
extern "C" fn native_println(x: u64) -> u64 {
    let val = Tagged::from_bits(x);
    println!("{}", val);
    NIL // Return nil
}
```

## Implementation Steps

### Step 1: Add core function registry to Compiler

```rust
impl Compiler {
    pub fn new() -> JankResult<Self> {
        let mut compiler = Self { ... };
        compiler.register_core_functions();
        Ok(compiler)
    }

    fn register_core_functions(&mut self) {
        // Math
        self.register_core("sqrt", native_sqrt as *const u8, 1);
        self.register_core("abs", native_abs as *const u8, 1);
        self.register_core("pow", native_pow as *const u8, 2);

        // I/O
        self.register_core("println", native_println as *const u8, 1);
        self.register_core("print", native_print as *const u8, 1);
    }
}
```

### Step 2: Declare functions in JIT module

```rust
fn compile_numeric_fn(&mut self, ...) -> JankResult<*const u8> {
    // Declare all registered native functions in the module
    for (name, native) in &self.native_functions {
        let sig = make_native_signature(native.param_count);
        let func_id = self.module.declare_function(name, Linkage::Import, &sig)?;
        // Store FuncRef for later use
    }
    ...
}
```

### Step 3: Handle native calls in compile_expr

```rust
// In compile_expr_with_loop, after checking built-ins:
if let Some((func_ref, param_count)) = ctx.native_refs.get(sym.name()) {
    let args: Vec<Value> = list.iter().skip(1).cloned().collect();
    if args.len() != *param_count {
        return Err(JankError::arity(sym.name(), param_count, args.len()));
    }

    // Compile arguments
    let mut compiled_args = Vec::new();
    for arg in &args {
        compiled_args.push(compile_expr_with_loop(builder, arg, env, loop_ctx, ctx)?);
    }

    // Emit call instruction
    let call = builder.ins().call(*func_ref, &compiled_args);
    return Ok(builder.inst_results(call)[0]);
}
```

### Step 4: Register symbols with JIT

```rust
// Before finalizing, register native function pointers with the JIT:
for (name, native) in &self.native_functions {
    self.module.define_function_bytes(func_id, native.ptr)?;
    // Or use symbol registration
}
```

## Example Usage

```clojure
;; This jank-rs code:
(defn distance [x1 y1 x2 y2]
  (sqrt (+ (* (- x2 x1) (- x2 x1))
           (* (- y2 y1) (- y2 y1)))))

;; Compiles to native code that:
;; 1. Computes (- x2 x1), (- y2 y1)
;; 2. Squares them with imul
;; 3. Adds with iadd
;; 4. DIRECTLY CALLS native_sqrt function
;; → Pure native speed, no interpreter!
```

## Files to Modify

1. `src/runtime/compiler.rs` - Add native function registry and call emission
2. `src/runtime/native.rs` (NEW) - Native Rust functions (sqrt, abs, println, etc.)
3. `src/runtime/mod.rs` - Export native module

## Success Criteria

1. `(sqrt 16)` compiles to a direct call to Rust sqrt function
2. No FFI overhead - verified by benchmarks
3. All 104 existing tests still pass
4. New tests for native function calls
