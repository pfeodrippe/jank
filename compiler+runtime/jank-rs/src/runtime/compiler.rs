//! AST-to-Cranelift compiler for jank-rs
//!
//! This module compiles Clojure function ASTs to native code via Cranelift IR.
//! Supports tagged values using NaN-boxing for mixed-type operations.
//!
//! Native Rust functions are registered and callable with ZERO overhead -
//! the JIT emits direct call instructions to Rust function addresses.

use std::collections::HashMap;

use cranelift::prelude::*;
use cranelift_jit::{JITBuilder, JITModule};
use cranelift_module::{Linkage, Module, FuncId};
use cranelift_codegen::ir::FuncRef;

use crate::error::{JankError, JankResult};
use crate::types::{Value, Symbol, List};
use crate::runtime::tagged::{Tagged, NIL, TRUE, FALSE};
use crate::runtime::native;

/// Compiled function that works with tagged values (NaN-boxed u64)
pub type CompiledTaggedFn = extern "C" fn(u64) -> u64;

/// Compiled function that works with raw integers (for numeric-only functions)
pub type CompiledNumericFn = extern "C" fn(i64) -> i64;

// NaN-boxing constants (must match tagged.rs)
/// Quiet NaN with sign bit clear
const QNAN: u64 = 0x7FF8_0000_0000_0000;
/// Tag bit to distinguish tagged values from doubles
const TAG_BIT: u64 = 0x0004_0000_0000_0000;
/// Combined mask for tagged values
const TAGGED_MASK: u64 = QNAN | TAG_BIT;
/// Mask for extracting payload
const PAYLOAD_MASK: u64 = 0x0000_FFFF_FFFF_FFFF;
/// Type tag mask
const TYPE_TAG_MASK: u64 = 0x0003_0000_0000_0000;
/// Integer tag
const TAG_INTEGER: u64 = 0x0001_0000_0000_0000;

/// Encode an integer as a NaN-boxed tagged value
#[inline]
pub fn tag_integer(n: i64) -> u64 {
    let payload = (n as u64) & PAYLOAD_MASK;
    TAGGED_MASK | TAG_INTEGER | payload
}

/// Decode a NaN-boxed tagged value to integer (assumes it IS an integer)
#[inline]
pub fn untag_integer(v: u64) -> i64 {
    let payload = v & PAYLOAD_MASK;
    // Sign-extend from 48 bits
    let shifted = (payload as i64) << 16;
    shifted >> 16
}

/// Check if a tagged value is an integer
#[inline]
pub fn is_tagged_integer(v: u64) -> bool {
    (v & TAGGED_MASK) == TAGGED_MASK && (v & TYPE_TAG_MASK) == TAG_INTEGER
}

/// Encode a boolean as a tagged value
#[inline]
pub fn tag_boolean(b: bool) -> u64 {
    if b { TRUE } else { FALSE }
}

/// Get the nil tagged value
#[inline]
pub fn tag_nil() -> u64 {
    NIL
}

/// Metadata about a registered native function
struct NativeFunction {
    /// Function pointer
    ptr: *const u8,
    /// Number of i64 parameters
    param_count: usize,
    /// Whether it returns an i64
    returns_i64: bool,
    /// The function ID in the JIT module (set after declaration)
    func_id: Option<FuncId>,
}

/// The compiler that translates Clojure AST to Cranelift IR
pub struct Compiler {
    /// The JIT module
    module: JITModule,
    /// Context for building functions
    ctx: codegen::Context,
    /// Function builder context
    func_ctx: FunctionBuilderContext,
    /// Compiled functions cache
    compiled: HashMap<String, *const u8>,
    /// Registered native Rust functions that can be called from JIT code
    native_functions: HashMap<String, NativeFunction>,
    /// Whether native functions have been declared in the module
    natives_declared: bool,
}

impl Compiler {
    /// Create a new compiler with all core Rust functions registered
    pub fn new() -> JankResult<Self> {
        let mut flag_builder = settings::builder();
        flag_builder.set("opt_level", "speed").map_err(|e| {
            JankError::eval(format!("Failed to set opt_level: {}", e))
        })?;

        let isa_builder = cranelift_native::builder().map_err(|e| {
            JankError::eval(format!("Failed to create ISA builder: {}", e))
        })?;

        let isa = isa_builder
            .finish(settings::Flags::new(flag_builder))
            .map_err(|e| {
                JankError::eval(format!("Failed to finish ISA: {}", e))
            })?;

        // Create JIT builder and register native function symbols
        let mut builder = JITBuilder::with_isa(isa, cranelift_module::default_libcall_names());

        // Register all native function symbols with the JIT linker
        // This allows Cranelift to resolve calls to these functions
        builder.symbol("jank_sqrt", native::jank_sqrt as *const u8);
        builder.symbol("jank_abs", native::jank_abs as *const u8);
        builder.symbol("jank_pow", native::jank_pow as *const u8);
        builder.symbol("jank_mod", native::jank_mod as *const u8);
        builder.symbol("jank_quot", native::jank_quot as *const u8);
        builder.symbol("jank_min", native::jank_min as *const u8);
        builder.symbol("jank_max", native::jank_max as *const u8);
        builder.symbol("jank_rand_int", native::jank_rand_int as *const u8);
        builder.symbol("jank_bit_and", native::jank_bit_and as *const u8);
        builder.symbol("jank_bit_or", native::jank_bit_or as *const u8);
        builder.symbol("jank_bit_xor", native::jank_bit_xor as *const u8);
        builder.symbol("jank_bit_not", native::jank_bit_not as *const u8);
        builder.symbol("jank_bit_shift_left", native::jank_bit_shift_left as *const u8);
        builder.symbol("jank_bit_shift_right", native::jank_bit_shift_right as *const u8);
        builder.symbol("jank_println_int", native::jank_println_int as *const u8);

        let module = JITModule::new(builder);
        let ctx = module.make_context();

        let mut compiler = Compiler {
            module,
            ctx,
            func_ctx: FunctionBuilderContext::new(),
            compiled: HashMap::new(),
            native_functions: HashMap::new(),
            natives_declared: false,
        };

        // Register metadata for all core functions
        compiler.register_core_functions();

        Ok(compiler)
    }

    /// Register all core Rust functions
    fn register_core_functions(&mut self) {
        // Math functions (1 param)
        self.register_native("sqrt", native::jank_sqrt as *const u8, 1, true);
        self.register_native("abs", native::jank_abs as *const u8, 1, true);

        // Math functions (2 params)
        self.register_native("pow", native::jank_pow as *const u8, 2, true);
        self.register_native("mod", native::jank_mod as *const u8, 2, true);
        self.register_native("quot", native::jank_quot as *const u8, 2, true);
        self.register_native("min", native::jank_min as *const u8, 2, true);
        self.register_native("max", native::jank_max as *const u8, 2, true);

        // Random (1 param)
        self.register_native("rand-int", native::jank_rand_int as *const u8, 1, true);

        // Bit operations
        self.register_native("bit-and", native::jank_bit_and as *const u8, 2, true);
        self.register_native("bit-or", native::jank_bit_or as *const u8, 2, true);
        self.register_native("bit-xor", native::jank_bit_xor as *const u8, 2, true);
        self.register_native("bit-not", native::jank_bit_not as *const u8, 1, true);
        self.register_native("bit-shift-left", native::jank_bit_shift_left as *const u8, 2, true);
        self.register_native("bit-shift-right", native::jank_bit_shift_right as *const u8, 2, true);

        // I/O (special case - uses i64 for simplicity)
        self.register_native("println-int", native::jank_println_int as *const u8, 1, true);
    }

