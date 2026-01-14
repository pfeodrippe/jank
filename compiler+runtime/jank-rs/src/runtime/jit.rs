//! Cranelift JIT compilation for jank-rs
//!
//! This module provides JIT compilation of Clojure functions to native code
//! using Cranelift. Hot functions are compiled for better performance.

use std::collections::HashMap;

use cranelift::prelude::*;
use cranelift_jit::{JITBuilder, JITModule};
use cranelift_module::{Linkage, Module};

use crate::error::{JankError, JankResult};

/// A compiled native function pointer
type CompiledFn = *const u8;

/// Threshold for JIT compilation (compile after this many calls)
const JIT_THRESHOLD: usize = 100;

/// The JIT runtime that compiles and executes native code
pub struct JitRuntime {
    /// The Cranelift JIT module
    module: JITModule,
    /// Context for building functions
    ctx: codegen::Context,
    /// Function builder context (reusable)
    func_ctx: FunctionBuilderContext,
    /// Cache of compiled functions by name
    compiled: HashMap<String, CompiledFn>,
    /// Call counts for tiered compilation
    call_counts: HashMap<String, usize>,
}

impl JitRuntime {
    /// Create a new JIT runtime
    pub fn new() -> JankResult<Self> {
        // Set up the Cranelift JIT
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

        let builder = JITBuilder::with_isa(isa, cranelift_module::default_libcall_names());
        let module = JITModule::new(builder);
        let ctx = module.make_context();

        Ok(JitRuntime {
            module,
            ctx,
            func_ctx: FunctionBuilderContext::new(),
            compiled: HashMap::new(),
            call_counts: HashMap::new(),
        })
    }

    /// Check if a function should be JIT compiled based on call count
    pub fn should_compile(&mut self, name: &str) -> bool {
        let count = self.call_counts.entry(name.to_string()).or_insert(0);
        *count += 1;
        *count >= JIT_THRESHOLD && !self.compiled.contains_key(name)
    }

    /// Get call count for a function
    pub fn get_call_count(&self, name: &str) -> usize {
        *self.call_counts.get(name).unwrap_or(&0)
    }

    /// Check if a function is already compiled
    pub fn is_compiled(&self, name: &str) -> bool {
        self.compiled.contains_key(name)
    }

    /// Compile a simple add(a, b) -> a + b function
    pub fn compile_binary_op<F>(&mut self, name: &str, op: F) -> JankResult<extern "C" fn(i64, i64) -> i64>
    where
        F: FnOnce(&mut FunctionBuilder, cranelift::prelude::Value, cranelift::prelude::Value) -> cranelift::prelude::Value,
    {
        // Define the function signature: fn(i64, i64) -> i64
        let mut sig = self.module.make_signature();
        sig.params.push(AbiParam::new(types::I64));
        sig.params.push(AbiParam::new(types::I64));
        sig.returns.push(AbiParam::new(types::I64));

        // Declare the function
        let func_id = self.module
            .declare_function(name, Linkage::Local, &sig)
            .map_err(|e| JankError::eval(format!("Failed to declare function: {}", e)))?;

        // Set up context
        self.ctx.func.signature = sig;

        // Build the function body
        {
            let mut builder = FunctionBuilder::new(&mut self.ctx.func, &mut self.func_ctx);

            // Create entry block
            let entry_block = builder.create_block();
            builder.append_block_params_for_function_params(entry_block);
            builder.switch_to_block(entry_block);
            builder.seal_block(entry_block);

            // Get parameters
            let a = builder.block_params(entry_block)[0];
            let b = builder.block_params(entry_block)[1];

            // Apply operation
            let result = op(&mut builder, a, b);

            // Return result
            builder.ins().return_(&[result]);

            builder.finalize();
        }

        // Compile the function
        self.module
            .define_function(func_id, &mut self.ctx)
            .map_err(|e| JankError::eval(format!("Failed to define function: {}", e)))?;

        self.module.clear_context(&mut self.ctx);

        self.module
            .finalize_definitions()
            .map_err(|e| JankError::eval(format!("Failed to finalize: {}", e)))?;

        // Get the function pointer
        let code_ptr = self.module.get_finalized_function(func_id);

        // Store it
        self.compiled.insert(name.to_string(), code_ptr);

        // Return as callable function
        Ok(unsafe { std::mem::transmute(code_ptr) })
    }

    /// Compile addition
    pub fn compile_add(&mut self, name: &str) -> JankResult<extern "C" fn(i64, i64) -> i64> {
        self.compile_binary_op(name, |builder, a, b| builder.ins().iadd(a, b))
    }