    /// Register a native Rust function that can be called from JIT code
    ///
    /// # Safety
    /// The function pointer must be valid and have the correct signature:
    /// extern "C" fn(i64, ...) -> i64
    ///
    /// # Example
    /// ```ignore
    /// // Register a Rust function
    /// extern "C" fn my_sqrt(x: i64) -> i64 {
    ///     (x as f64).sqrt() as i64
    /// }
    /// compiler.register_native("sqrt", my_sqrt as *const u8, 1, true);
    ///
    /// // Now you can call (sqrt 16) from JIT code!
    /// ```
    pub fn register_native(
        &mut self,
        name: &str,
        ptr: *const u8,
        param_count: usize,
        returns_i64: bool,
    ) {
        self.native_functions.insert(name.to_string(), NativeFunction {
            ptr,
            param_count,
            returns_i64,
            func_id: None,
        });
    }

    /// Check if a function is a registered native function
    pub fn is_native(&self, name: &str) -> bool {
        self.native_functions.contains_key(name)
    }

    /// Get the internal symbol name for a native function
    fn get_native_symbol(&self, name: &str) -> Option<String> {
        // Map Clojure-style names to Rust symbol names
        match name {
            "sqrt" => Some("jank_sqrt".to_string()),
            "abs" => Some("jank_abs".to_string()),
            "pow" => Some("jank_pow".to_string()),
            "mod" => Some("jank_mod".to_string()),
            "quot" => Some("jank_quot".to_string()),
            "min" => Some("jank_min".to_string()),
            "max" => Some("jank_max".to_string()),
            "rand-int" => Some("jank_rand_int".to_string()),
            "bit-and" => Some("jank_bit_and".to_string()),
            "bit-or" => Some("jank_bit_or".to_string()),
            "bit-xor" => Some("jank_bit_xor".to_string()),
            "bit-not" => Some("jank_bit_not".to_string()),
            "bit-shift-left" => Some("jank_bit_shift_left".to_string()),
            "bit-shift-right" => Some("jank_bit_shift_right".to_string()),
            "println-int" => Some("jank_println_int".to_string()),
            _ => None,
        }
    }

    /// Declare all native functions in the JIT module (called once per compilation)
    fn declare_native_functions(&mut self) -> JankResult<HashMap<String, (FuncId, usize)>> {
        let mut func_ids = HashMap::new();

        for (name, native) in &self.native_functions {
            // Create signature based on param count
            let mut sig = self.module.make_signature();
            for _ in 0..native.param_count {
                sig.params.push(AbiParam::new(types::I64));
            }
            if native.returns_i64 {
                sig.returns.push(AbiParam::new(types::I64));
            }

            // Get the actual symbol name
            let symbol_name = self.get_native_symbol(name)
                .unwrap_or_else(|| name.replace('-', "_"));

            // Declare as imported function
            let func_id = self.module
                .declare_function(&symbol_name, Linkage::Import, &sig)
                .map_err(|e| JankError::eval(format!("Failed to declare native function {}: {}", name, e)))?;

            func_ids.insert(name.clone(), (func_id, native.param_count));
        }

        self.natives_declared = true;
        Ok(func_ids)
    }

    /// Compile a simple numeric function from AST
    /// This handles: (fn [x] (+ x 1)), (fn [x y] (* x y)), etc.
    /// Also supports calling registered native Rust functions like sqrt, abs, etc.
    pub fn compile_numeric_fn(
        &mut self,
        name: &str,
        params: &[Symbol],
        body: &Value,
    ) -> JankResult<*const u8> {
        // Declare all native functions in the module (if not already done)
        let native_func_ids = self.declare_native_functions()?;

        // Create signature: all params are i64, return is i64
        let mut sig = self.module.make_signature();
        for _ in params {
            sig.params.push(AbiParam::new(types::I64));
        }
        sig.returns.push(AbiParam::new(types::I64));

        let func_id = self.module
            .declare_function(name, Linkage::Local, &sig)
            .map_err(|e| JankError::eval(format!("Failed to declare function: {}", e)))?;

        self.ctx.func.signature = sig;

        {
            let mut builder = FunctionBuilder::new(&mut self.ctx.func, &mut self.func_ctx);

            // Import all native functions into this function
            let mut native_refs = HashMap::new();
            for (fn_name, (fn_id, param_count)) in &native_func_ids {
                let func_ref = self.module.declare_func_in_func(*fn_id, builder.func);
                native_refs.insert(fn_name.clone(), (func_ref, *param_count));
            }

            let ctx = CompileContext { native_refs };

            let entry = builder.create_block();
            builder.append_block_params_for_function_params(entry);
            builder.switch_to_block(entry);
            builder.seal_block(entry);

            // Create variable bindings for parameters
            let mut env: HashMap<String, cranelift::prelude::Value> = HashMap::new();
            for (i, param) in params.iter().enumerate() {
                let val = builder.block_params(entry)[i];
                env.insert(param.name().to_string(), val);
            }

            // Compile the body expression
            let result = compile_expr(&mut builder, body, &env, &ctx)?;

            builder.ins().return_(&[result]);
            builder.finalize();
        }

        self.module.define_function(func_id, &mut self.ctx)
            .map_err(|e| JankError::eval(format!("Failed to define function: {}", e)))?;
        self.module.clear_context(&mut self.ctx);
        self.module.finalize_definitions()
            .map_err(|e| JankError::eval(format!("Failed to finalize: {}", e)))?;

        let code_ptr = self.module.get_finalized_function(func_id);
        self.compiled.insert(name.to_string(), code_ptr);

        Ok(code_ptr)
    }
}

/// Context for loop compilation - passed through to handle recur
struct LoopContext {
    /// The loop header block to jump to on recur
    header_block: Block,
    /// Names of the loop bindings (in order)
    binding_names: Vec<String>,
}

/// Context for compiling expressions - holds native function references
struct CompileContext {
    /// Native function references (name -> FuncRef)
    native_refs: HashMap<String, (FuncRef, usize)>,
}

impl CompileContext {
    fn new() -> Self {
        CompileContext {
            native_refs: HashMap::new(),
        }
    }
}

/// Compile an expression to Cranelift IR (standalone function to avoid borrow issues)
fn compile_expr(
    builder: &mut FunctionBuilder,
    expr: &Value,
    env: &HashMap<String, cranelift::prelude::Value>,
    ctx: &CompileContext,
) -> JankResult<cranelift::prelude::Value> {
    compile_expr_with_loop(builder, expr, env, None, ctx)
}

/// Compile an expression with optional loop context for recur
fn compile_expr_with_loop(
    builder: &mut FunctionBuilder,
    expr: &Value,
    env: &HashMap<String, cranelift::prelude::Value>,
    loop_ctx: Option<&LoopContext>,
    ctx: &CompileContext,
) -> JankResult<cranelift::prelude::Value> {
    match expr {
        // Integer literal
        Value::Integer(n) => {
            Ok(builder.ins().iconst(types::I64, *n))
        }

        // Boolean literal (true = 1, false = 0)
        Value::Bool(b) => {
            Ok(builder.ins().iconst(types::I64, if *b { 1 } else { 0 }))
        }

        // Symbol reference (variable lookup)
        Value::Symbol(sym) => {
            env.get(sym.name())
                .copied()
                .ok_or_else(|| JankError::eval(format!("Unknown variable: {}", sym.name())))
        }

        // Function call
        Value::List(list) => {
            if list.is_empty() {
                return Err(JankError::eval("Empty list in expression"));
            }

            let head = list.head().unwrap();

            // Check for built-in operations
            if let Value::Symbol(sym) = &head {
                match sym.name() {
                    "+" => return compile_binary_arith(builder, list, env, loop_ctx, ctx, BinaryOp::Add),
                    "-" => return compile_binary_arith(builder, list, env, loop_ctx, ctx, BinaryOp::Sub),
                    "*" => return compile_binary_arith(builder, list, env, loop_ctx, ctx, BinaryOp::Mul),
                    "/" => return compile_binary_arith(builder, list, env, loop_ctx, ctx, BinaryOp::Div),
                    "inc" => return compile_inc(builder, list, env, loop_ctx, ctx),
                    "dec" => return compile_dec(builder, list, env, loop_ctx, ctx),
                    "<" => return compile_compare(builder, list, env, loop_ctx, ctx, IntCC::SignedLessThan),
                    ">" => return compile_compare(builder, list, env, loop_ctx, ctx, IntCC::SignedGreaterThan),
                    "<=" => return compile_compare(builder, list, env, loop_ctx, ctx, IntCC::SignedLessThanOrEqual),
                    ">=" => return compile_compare(builder, list, env, loop_ctx, ctx, IntCC::SignedGreaterThanOrEqual),
                    "=" => return compile_compare(builder, list, env, loop_ctx, ctx, IntCC::Equal),
                    "if" => return compile_if(builder, list, env, loop_ctx, ctx),
                    "do" => return compile_do(builder, list, env, loop_ctx, ctx),
                    "not" => return compile_not(builder, list, env, loop_ctx, ctx),
                    "and" => return compile_and(builder, list, env, loop_ctx, ctx),
                    "or" => return compile_or(builder, list, env, loop_ctx, ctx),
                    "zero?" => return compile_zero_check(builder, list, env, loop_ctx, ctx),
                    "pos?" => return compile_pos_check(builder, list, env, loop_ctx, ctx),
                    "neg?" => return compile_neg_check(builder, list, env, loop_ctx, ctx),
                    "loop" => return compile_loop(builder, list, env, ctx),
                    "recur" => return compile_recur(builder, list, env, loop_ctx, ctx),
                    _ => {
                        // Check if it's a registered native function
                        if let Some((func_ref, param_count)) = ctx.native_refs.get(sym.name()) {
                            return compile_native_call(builder, list, env, loop_ctx, ctx, *func_ref, *param_count, sym.name());
                        }
                    }
                }
            }

            Err(JankError::eval(format!(
                "Cannot compile call to: {}",
                head
            )))
        }

        _ => Err(JankError::eval(format!(
            "Cannot compile expression: {:?}",
            expr
        ))),
    }
}

/// Compile a call to a registered native Rust function
/// This emits a direct call instruction to the native function - ZERO overhead!
fn compile_native_call(
    builder: &mut FunctionBuilder,
    list: &List,
    env: &HashMap<String, cranelift::prelude::Value>,
    loop_ctx: Option<&LoopContext>,
    ctx: &CompileContext,
    func_ref: FuncRef,
    param_count: usize,
    fn_name: &str,
) -> JankResult<cranelift::prelude::Value> {
    let args: Vec<Value> = list.iter().skip(1).cloned().collect();

    if args.len() != param_count {
        return Err(JankError::arity(fn_name, param_count.to_string(), args.len()));
    }

    // Compile all arguments
    let mut compiled_args = Vec::new();
    for arg in &args {
        compiled_args.push(compile_expr_with_loop(builder, arg, env, loop_ctx, ctx)?);
    }

    // Emit the call instruction - this is a DIRECT call to native code!
    let call = builder.ins().call(func_ref, &compiled_args);
    let results = builder.inst_results(call);

    if results.is_empty() {
        // Void function - return 0 (nil)
        Ok(builder.ins().iconst(types::I64, 0))
    } else {
        Ok(results[0])
    }
}

/// Compile a binary arithmetic operation
fn compile_binary_arith(
    builder: &mut FunctionBuilder,
    list: &List,
    env: &HashMap<String, cranelift::prelude::Value>,
    loop_ctx: Option<&LoopContext>,
    ctx: &CompileContext,
    op: BinaryOp,
) -> JankResult<cranelift::prelude::Value> {
    let args: Vec<Value> = list.iter().skip(1).cloned().collect();

    if args.is_empty() {
        // (+) => 0, (*) => 1
        return Ok(builder.ins().iconst(types::I64, match op {
            BinaryOp::Add | BinaryOp::Sub => 0,
            BinaryOp::Mul | BinaryOp::Div => 1,
        }));
    }

    if args.len() == 1 {
        // Unary: (- x) => negation, others just return x
        let x = compile_expr_with_loop(builder, &args[0], env, loop_ctx, ctx)?;
        return Ok(match op {
            BinaryOp::Sub => builder.ins().ineg(x),
            _ => x,
        });
    }

    // Binary or n-ary
    let mut result = compile_expr_with_loop(builder, &args[0], env, loop_ctx, ctx)?;
    for arg in &args[1..] {
        let val = compile_expr_with_loop(builder, arg, env, loop_ctx, ctx)?;
        result = match op {
            BinaryOp::Add => builder.ins().iadd(result, val),
            BinaryOp::Sub => builder.ins().isub(result, val),
            BinaryOp::Mul => builder.ins().imul(result, val),
            BinaryOp::Div => builder.ins().sdiv(result, val),
        };
    }

    Ok(result)
}

/// Compile (inc x)
fn compile_inc(
    builder: &mut FunctionBuilder,
    list: &List,
    env: &HashMap<String, cranelift::prelude::Value>,
    loop_ctx: Option<&LoopContext>,
    ctx: &CompileContext,
) -> JankResult<cranelift::prelude::Value> {
    let args: Vec<Value> = list.iter().skip(1).cloned().collect();
    if args.len() != 1 {
        return Err(JankError::arity("inc", "1", args.len()));
    }
    let x = compile_expr_with_loop(builder, &args[0], env, loop_ctx, ctx)?;
    let one = builder.ins().iconst(types::I64, 1);
    Ok(builder.ins().iadd(x, one))
}

/// Compile (dec x)
fn compile_dec(
    builder: &mut FunctionBuilder,
    list: &List,
    env: &HashMap<String, cranelift::prelude::Value>,
    loop_ctx: Option<&LoopContext>,
    ctx: &CompileContext,
) -> JankResult<cranelift::prelude::Value> {
    let args: Vec<Value> = list.iter().skip(1).cloned().collect();
    if args.len() != 1 {
        return Err(JankError::arity("dec", "1", args.len()));
    }
    let x = compile_expr_with_loop(builder, &args[0], env, loop_ctx, ctx)?;
    let one = builder.ins().iconst(types::I64, 1);
    Ok(builder.ins().isub(x, one))
}