    /// Compile subtraction
    pub fn compile_sub(&mut self, name: &str) -> JankResult<extern "C" fn(i64, i64) -> i64> {
        self.compile_binary_op(name, |builder, a, b| builder.ins().isub(a, b))
    }

    /// Compile multiplication
    pub fn compile_mul(&mut self, name: &str) -> JankResult<extern "C" fn(i64, i64) -> i64> {
        self.compile_binary_op(name, |builder, a, b| builder.ins().imul(a, b))
    }

    /// Compile factorial function: fn(n: i64) -> i64
    /// Implements: (loop [i n acc 1] (if (<= i 1) acc (recur (dec i) (* acc i))))
    pub fn compile_factorial(&mut self, name: &str) -> JankResult<extern "C" fn(i64) -> i64> {
        let mut sig = self.module.make_signature();
        sig.params.push(AbiParam::new(types::I64));
        sig.returns.push(AbiParam::new(types::I64));

        let func_id = self.module
            .declare_function(name, Linkage::Local, &sig)
            .map_err(|e| JankError::eval(format!("Failed to declare function: {}", e)))?;

        self.ctx.func.signature = sig;

        {
            let mut builder = FunctionBuilder::new(&mut self.ctx.func, &mut self.func_ctx);

            // Block structure:
            // entry -> loop_header -> (loop_body -> loop_header) | exit

            let entry = builder.create_block();
            builder.append_block_params_for_function_params(entry);

            let loop_header = builder.create_block();
            builder.append_block_param(loop_header, types::I64); // i
            builder.append_block_param(loop_header, types::I64); // acc

            let loop_body = builder.create_block();

            let exit = builder.create_block();
            builder.append_block_param(exit, types::I64); // result

            // Entry: initialize loop with (n, 1)
            builder.switch_to_block(entry);
            let n = builder.block_params(entry)[0];
            let one = builder.ins().iconst(types::I64, 1);
            builder.ins().jump(loop_header, &[n, one]);
            builder.seal_block(entry);

            // Loop header: check condition
            builder.switch_to_block(loop_header);
            let i = builder.block_params(loop_header)[0];
            let acc = builder.block_params(loop_header)[1];

            // if i <= 1, exit with acc; else continue to body
            let one2 = builder.ins().iconst(types::I64, 1);
            let cond = builder.ins().icmp(IntCC::SignedLessThanOrEqual, i, one2);
            builder.ins().brif(cond, exit, &[acc], loop_body, &[]);

            // Loop body: compute new values and jump back
            builder.switch_to_block(loop_body);
            builder.seal_block(loop_body);

            // i_new = i - 1
            let one3 = builder.ins().iconst(types::I64, 1);
            let i_new = builder.ins().isub(i, one3);
            // acc_new = acc * i
            let acc_new = builder.ins().imul(acc, i);
            // recur
            builder.ins().jump(loop_header, &[i_new, acc_new]);

            // Seal loop_header after all predecessors are known
            builder.seal_block(loop_header);

            // Exit: return result
            builder.switch_to_block(exit);
            let result = builder.block_params(exit)[0];
            builder.ins().return_(&[result]);
            builder.seal_block(exit);

            builder.finalize();
        }

        self.module.define_function(func_id, &mut self.ctx)
            .map_err(|e| JankError::eval(format!("Failed to define function: {}", e)))?;
        self.module.clear_context(&mut self.ctx);
        self.module.finalize_definitions()
            .map_err(|e| JankError::eval(format!("Failed to finalize: {}", e)))?;

        let code_ptr = self.module.get_finalized_function(func_id);
        self.compiled.insert(name.to_string(), code_ptr);

        Ok(unsafe { std::mem::transmute(code_ptr) })
    }