/// Compile comparison (< > <= >= =)
fn compile_compare(
    builder: &mut FunctionBuilder,
    list: &List,
    env: &HashMap<String, cranelift::prelude::Value>,
    loop_ctx: Option<&LoopContext>,
    ctx: &CompileContext,
    cc: IntCC,
) -> JankResult<cranelift::prelude::Value> {
    let args: Vec<Value> = list.iter().skip(1).cloned().collect();
    if args.len() != 2 {
        return Err(JankError::arity("comparison", "2", args.len()));
    }

    let a = compile_expr_with_loop(builder, &args[0], env, loop_ctx, ctx)?;
    let b = compile_expr_with_loop(builder, &args[1], env, loop_ctx, ctx)?;

    // Compare and convert bool to i64 (0 or 1)
    let cmp = builder.ins().icmp(cc, a, b);
    let zero = builder.ins().iconst(types::I64, 0);
    let one = builder.ins().iconst(types::I64, 1);
    Ok(builder.ins().select(cmp, one, zero))
}

/// Compile (if cond then else) - creates control flow blocks
fn compile_if(
    builder: &mut FunctionBuilder,
    list: &List,
    env: &HashMap<String, cranelift::prelude::Value>,
    loop_ctx: Option<&LoopContext>,
    ctx: &CompileContext,
) -> JankResult<cranelift::prelude::Value> {
    let args: Vec<Value> = list.iter().skip(1).cloned().collect();
    if args.len() < 2 || args.len() > 3 {
        return Err(JankError::arity("if", "2 or 3", args.len()));
    }

    // Compile condition
    let cond_val = compile_expr_with_loop(builder, &args[0], env, loop_ctx, ctx)?;

    // Create blocks
    let then_block = builder.create_block();
    let else_block = builder.create_block();
    let merge_block = builder.create_block();
    builder.append_block_param(merge_block, types::I64); // result

    // Branch based on condition (non-zero is truthy)
    let zero = builder.ins().iconst(types::I64, 0);
    let is_truthy = builder.ins().icmp(IntCC::NotEqual, cond_val, zero);
    builder.ins().brif(is_truthy, then_block, &[], else_block, &[]);

    // Then block
    builder.switch_to_block(then_block);
    builder.seal_block(then_block);
    let then_val = compile_expr_with_loop(builder, &args[1], env, loop_ctx, ctx)?;
    builder.ins().jump(merge_block, &[then_val]);

    // Else block
    builder.switch_to_block(else_block);
    builder.seal_block(else_block);
    let else_val = if args.len() == 3 {
        compile_expr_with_loop(builder, &args[2], env, loop_ctx, ctx)?
    } else {
        builder.ins().iconst(types::I64, 0) // nil = 0
    };
    builder.ins().jump(merge_block, &[else_val]);

    // Merge block
    builder.switch_to_block(merge_block);
    builder.seal_block(merge_block);

    Ok(builder.block_params(merge_block)[0])
}

/// Compile (do expr...) - evaluate expressions, return last
fn compile_do(
    builder: &mut FunctionBuilder,
    list: &List,
    env: &HashMap<String, cranelift::prelude::Value>,
    loop_ctx: Option<&LoopContext>,
    ctx: &CompileContext,
) -> JankResult<cranelift::prelude::Value> {
    let args: Vec<Value> = list.iter().skip(1).cloned().collect();
    if args.is_empty() {
        return Ok(builder.ins().iconst(types::I64, 0)); // nil
    }

    let mut result = builder.ins().iconst(types::I64, 0);
    for arg in &args {
        result = compile_expr_with_loop(builder, arg, env, loop_ctx, ctx)?;
    }
    Ok(result)
}

/// Compile (not x) - logical not
fn compile_not(
    builder: &mut FunctionBuilder,
    list: &List,
    env: &HashMap<String, cranelift::prelude::Value>,
    loop_ctx: Option<&LoopContext>,
    ctx: &CompileContext,
) -> JankResult<cranelift::prelude::Value> {
    let args: Vec<Value> = list.iter().skip(1).cloned().collect();
    if args.len() != 1 {
        return Err(JankError::arity("not", "1", args.len()));
    }

    let x = compile_expr_with_loop(builder, &args[0], env, loop_ctx, ctx)?;
    let zero = builder.ins().iconst(types::I64, 0);
    let is_falsy = builder.ins().icmp(IntCC::Equal, x, zero);
    let one = builder.ins().iconst(types::I64, 1);
    let zero2 = builder.ins().iconst(types::I64, 0);
    Ok(builder.ins().select(is_falsy, one, zero2))
}

/// Compile (and a b) - short-circuit and
fn compile_and(
    builder: &mut FunctionBuilder,
    list: &List,
    env: &HashMap<String, cranelift::prelude::Value>,
    loop_ctx: Option<&LoopContext>,
    ctx: &CompileContext,
) -> JankResult<cranelift::prelude::Value> {
    let args: Vec<Value> = list.iter().skip(1).cloned().collect();
    if args.is_empty() {
        return Ok(builder.ins().iconst(types::I64, 1)); // (and) = true
    }

    // For now, just evaluate all and return 1 if all truthy, 0 otherwise
    let mut result = compile_expr_with_loop(builder, &args[0], env, loop_ctx, ctx)?;
    for arg in &args[1..] {
        let val = compile_expr_with_loop(builder, arg, env, loop_ctx, ctx)?;
        let zero = builder.ins().iconst(types::I64, 0);
        // result = result && val (both non-zero)
        let r_truthy = builder.ins().icmp(IntCC::NotEqual, result, zero);
        let v_truthy = builder.ins().icmp(IntCC::NotEqual, val, zero);
        let both = builder.ins().band(r_truthy, v_truthy);
        let one = builder.ins().iconst(types::I64, 1);
        let zero2 = builder.ins().iconst(types::I64, 0);
        result = builder.ins().select(both, one, zero2);
    }
    Ok(result)
}

/// Compile (or a b) - short-circuit or
fn compile_or(
    builder: &mut FunctionBuilder,
    list: &List,
    env: &HashMap<String, cranelift::prelude::Value>,
    loop_ctx: Option<&LoopContext>,
    ctx: &CompileContext,
) -> JankResult<cranelift::prelude::Value> {
    let args: Vec<Value> = list.iter().skip(1).cloned().collect();
    if args.is_empty() {
        return Ok(builder.ins().iconst(types::I64, 0)); // (or) = false/nil
    }

    let mut result = compile_expr_with_loop(builder, &args[0], env, loop_ctx, ctx)?;
    for arg in &args[1..] {
        let val = compile_expr_with_loop(builder, arg, env, loop_ctx, ctx)?;
        let zero = builder.ins().iconst(types::I64, 0);
        // result = result || val (either non-zero)
        let r_truthy = builder.ins().icmp(IntCC::NotEqual, result, zero);
        let v_truthy = builder.ins().icmp(IntCC::NotEqual, val, zero);
        let either = builder.ins().bor(r_truthy, v_truthy);
        let one = builder.ins().iconst(types::I64, 1);
        let zero2 = builder.ins().iconst(types::I64, 0);
        result = builder.ins().select(either, one, zero2);
    }
    Ok(result)
}