    /// Compile iterative fibonacci: fn(n: i64) -> i64
    /// Implements: (loop [a 0 b 1 i n] (if (<= i 0) a (recur b (+ a b) (dec i))))
    pub fn compile_fibonacci(&mut self, name: &str) -> JankResult<extern "C" fn(i64) -> i64> {
        let mut sig = self.module.make_signature();
        sig.params.push(AbiParam::new(types::I64));
        sig.returns.push(AbiParam::new(types::I64));

        let func_id = self.module
            .declare_function(name, Linkage::Local, &sig)
            .map_err(|e| JankError::eval(format!("Failed to declare function: {}", e)))?;

        self.ctx.func.signature = sig;

        {
            let mut builder = FunctionBuilder::new(&mut self.ctx.func, &mut self.func_ctx);

            let entry = builder.create_block();
            builder.append_block_params_for_function_params(entry);

            let loop_header = builder.create_block();
            builder.append_block_param(loop_header, types::I64); // a
            builder.append_block_param(loop_header, types::I64); // b
            builder.append_block_param(loop_header, types::I64); // i

            let loop_body = builder.create_block();

            let exit = builder.create_block();
            builder.append_block_param(exit, types::I64); // result

            // Entry: jump to loop with (0, 1, n)
            builder.switch_to_block(entry);
            let n = builder.block_params(entry)[0];
            let zero = builder.ins().iconst(types::I64, 0);
            let one = builder.ins().iconst(types::I64, 1);
            builder.ins().jump(loop_header, &[zero, one, n]);
            builder.seal_block(entry);

            // Loop header: if i <= 0, return a
            builder.switch_to_block(loop_header);
            let a = builder.block_params(loop_header)[0];
            let b = builder.block_params(loop_header)[1];
            let i = builder.block_params(loop_header)[2];
            let zero2 = builder.ins().iconst(types::I64, 0);
            let cond = builder.ins().icmp(IntCC::SignedLessThanOrEqual, i, zero2);
            builder.ins().brif(cond, exit, &[a], loop_body, &[]);

            // Loop body: recur with (b, a+b, i-1)
            builder.switch_to_block(loop_body);
            builder.seal_block(loop_body);
            let a_plus_b = builder.ins().iadd(a, b);
            let one2 = builder.ins().iconst(types::I64, 1);
            let i_dec = builder.ins().isub(i, one2);
            builder.ins().jump(loop_header, &[b, a_plus_b, i_dec]);

            builder.seal_block(loop_header);

            // Exit
            builder.switch_to_block(exit);
            let result = builder.block_params(exit)[0];
            builder.ins().return_(&[result]);
            builder.seal_block(exit);

            builder.finalize();
        }

        self.module.define_function(func_id, &mut self.ctx)
            .map_err(|e| JankError::eval(format!("Failed to define function: {}", e)))?;
        self.module.clear_context(&mut self.ctx);
        self.module.finalize_definitions()
            .map_err(|e| JankError::eval(format!("Failed to finalize: {}", e)))?;

        let code_ptr = self.module.get_finalized_function(func_id);
        self.compiled.insert(name.to_string(), code_ptr);

        Ok(unsafe { std::mem::transmute(code_ptr) })
    }

    /// Compile a sum-to-n function: fn(n: i64) -> i64
    /// Returns 0 + 1 + 2 + ... + n
    pub fn compile_sum_to_n(&mut self, name: &str) -> JankResult<extern "C" fn(i64) -> i64> {
        let mut sig = self.module.make_signature();
        sig.params.push(AbiParam::new(types::I64));
        sig.returns.push(AbiParam::new(types::I64));

        let func_id = self.module
            .declare_function(name, Linkage::Local, &sig)
            .map_err(|e| JankError::eval(format!("Failed to declare function: {}", e)))?;

        self.ctx.func.signature = sig;

        {
            let mut builder = FunctionBuilder::new(&mut self.ctx.func, &mut self.func_ctx);

            let entry = builder.create_block();
            builder.append_block_params_for_function_params(entry);

            let loop_header = builder.create_block();
            builder.append_block_param(loop_header, types::I64); // i
            builder.append_block_param(loop_header, types::I64); // sum

            let loop_body = builder.create_block();

            let exit = builder.create_block();
            builder.append_block_param(exit, types::I64);

            // Entry: start loop with (n, 0)
            builder.switch_to_block(entry);
            let n = builder.block_params(entry)[0];
            let zero = builder.ins().iconst(types::I64, 0);
            builder.ins().jump(loop_header, &[n, zero]);
            builder.seal_block(entry);

            // Loop header: if i <= 0, return sum
            builder.switch_to_block(loop_header);
            let i = builder.block_params(loop_header)[0];
            let sum = builder.block_params(loop_header)[1];
            let zero2 = builder.ins().iconst(types::I64, 0);
            let cond = builder.ins().icmp(IntCC::SignedLessThanOrEqual, i, zero2);
            builder.ins().brif(cond, exit, &[sum], loop_body, &[]);

            // Loop body: recur with (i-1, sum+i)
            builder.switch_to_block(loop_body);
            builder.seal_block(loop_body);
            let one = builder.ins().iconst(types::I64, 1);
            let i_dec = builder.ins().isub(i, one);
            let sum_new = builder.ins().iadd(sum, i);
            builder.ins().jump(loop_header, &[i_dec, sum_new]);

            builder.seal_block(loop_header);

            // Exit
            builder.switch_to_block(exit);
            let result = builder.block_params(exit)[0];
            builder.ins().return_(&[result]);
            builder.seal_block(exit);

            builder.finalize();
        }

        self.module.define_function(func_id, &mut self.ctx)
            .map_err(|e| JankError::eval(format!("Failed to define function: {}", e)))?;
        self.module.clear_context(&mut self.ctx);
        self.module.finalize_definitions()
            .map_err(|e| JankError::eval(format!("Failed to finalize: {}", e)))?;

        let code_ptr = self.module.get_finalized_function(func_id);
        self.compiled.insert(name.to_string(), code_ptr);

        Ok(unsafe { std::mem::transmute(code_ptr) })
    }
}