/// Compile (zero? x)
fn compile_zero_check(
    builder: &mut FunctionBuilder,
    list: &List,
    env: &HashMap<String, cranelift::prelude::Value>,
    loop_ctx: Option<&LoopContext>,
    ctx: &CompileContext,
) -> JankResult<cranelift::prelude::Value> {
    let args: Vec<Value> = list.iter().skip(1).cloned().collect();
    if args.len() != 1 {
        return Err(JankError::arity("zero?", "1", args.len()));
    }
    let x = compile_expr_with_loop(builder, &args[0], env, loop_ctx, ctx)?;
    let zero = builder.ins().iconst(types::I64, 0);
    let is_zero = builder.ins().icmp(IntCC::Equal, x, zero);
    let one = builder.ins().iconst(types::I64, 1);
    let zero2 = builder.ins().iconst(types::I64, 0);
    Ok(builder.ins().select(is_zero, one, zero2))
}

/// Compile (pos? x)
fn compile_pos_check(
    builder: &mut FunctionBuilder,
    list: &List,
    env: &HashMap<String, cranelift::prelude::Value>,
    loop_ctx: Option<&LoopContext>,
    ctx: &CompileContext,
) -> JankResult<cranelift::prelude::Value> {
    let args: Vec<Value> = list.iter().skip(1).cloned().collect();
    if args.len() != 1 {
        return Err(JankError::arity("pos?", "1", args.len()));
    }
    let x = compile_expr_with_loop(builder, &args[0], env, loop_ctx, ctx)?;
    let zero = builder.ins().iconst(types::I64, 0);
    let is_pos = builder.ins().icmp(IntCC::SignedGreaterThan, x, zero);
    let one = builder.ins().iconst(types::I64, 1);
    let zero2 = builder.ins().iconst(types::I64, 0);
    Ok(builder.ins().select(is_pos, one, zero2))
}

/// Compile (neg? x)
fn compile_neg_check(
    builder: &mut FunctionBuilder,
    list: &List,
    env: &HashMap<String, cranelift::prelude::Value>,
    loop_ctx: Option<&LoopContext>,
    ctx: &CompileContext,
) -> JankResult<cranelift::prelude::Value> {
    let args: Vec<Value> = list.iter().skip(1).cloned().collect();
    if args.len() != 1 {
        return Err(JankError::arity("neg?", "1", args.len()));
    }
    let x = compile_expr_with_loop(builder, &args[0], env, loop_ctx, ctx)?;
    let zero = builder.ins().iconst(types::I64, 0);
    let is_neg = builder.ins().icmp(IntCC::SignedLessThan, x, zero);
    let one = builder.ins().iconst(types::I64, 1);
    let zero2 = builder.ins().iconst(types::I64, 0);
    Ok(builder.ins().select(is_neg, one, zero2))
}

/// Compile (loop [bindings...] body...)
/// Creates a loop header block with block parameters for each binding
fn compile_loop(
    builder: &mut FunctionBuilder,
    list: &List,
    env: &HashMap<String, cranelift::prelude::Value>,
    ctx: &CompileContext,
) -> JankResult<cranelift::prelude::Value> {
    let args: Vec<Value> = list.iter().skip(1).cloned().collect();
    if args.is_empty() {
        return Err(JankError::parse("loop requires a binding vector", 0, 0));
    }

    // Get bindings vector
    let bindings = match &args[0] {
        Value::Vector(v) => v,
        _ => return Err(JankError::type_error("vector", args[0].type_name())),
    };

    if bindings.len() % 2 != 0 {
        return Err(JankError::parse("loop bindings must be even", 0, 0));
    }

    // Parse bindings and compute initial values
    let bindings_vec: Vec<_> = bindings.iter().cloned().collect();
    let mut binding_names = Vec::new();
    let mut init_values = Vec::new();

    for chunk in bindings_vec.chunks(2) {
        let name = match &chunk[0] {
            Value::Symbol(s) => s.name().to_string(),
            _ => return Err(JankError::type_error("symbol", chunk[0].type_name())),
        };
        binding_names.push(name);
        // Evaluate initial value in current env (no loop context yet)
        init_values.push(compile_expr_with_loop(builder, &chunk[1], env, None, ctx)?);
    }

    // Create loop header block with block params
    let loop_header = builder.create_block();
    for _ in &binding_names {
        builder.append_block_param(loop_header, types::I64);
    }

    // Create exit block to receive result
    let exit_block = builder.create_block();
    builder.append_block_param(exit_block, types::I64);

    // Jump to loop header with initial values
    builder.ins().jump(loop_header, &init_values);

    // Switch to loop header
    builder.switch_to_block(loop_header);

    // Create environment with loop bindings
    let mut loop_env = env.clone();
    for (i, name) in binding_names.iter().enumerate() {
        loop_env.insert(name.clone(), builder.block_params(loop_header)[i]);
    }

    // Create loop context for recur
    let loop_ctx = LoopContext {
        header_block: loop_header,
        binding_names: binding_names.clone(),
    };

    // Compile body expressions
    let body_forms: Vec<_> = args.iter().skip(1).cloned().collect();
    let mut result = builder.ins().iconst(types::I64, 0);
    for form in &body_forms {
        result = compile_expr_with_loop(builder, form, &loop_env, Some(&loop_ctx), ctx)?;
    }

    // If we reach here without recur, jump to exit
    builder.ins().jump(exit_block, &[result]);

    // Seal the loop header now that all predecessors are known
    builder.seal_block(loop_header);

    // Switch to exit block and return result
    builder.switch_to_block(exit_block);
    builder.seal_block(exit_block);

    Ok(builder.block_params(exit_block)[0])
}

/// Compile (recur expr...)
/// Evaluates arguments and jumps back to loop header with new values
fn compile_recur(
    builder: &mut FunctionBuilder,
    list: &List,
    env: &HashMap<String, cranelift::prelude::Value>,
    loop_ctx: Option<&LoopContext>,
    ctx: &CompileContext,
) -> JankResult<cranelift::prelude::Value> {
    let loop_ctx = loop_ctx.ok_or_else(|| JankError::eval("recur outside of loop"))?;

    let args: Vec<Value> = list.iter().skip(1).cloned().collect();
    if args.len() != loop_ctx.binding_names.len() {
        return Err(JankError::arity(
            "recur",
            loop_ctx.binding_names.len().to_string(),
            args.len(),
        ));
    }

    // Evaluate recur arguments
    let mut new_values = Vec::new();
    for arg in &args {
        new_values.push(compile_expr_with_loop(builder, arg, env, Some(loop_ctx), ctx)?);
    }

    // Jump back to loop header with new values
    builder.ins().jump(loop_ctx.header_block, &new_values);

    // Create a new block for unreachable code after recur
    // This is needed because Cranelift requires all blocks to be properly terminated
    let unreachable_block = builder.create_block();
    builder.switch_to_block(unreachable_block);
    builder.seal_block(unreachable_block);

    // Return a dummy value (never actually reached)
    Ok(builder.ins().iconst(types::I64, 0))
}

#[derive(Debug, Clone, Copy)]
enum BinaryOp {
    Add,
    Sub,
    Mul,
    Div,
}