impl Default for JitRuntime {
    fn default() -> Self {
        Self::new().expect("Failed to create JIT runtime")
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Instant;

    #[test]
    fn test_jit_add() {
        let mut jit = JitRuntime::new().unwrap();
        let add = jit.compile_add("test_add").unwrap();

        assert_eq!(add(1, 2), 3);
        assert_eq!(add(100, 200), 300);
        assert_eq!(add(-5, 10), 5);
        assert_eq!(add(i64::MAX - 1, 1), i64::MAX);
    }

    #[test]
    fn test_jit_sub() {
        let mut jit = JitRuntime::new().unwrap();
        let sub = jit.compile_sub("test_sub").unwrap();

        assert_eq!(sub(10, 3), 7);
        assert_eq!(sub(5, 10), -5);
        assert_eq!(sub(0, 0), 0);
    }

    #[test]
    fn test_jit_mul() {
        let mut jit = JitRuntime::new().unwrap();
        let mul = jit.compile_mul("test_mul").unwrap();

        assert_eq!(mul(3, 4), 12);
        assert_eq!(mul(-5, 10), -50);
        assert_eq!(mul(0, 1000), 0);
    }

    #[test]
    fn test_jit_factorial() {
        let mut jit = JitRuntime::new().unwrap();
        let factorial = jit.compile_factorial("factorial").unwrap();

        assert_eq!(factorial(0), 1);
        assert_eq!(factorial(1), 1);
        assert_eq!(factorial(5), 120);
        assert_eq!(factorial(10), 3628800);
        assert_eq!(factorial(20), 2432902008176640000);
    }

    #[test]
    fn test_jit_fibonacci() {
        let mut jit = JitRuntime::new().unwrap();
        let fib = jit.compile_fibonacci("fibonacci").unwrap();

        assert_eq!(fib(0), 0);
        assert_eq!(fib(1), 1);
        assert_eq!(fib(2), 1);
        assert_eq!(fib(10), 55);
        assert_eq!(fib(20), 6765);
        assert_eq!(fib(40), 102334155);
    }

    #[test]
    fn test_jit_fibonacci_performance() {
        let mut jit = JitRuntime::new().unwrap();
        let fib = jit.compile_fibonacci("fib_perf").unwrap();

        // Warm up
        let _ = fib(40);

        // Benchmark
        let start = Instant::now();
        let iterations = 1000;
        for _ in 0..iterations {
            let _ = fib(40);
        }
        let elapsed = start.elapsed();

        println!(
            "JIT fib(40) x {}: {:?} ({:?} per call)",
            iterations,
            elapsed,
            elapsed / iterations
        );

        // Should be very fast - less than 1ms per call
        assert!(elapsed.as_millis() < 100, "fib(40) too slow: {:?}", elapsed);
    }

    #[test]
    fn test_jit_sum_to_n() {
        let mut jit = JitRuntime::new().unwrap();
        let sum = jit.compile_sum_to_n("sum_to_n").unwrap();

        assert_eq!(sum(0), 0);
        assert_eq!(sum(1), 1);
        assert_eq!(sum(10), 55);  // 1+2+...+10 = 55
        assert_eq!(sum(100), 5050);
    }

    #[test]
    fn test_jit_sum_performance() {
        let mut jit = JitRuntime::new().unwrap();
        let sum = jit.compile_sum_to_n("sum_perf").unwrap();

        let start = Instant::now();
        let result = sum(100_000_000);
        let elapsed = start.elapsed();

        println!("JIT sum(100M): {} in {:?}", result, elapsed);

        // Should compute 100 million iterations very quickly
        assert!(elapsed.as_millis() < 500, "sum too slow: {:?}", elapsed);
        assert_eq!(result, 5000000050000000); // n*(n+1)/2 for n=100M
    }
}