impl Default for Compiler {
    fn default() -> Self {
        Self::new().expect("Failed to create compiler")
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::reader::read_string;

    /// Helper to parse a fn form and extract params and body
    fn parse_fn(code: &str) -> (Vec<Symbol>, Value) {
        let form = read_string(code).expect("Failed to parse");
        if let Value::List(list) = form {
            let items: Vec<Value> = list.iter().cloned().collect();
            // items[0] = fn, items[1] = params vec, items[2..] = body
            let params_vec = match &items[1] {
                Value::Vector(v) => v.iter().filter_map(|v| {
                    if let Value::Symbol(s) = v { Some(s.clone()) } else { None }
                }).collect(),
                _ => vec![],
            };
            let body = if items.len() == 3 {
                items[2].clone()
            } else {
                // Wrap multiple body forms in do
                Value::list(
                    std::iter::once(Value::Symbol(Symbol::new("do")))
                        .chain(items.into_iter().skip(2))
                )
            };
            (params_vec, body)
        } else {
            panic!("Expected fn form")
        }
    }

    #[test]
    fn test_compile_simple_add() {
        let mut compiler = Compiler::new().unwrap();
        let (params, body) = parse_fn("(fn [x y] (+ x y))");
        let ptr = compiler.compile_numeric_fn("add_xy", &params, &body).unwrap();
        let add: extern "C" fn(i64, i64) -> i64 = unsafe { std::mem::transmute(ptr) };

        assert_eq!(add(1, 2), 3);
        assert_eq!(add(100, 200), 300);
    }

    #[test]
    fn test_compile_nested_expr() {
        let mut compiler = Compiler::new().unwrap();
        let (params, body) = parse_fn("(fn [x] (+ x (+ 1 2)))");
        let ptr = compiler.compile_numeric_fn("add_nested", &params, &body).unwrap();
        let f: extern "C" fn(i64) -> i64 = unsafe { std::mem::transmute(ptr) };

        assert_eq!(f(10), 13); // 10 + (1 + 2) = 13
    }

    #[test]
    fn test_compile_mul() {
        let mut compiler = Compiler::new().unwrap();
        let (params, body) = parse_fn("(fn [x y] (* x y))");
        let ptr = compiler.compile_numeric_fn("mul_xy", &params, &body).unwrap();
        let mul: extern "C" fn(i64, i64) -> i64 = unsafe { std::mem::transmute(ptr) };

        assert_eq!(mul(3, 4), 12);
        assert_eq!(mul(-5, 10), -50);
    }

    #[test]
    fn test_compile_square() {
        let mut compiler = Compiler::new().unwrap();
        let (params, body) = parse_fn("(fn [x] (* x x))");
        let ptr = compiler.compile_numeric_fn("square", &params, &body).unwrap();
        let square: extern "C" fn(i64) -> i64 = unsafe { std::mem::transmute(ptr) };

        assert_eq!(square(5), 25);
        assert_eq!(square(7), 49);
    }

    #[test]
    fn test_compile_inc_dec() {
        let mut compiler = Compiler::new().unwrap();
        let (params, body) = parse_fn("(fn [x] (inc x))");
        let ptr = compiler.compile_numeric_fn("my_inc", &params, &body).unwrap();
        let inc: extern "C" fn(i64) -> i64 = unsafe { std::mem::transmute(ptr) };

        assert_eq!(inc(5), 6);
        assert_eq!(inc(-1), 0);
    }

    #[test]
    fn test_compile_comparison() {
        let mut compiler = Compiler::new().unwrap();
        let (params, body) = parse_fn("(fn [x y] (< x y))");
        let ptr = compiler.compile_numeric_fn("less_than", &params, &body).unwrap();
        let lt: extern "C" fn(i64, i64) -> i64 = unsafe { std::mem::transmute(ptr) };

        assert_eq!(lt(1, 2), 1); // true
        assert_eq!(lt(2, 1), 0); // false
        assert_eq!(lt(1, 1), 0); // false
    }

    #[test]
    fn test_compile_if() {
        let mut compiler = Compiler::new().unwrap();
        let (params, body) = parse_fn("(fn [x] (if (> x 0) 1 -1))");
        let ptr = compiler.compile_numeric_fn("sign", &params, &body).unwrap();
        let sign: extern "C" fn(i64) -> i64 = unsafe { std::mem::transmute(ptr) };

        assert_eq!(sign(5), 1);
        assert_eq!(sign(-5), -1);
        assert_eq!(sign(0), -1); // 0 is not > 0
    }

    #[test]
    fn test_compile_nested_if() {
        let mut compiler = Compiler::new().unwrap();
        let (params, body) = parse_fn("(fn [x] (if (> x 0) (if (< x 10) 1 2) 0))");
        let ptr = compiler.compile_numeric_fn("nested_if", &params, &body).unwrap();
        let f: extern "C" fn(i64) -> i64 = unsafe { std::mem::transmute(ptr) };

        assert_eq!(f(-5), 0);  // x <= 0
        assert_eq!(f(5), 1);   // 0 < x < 10
        assert_eq!(f(15), 2);  // x >= 10
    }

    #[test]
    fn test_compile_abs() {
        let mut compiler = Compiler::new().unwrap();
        let (params, body) = parse_fn("(fn [x] (if (neg? x) (- 0 x) x))");
        let ptr = compiler.compile_numeric_fn("abs", &params, &body).unwrap();
        let abs: extern "C" fn(i64) -> i64 = unsafe { std::mem::transmute(ptr) };

        assert_eq!(abs(5), 5);
        assert_eq!(abs(-5), 5);
        assert_eq!(abs(0), 0);
    }

    #[test]
    fn test_compile_max() {
        let mut compiler = Compiler::new().unwrap();
        let (params, body) = parse_fn("(fn [a b] (if (> a b) a b))");
        let ptr = compiler.compile_numeric_fn("max", &params, &body).unwrap();
        let max: extern "C" fn(i64, i64) -> i64 = unsafe { std::mem::transmute(ptr) };

        assert_eq!(max(5, 3), 5);
        assert_eq!(max(3, 5), 5);
        assert_eq!(max(4, 4), 4);
    }

    #[test]
    fn test_compile_zero_check() {
        let mut compiler = Compiler::new().unwrap();
        let (params, body) = parse_fn("(fn [x] (zero? x))");
        let ptr = compiler.compile_numeric_fn("is_zero", &params, &body).unwrap();
        let is_zero: extern "C" fn(i64) -> i64 = unsafe { std::mem::transmute(ptr) };

        assert_eq!(is_zero(0), 1);
        assert_eq!(is_zero(1), 0);
        assert_eq!(is_zero(-1), 0);
    }

    #[test]
    fn test_compile_do() {
        let mut compiler = Compiler::new().unwrap();
        let (params, body) = parse_fn("(fn [x] (do (+ x 1) (+ x 2) (+ x 3)))");
        let ptr = compiler.compile_numeric_fn("do_test", &params, &body).unwrap();
        let f: extern "C" fn(i64) -> i64 = unsafe { std::mem::transmute(ptr) };

        // Should return the last expression
        assert_eq!(f(10), 13); // 10 + 3
    }

    #[test]
    fn test_compile_loop_factorial() {
        let mut compiler = Compiler::new().unwrap();
        let (params, body) = parse_fn(
            "(fn [n] (loop [i n acc 1] (if (<= i 1) acc (recur (dec i) (* acc i)))))"
        );
        let ptr = compiler.compile_numeric_fn("factorial", &params, &body).unwrap();
        let factorial: extern "C" fn(i64) -> i64 = unsafe { std::mem::transmute(ptr) };

        assert_eq!(factorial(0), 1);
        assert_eq!(factorial(1), 1);
        assert_eq!(factorial(5), 120);
        assert_eq!(factorial(10), 3628800);
        assert_eq!(factorial(20), 2432902008176640000);
    }

    #[test]
    fn test_compile_loop_fibonacci() {
        let mut compiler = Compiler::new().unwrap();
        let (params, body) = parse_fn(
            "(fn [n] (loop [a 0 b 1 i n] (if (<= i 0) a (recur b (+ a b) (dec i)))))"
        );
        let ptr = compiler.compile_numeric_fn("fib", &params, &body).unwrap();
        let fib: extern "C" fn(i64) -> i64 = unsafe { std::mem::transmute(ptr) };

        assert_eq!(fib(0), 0);
        assert_eq!(fib(1), 1);
        assert_eq!(fib(2), 1);
        assert_eq!(fib(10), 55);
        assert_eq!(fib(20), 6765);
        assert_eq!(fib(40), 102334155);
    }

    #[test]
    fn test_compile_loop_sum() {
        let mut compiler = Compiler::new().unwrap();
        let (params, body) = parse_fn(
            "(fn [n] (loop [i n sum 0] (if (<= i 0) sum (recur (dec i) (+ sum i)))))"
        );
        let ptr = compiler.compile_numeric_fn("sum_to_n", &params, &body).unwrap();
        let sum: extern "C" fn(i64) -> i64 = unsafe { std::mem::transmute(ptr) };

        assert_eq!(sum(0), 0);
        assert_eq!(sum(1), 1);
        assert_eq!(sum(10), 55);  // 1+2+...+10 = 55
        assert_eq!(sum(100), 5050);
    }

    #[test]
    fn test_compile_loop_performance() {
        use std::time::Instant;

        let mut compiler = Compiler::new().unwrap();
        let (params, body) = parse_fn(
            "(fn [n] (loop [a 0 b 1 i n] (if (<= i 0) a (recur b (+ a b) (dec i)))))"
        );
        let ptr = compiler.compile_numeric_fn("fib_perf", &params, &body).unwrap();
        let fib: extern "C" fn(i64) -> i64 = unsafe { std::mem::transmute(ptr) };

        // Warm up
        let _ = fib(40);

        // Benchmark fib(40) * 1000
        let start = Instant::now();
        let iterations = 1000;
        for _ in 0..iterations {
            let _ = fib(40);
        }
        let elapsed = start.elapsed();

        println!(
            "Compiled loop fib(40) x {}: {:?} ({:?} per call)",
            iterations,
            elapsed,
            elapsed / iterations
        );

        // Should be very fast - native code speed
        assert!(elapsed.as_millis() < 100, "fib(40) too slow: {:?}", elapsed);
    }

    // Tagged value tests
    #[test]
    fn test_tag_integer() {
        // Test basic tagging/untagging
        assert_eq!(untag_integer(tag_integer(0)), 0);
        assert_eq!(untag_integer(tag_integer(42)), 42);
        assert_eq!(untag_integer(tag_integer(-1)), -1);
        assert_eq!(untag_integer(tag_integer(-42)), -42);
        assert_eq!(untag_integer(tag_integer(1_000_000)), 1_000_000);
        assert_eq!(untag_integer(tag_integer(-1_000_000)), -1_000_000);

        // Large values (within 48-bit range)
        let large = 1_000_000_000_000i64;
        assert_eq!(untag_integer(tag_integer(large)), large);
        assert_eq!(untag_integer(tag_integer(-large)), -large);
    }

    #[test]
    fn test_is_tagged_integer() {
        assert!(is_tagged_integer(tag_integer(0)));
        assert!(is_tagged_integer(tag_integer(42)));
        assert!(is_tagged_integer(tag_integer(-1)));

        // These should NOT be integers
        assert!(!is_tagged_integer(NIL));
        assert!(!is_tagged_integer(TRUE));
        assert!(!is_tagged_integer(FALSE));
        assert!(!is_tagged_integer(0)); // Raw 0 is a double, not a tagged integer
    }

    #[test]
    fn test_tag_boolean() {
        assert_eq!(tag_boolean(true), TRUE);
        assert_eq!(tag_boolean(false), FALSE);
        assert_ne!(tag_boolean(true), tag_boolean(false));
    }

    #[test]
    fn test_tag_nil() {
        assert_eq!(tag_nil(), NIL);
        assert!(!is_tagged_integer(tag_nil()));
    }

    // =========================================================================
    // Native Function Call Tests - Seamless Rust Integration!
    // =========================================================================

    #[test]
    fn test_native_sqrt() {
        // Test calling Rust sqrt from JIT code - DIRECT CALL, NO FFI OVERHEAD!
        let mut compiler = Compiler::new().unwrap();
        let (params, body) = parse_fn("(fn [x] (sqrt x))");
        let ptr = compiler.compile_numeric_fn("call_sqrt", &params, &body).unwrap();
        let call_sqrt: extern "C" fn(i64) -> i64 = unsafe { std::mem::transmute(ptr) };

        assert_eq!(call_sqrt(0), 0);
        assert_eq!(call_sqrt(1), 1);
        assert_eq!(call_sqrt(4), 2);
        assert_eq!(call_sqrt(16), 4);
        assert_eq!(call_sqrt(100), 10);
        assert_eq!(call_sqrt(144), 12);
    }

    #[test]
    fn test_native_abs() {
        let mut compiler = Compiler::new().unwrap();
        let (params, body) = parse_fn("(fn [x] (abs x))");
        let ptr = compiler.compile_numeric_fn("call_abs", &params, &body).unwrap();
        let call_abs: extern "C" fn(i64) -> i64 = unsafe { std::mem::transmute(ptr) };

        assert_eq!(call_abs(5), 5);
        assert_eq!(call_abs(-5), 5);
        assert_eq!(call_abs(0), 0);
        assert_eq!(call_abs(-100), 100);
    }

    #[test]
    fn test_native_pow() {
        let mut compiler = Compiler::new().unwrap();
        let (params, body) = parse_fn("(fn [base exp] (pow base exp))");
        let ptr = compiler.compile_numeric_fn("call_pow", &params, &body).unwrap();
        let call_pow: extern "C" fn(i64, i64) -> i64 = unsafe { std::mem::transmute(ptr) };

        assert_eq!(call_pow(2, 0), 1);
        assert_eq!(call_pow(2, 1), 2);
        assert_eq!(call_pow(2, 10), 1024);
        assert_eq!(call_pow(3, 3), 27);
        assert_eq!(call_pow(10, 3), 1000);
    }

    #[test]
    fn test_native_mod_quot() {
        let mut compiler = Compiler::new().unwrap();

        // Test mod
        let (params, body) = parse_fn("(fn [a b] (mod a b))");
        let ptr = compiler.compile_numeric_fn("call_mod", &params, &body).unwrap();
        let call_mod: extern "C" fn(i64, i64) -> i64 = unsafe { std::mem::transmute(ptr) };
        assert_eq!(call_mod(10, 3), 1);
        assert_eq!(call_mod(10, 5), 0);
        assert_eq!(call_mod(17, 4), 1);

        // Test quot (need a new compiler for separate function)
        let mut compiler2 = Compiler::new().unwrap();
        let (params2, body2) = parse_fn("(fn [a b] (quot a b))");
        let ptr2 = compiler2.compile_numeric_fn("call_quot", &params2, &body2).unwrap();
        let call_quot: extern "C" fn(i64, i64) -> i64 = unsafe { std::mem::transmute(ptr2) };
        assert_eq!(call_quot(10, 3), 3);
        assert_eq!(call_quot(10, 5), 2);
        assert_eq!(call_quot(17, 4), 4);
    }

    #[test]
    fn test_native_min_max() {
        let mut compiler = Compiler::new().unwrap();
        let (params, body) = parse_fn("(fn [a b] (min a b))");
        let ptr = compiler.compile_numeric_fn("call_min", &params, &body).unwrap();
        let call_min: extern "C" fn(i64, i64) -> i64 = unsafe { std::mem::transmute(ptr) };

        assert_eq!(call_min(3, 5), 3);
        assert_eq!(call_min(5, 3), 3);
        assert_eq!(call_min(-1, 1), -1);

        let mut compiler2 = Compiler::new().unwrap();
        let (params2, body2) = parse_fn("(fn [a b] (max a b))");
        let ptr2 = compiler2.compile_numeric_fn("call_max", &params2, &body2).unwrap();
        let call_max: extern "C" fn(i64, i64) -> i64 = unsafe { std::mem::transmute(ptr2) };

        assert_eq!(call_max(3, 5), 5);
        assert_eq!(call_max(5, 3), 5);
        assert_eq!(call_max(-1, 1), 1);
    }

    #[test]
    fn test_native_bit_ops() {
        let mut compiler = Compiler::new().unwrap();

        // Test bit-and
        let (params, body) = parse_fn("(fn [a b] (bit-and a b))");
        let ptr = compiler.compile_numeric_fn("call_bit_and", &params, &body).unwrap();
        let call_bit_and: extern "C" fn(i64, i64) -> i64 = unsafe { std::mem::transmute(ptr) };
        assert_eq!(call_bit_and(0b1010, 0b1100), 0b1000);
        assert_eq!(call_bit_and(0xFF, 0x0F), 0x0F);

        // Test bit-or
        let mut compiler2 = Compiler::new().unwrap();
        let (params2, body2) = parse_fn("(fn [a b] (bit-or a b))");
        let ptr2 = compiler2.compile_numeric_fn("call_bit_or", &params2, &body2).unwrap();
        let call_bit_or: extern "C" fn(i64, i64) -> i64 = unsafe { std::mem::transmute(ptr2) };
        assert_eq!(call_bit_or(0b1010, 0b1100), 0b1110);

        // Test bit-shift-left
        let mut compiler3 = Compiler::new().unwrap();
        let (params3, body3) = parse_fn("(fn [a n] (bit-shift-left a n))");
        let ptr3 = compiler3.compile_numeric_fn("call_bsl", &params3, &body3).unwrap();
        let call_bsl: extern "C" fn(i64, i64) -> i64 = unsafe { std::mem::transmute(ptr3) };
        assert_eq!(call_bsl(1, 4), 16);
        assert_eq!(call_bsl(1, 8), 256);
    }

    #[test]
    fn test_native_in_expression() {
        // Test using native functions in complex expressions
        let mut compiler = Compiler::new().unwrap();

        // (fn [x] (+ (sqrt x) 10))
        let (params, body) = parse_fn("(fn [x] (+ (sqrt x) 10))");
        let ptr = compiler.compile_numeric_fn("sqrt_plus_10", &params, &body).unwrap();
        let f: extern "C" fn(i64) -> i64 = unsafe { std::mem::transmute(ptr) };

        assert_eq!(f(16), 14); // sqrt(16) + 10 = 4 + 10 = 14
        assert_eq!(f(100), 20); // sqrt(100) + 10 = 10 + 10 = 20
    }

    #[test]
    fn test_native_nested() {
        // Test nested native function calls
        let mut compiler = Compiler::new().unwrap();

        // (fn [x] (sqrt (abs x)))
        let (params, body) = parse_fn("(fn [x] (sqrt (abs x)))");
        let ptr = compiler.compile_numeric_fn("sqrt_abs", &params, &body).unwrap();
        let f: extern "C" fn(i64) -> i64 = unsafe { std::mem::transmute(ptr) };

        assert_eq!(f(16), 4);
        assert_eq!(f(-16), 4); // sqrt(abs(-16)) = sqrt(16) = 4
        assert_eq!(f(-100), 10);
    }

    #[test]
    fn test_native_in_loop() {
        // Test calling native functions inside a loop
        let mut compiler = Compiler::new().unwrap();

        // Sum of square roots: (fn [n] (loop [i n sum 0] (if (<= i 0) sum (recur (dec i) (+ sum (sqrt i))))))
        let (params, body) = parse_fn(
            "(fn [n] (loop [i n sum 0] (if (<= i 0) sum (recur (dec i) (+ sum (sqrt i))))))"
        );
        let ptr = compiler.compile_numeric_fn("sum_sqrt", &params, &body).unwrap();
        let sum_sqrt: extern "C" fn(i64) -> i64 = unsafe { std::mem::transmute(ptr) };

        // sqrt(1) + sqrt(2) + sqrt(3) + sqrt(4) = 1 + 1 + 1 + 2 = 5
        assert_eq!(sum_sqrt(4), 5);
        // sqrt(1) + ... + sqrt(9) = 1 + 1 + 1 + 2 + 2 + 2 + 2 + 2 + 3 = 16
        assert_eq!(sum_sqrt(9), 16);
    }

    #[test]
    fn test_native_performance() {
        use std::time::Instant;

        let mut compiler = Compiler::new().unwrap();

        // Call sqrt in a tight loop
        let (params, body) = parse_fn(
            "(fn [n] (loop [i n sum 0] (if (<= i 0) sum (recur (dec i) (+ sum (sqrt i))))))"
        );
        let ptr = compiler.compile_numeric_fn("perf_sqrt", &params, &body).unwrap();
        let sum_sqrt: extern "C" fn(i64) -> i64 = unsafe { std::mem::transmute(ptr) };

        // Warm up
        let _ = sum_sqrt(1000);

        // Benchmark
        let start = Instant::now();
        let iterations = 10000;
        for _ in 0..iterations {
            let _ = sum_sqrt(100);
        }
        let elapsed = start.elapsed();

        println!(
            "Native sqrt in loop (100 iterations) x {}: {:?} ({:?} per call)",
            iterations,
            elapsed,
            elapsed / iterations as u32
        );

        // Should be very fast - direct native calls!
        assert!(elapsed.as_millis() < 500, "Native calls too slow: {:?}", elapsed);
    }

    #[test]
    fn test_distance_formula() {
        // Real-world example: Euclidean distance (integer approximation)
        // distance(x1, y1, x2, y2) = sqrt((x2-x1)^2 + (y2-y1)^2)
        let mut compiler = Compiler::new().unwrap();

        let (params, body) = parse_fn(
            "(fn [x1 y1 x2 y2] (sqrt (+ (* (- x2 x1) (- x2 x1)) (* (- y2 y1) (- y2 y1)))))"
        );
        let ptr = compiler.compile_numeric_fn("distance", &params, &body).unwrap();
        let distance: extern "C" fn(i64, i64, i64, i64) -> i64 = unsafe { std::mem::transmute(ptr) };

        // distance(0, 0, 3, 4) = sqrt(9 + 16) = sqrt(25) = 5
        assert_eq!(distance(0, 0, 3, 4), 5);
        // distance(0, 0, 6, 8) = sqrt(36 + 64) = sqrt(100) = 10
        assert_eq!(distance(0, 0, 6, 8), 10);
        // distance(1, 1, 4, 5) = sqrt(9 + 16) = sqrt(25) = 5
        assert_eq!(distance(1, 1, 4, 5), 5);
    }
}
