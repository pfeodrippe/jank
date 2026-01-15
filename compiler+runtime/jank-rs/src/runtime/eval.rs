//! Evaluator for jank-rs
//!
//! This module implements the interpreter that evaluates Clojure forms.
//! Supports namespaces via (ns ...) and (require ...) forms.

use std::collections::HashMap;
use std::sync::Arc;
use std::path::PathBuf;

use crate::types::{Value, Symbol, Function, Arity, List, Keyword};
use crate::error::{JankError, JankResult};
use crate::runtime::env::Environment;
use crate::runtime::compiler::Compiler;
use crate::runtime::namespace::NamespaceRegistry;
use crate::reader::read_string;

/// Maximum recursion depth before stack overflow
const MAX_RECURSION_DEPTH: usize = 10000;

/// JIT compilation threshold (compile after this many calls)
/// Set to 1 for immediate compilation - no interpreter overhead!
const JIT_THRESHOLD: usize = 1;

/// A compiled function with its metadata
struct CompiledFunction {
    /// Function pointer (transmuted to correct arity)
    ptr: *const u8,
    /// Number of parameters
    arity: usize,
}

/// The evaluator for Clojure expressions
pub struct Evaluator {
    /// Global environment with core functions
    global_env: Arc<Environment>,
    /// Current recursion depth
    depth: usize,
    /// JIT compiler (lazy-initialized)
    compiler: Option<Compiler>,
    /// Compiled functions cache (function name -> compiled fn)
    compiled: HashMap<String, CompiledFunction>,
    /// Call counts for functions
    call_counts: HashMap<String, usize>,
    /// Namespace registry for managing namespaces
    namespaces: NamespaceRegistry,
    /// Gensym counter for unique symbol generation
    gensym_counter: usize,
}

impl Evaluator {
    /// Create a new evaluator with standard library
    pub fn new() -> Self {
        let global_env = Arc::new(Environment::new());

        // Load native core functions
        crate::runtime::core::load_core(&global_env);

        let mut evaluator = Evaluator {
            global_env,
            depth: 0,
            compiler: None,
            compiled: HashMap::new(),
            call_counts: HashMap::new(),
            namespaces: NamespaceRegistry::new(),
            gensym_counter: 0,
        };

        // Auto-load clojure.core (like Clojure does)
        evaluator.load_clojure_core();

        evaluator
    }

    /// Load clojure.core and refer all its symbols into the current namespace
    fn load_clojure_core(&mut self) {
        // Try to load clojure/core.jrs
        if let Err(e) = self.load_namespace("clojure.core") {
            // If file doesn't exist, that's OK - we'll just use native functions
            // But print other errors for debugging
            eprintln!("Warning: Failed to load clojure.core: {:?}", e);
            return;
        }

        // Refer all symbols from clojure.core into the current namespace (user)
        if let Some(core_ns) = self.namespaces.get("clojure.core") {
            let defs: Vec<String> = core_ns.iter_defs().map(|(k, _)| k.clone()).collect();
            for name in defs {
                self.namespaces.current_mut().add_refer(&name, "clojure.core", &name);
            }
        }
    }

    /// Create evaluator with a custom environment
    pub fn with_env(env: Arc<Environment>) -> Self {
        Evaluator {
            global_env: env,
            depth: 0,
            compiler: None,
            compiled: HashMap::new(),
            call_counts: HashMap::new(),
            namespaces: NamespaceRegistry::new(),
            gensym_counter: 0,
        }
    }

    /// Get the namespace registry
    pub fn namespaces(&self) -> &NamespaceRegistry {
        &self.namespaces
    }

    /// Get the namespace registry mutably
    pub fn namespaces_mut(&mut self) -> &mut NamespaceRegistry {
        &mut self.namespaces
    }

    /// Add a source path for loading .jrs files
    pub fn add_source_path(&mut self, path: impl Into<PathBuf>) {
        self.namespaces.add_source_path(path);
    }

    /// Generate a unique gensym counter value
    fn gensym_counter(&mut self) -> usize {
        let c = self.gensym_counter;
        self.gensym_counter += 1;
        c
    }

    /// Get or initialize the JIT compiler
    fn get_compiler(&mut self) -> JankResult<&mut Compiler> {
        if self.compiler.is_none() {
            self.compiler = Some(Compiler::new()?);
        }
        Ok(self.compiler.as_mut().unwrap())
    }

    /// Check if a function should be JIT compiled based on call count
    fn should_compile(&mut self, name: &str) -> bool {
        let count = self.call_counts.entry(name.to_string()).or_insert(0);
        *count += 1;
        *count == JIT_THRESHOLD && !self.compiled.contains_key(name)
    }

    /// Try to JIT compile a function if it's eligible
    fn try_jit_compile(&mut self, name: &str, params: &[Symbol], body: &Value) -> Option<()> {
        // Check if body is JIT-eligible (only numeric operations)
        if !is_jit_eligible(body) {
            return None;
        }

        // Try to compile
        let compiler = self.get_compiler().ok()?;
        let ptr = compiler.compile_numeric_fn(name, params, body).ok()?;

        self.compiled.insert(name.to_string(), CompiledFunction {
            ptr,
            arity: params.len(),
        });

        Some(())
    }

    /// Call a compiled function with i64 args
    fn call_compiled(&self, name: &str, args: &[Value]) -> Option<Value> {
        let compiled = self.compiled.get(name)?;
        if args.len() != compiled.arity {
            return None;
        }

        // Convert args to i64
        let int_args: Vec<i64> = args.iter()
            .filter_map(|v| match v {
                Value::Integer(n) => Some(*n),
                _ => None,
            })
            .collect();

        if int_args.len() != args.len() {
            return None; // Not all args are integers
        }

        // Call based on arity
        let result: i64 = unsafe {
            match compiled.arity {
                0 => {
                    let f: extern "C" fn() -> i64 = std::mem::transmute(compiled.ptr);
                    f()
                }
                1 => {
                    let f: extern "C" fn(i64) -> i64 = std::mem::transmute(compiled.ptr);
                    f(int_args[0])
                }
                2 => {
                    let f: extern "C" fn(i64, i64) -> i64 = std::mem::transmute(compiled.ptr);
                    f(int_args[0], int_args[1])
                }
                3 => {
                    let f: extern "C" fn(i64, i64, i64) -> i64 = std::mem::transmute(compiled.ptr);
                    f(int_args[0], int_args[1], int_args[2])
                }
                4 => {
                    let f: extern "C" fn(i64, i64, i64, i64) -> i64 = std::mem::transmute(compiled.ptr);
                    f(int_args[0], int_args[1], int_args[2], int_args[3])
                }
                _ => return None, // Too many args
            }
        };

        Some(Value::Integer(result))
    }

    /// Get the global environment
    pub fn global_env(&self) -> Arc<Environment> {
        Arc::clone(&self.global_env)
    }

    /// Check if a function has been JIT compiled (for testing)
    #[cfg(test)]
    pub fn is_compiled(&self, name: &str) -> bool {
        self.compiled.contains_key(name)
    }

    /// Get the call count for a function (for testing)
    #[cfg(test)]
    pub fn call_count(&self, name: &str) -> usize {
        self.call_counts.get(name).copied().unwrap_or(0)
    }

    /// Get the function pointer for a compiled function (for testing/benchmarks)
    #[cfg(test)]
    pub fn get_compiled_ptr(&self, name: &str) -> Option<*const u8> {
        self.compiled.get(name).map(|f| f.ptr)
    }

    /// Evaluate a form in the global environment
    pub fn eval(&mut self, form: &Value) -> JankResult<Value> {
        self.eval_in_env(form, Arc::clone(&self.global_env))
    }

    /// Evaluate a form in a specific environment
    pub fn eval_in_env(&mut self, form: &Value, env: Arc<Environment>) -> JankResult<Value> {
        // Check recursion depth
        self.depth += 1;
        if self.depth > MAX_RECURSION_DEPTH {
            self.depth -= 1;
            return Err(JankError::StackOverflow);
        }

        let result = self.eval_form(form, env);
        self.depth -= 1;
        result
    }

    /// Internal evaluation
    fn eval_form(&mut self, form: &Value, env: Arc<Environment>) -> JankResult<Value> {
        match form {
            // Self-evaluating forms
            Value::Nil | Value::Bool(_) | Value::Integer(_) | Value::Float(_) |
            Value::String(_) | Value::Char(_) | Value::Regex(_) |
            Value::Function(_) => Ok(form.clone()),

            // Keywords evaluate to themselves
            Value::Keyword(_) => Ok(form.clone()),

            // Symbols are looked up in the environment or namespace
            Value::Symbol(sym) => {
                // First, check if it's a qualified symbol (ns/name)
                if sym.namespace().is_some() {
                    // Qualified symbol - resolve through namespace registry
                    if let Some(value) = self.namespaces.resolve(sym) {
                        return Ok(value);
                    }
                    // Also check global_env for native/ prefixed functions
                    if let Some(value) = self.global_env.lookup_symbol(sym) {
                        return Ok(value);
                    }
                    return Err(JankError::undefined_symbol(&format!("{}/{}",
                        sym.namespace().unwrap(), sym.name())));
                }

                // Try local environment first (for let bindings, fn params)
                // But NOT the global_env - we check that last as fallback for natives
                if !Arc::ptr_eq(&env, &self.global_env) {
                    if let Some(value) = env.lookup_local(sym) {
                        return Ok(value);
                    }
                }

                // Try namespace registry (for referred symbols and current ns defs)
                // This takes priority over global_env natives
                if let Some(value) = self.namespaces.resolve(sym) {
                    return Ok(value);
                }

                // Fall back to global_env for native functions
                if let Some(value) = self.global_env.lookup_symbol(sym) {
                    return Ok(value);
                }

                Err(JankError::undefined_symbol(sym.name()))
            }

            // Vectors: evaluate each element
            Value::Vector(v) => {
                let mut result = imbl::Vector::new();
                for item in v.iter() {
                    result.push_back(self.eval_in_env(item, Arc::clone(&env))?);
                }
                Ok(Value::Vector(Arc::new(result)))
            }

            // Maps: evaluate keys and values
            Value::Map(m) => {
                let mut result = imbl::HashMap::new();
                for (k, v) in m.iter() {
                    let key = self.eval_in_env(k, Arc::clone(&env))?;
                    let val = self.eval_in_env(v, Arc::clone(&env))?;
                    result.insert(key, val);
                }
                Ok(Value::Map(Arc::new(result)))
            }

            // Sets: evaluate each element
            Value::Set(s) => {
                let mut result = imbl::HashSet::new();
                for item in s.iter() {
                    result.insert(self.eval_in_env(item, Arc::clone(&env))?);
                }
                Ok(Value::Set(Arc::new(result)))
            }

            // Lists are function calls or special forms
            Value::List(list) => {
                if list.is_empty() {
                    return Ok(Value::list(vec![]));
                }

                let head = list.head().unwrap();

                // Check for special forms
                if let Value::Symbol(sym) = &head {
                    match sym.name() {
                        "quote" => return self.eval_quote(list, Arc::clone(&env)),
                        "syntax-quote" => return self.eval_syntax_quote(list, Arc::clone(&env)),
                        "unquote" => return Err(JankError::eval("unquote outside of syntax-quote")),
                        "unquote-splicing" => return Err(JankError::eval("unquote-splicing outside of syntax-quote")),
                        "if" => return self.eval_if(list, Arc::clone(&env)),
                        "do" => return self.eval_do(list, Arc::clone(&env)),
                        "let" | "let*" => return self.eval_let(list, Arc::clone(&env)),
                        "fn" | "fn*" => return self.eval_fn(list, Arc::clone(&env)),
                        "macro" => return self.eval_macro(list, Arc::clone(&env)),
                        "def" => return self.eval_def(list, Arc::clone(&env)),
                        // defn and defmacro are now macros defined in clojure.core using def+fn and def+macro
                        "loop" => return self.eval_loop(list, Arc::clone(&env)),
                        "recur" => return self.eval_recur(list, Arc::clone(&env)),
                        "ns" => return self.eval_ns(list, Arc::clone(&env)),
                        "require" => return self.eval_require(list, Arc::clone(&env)),
                        "in-ns" => return self.eval_in_ns(list, Arc::clone(&env)),
                        _ => {}
                    }
                }

                // Evaluate head to get function/macro
                let func_val = self.eval_in_env(&head, Arc::clone(&env))?;

                // Check if it's a macro - macros receive unevaluated arguments
                if let Value::Function(ref f) = func_val {
                    if f.is_macro() {
                        // Get unevaluated arguments
                        let raw_args: Vec<Value> = list.iter().skip(1).cloned().collect();
                        // Expand the macro
                        let expanded = self.expand_macro(f, &raw_args)?;
                        // Evaluate the expanded form
                        return self.eval_in_env(&expanded, env);
                    }
                }

                // Regular function call - evaluate arguments
                let args: Vec<Value> = list.iter()
                    .skip(1)
                    .map(|arg| self.eval_in_env(&arg, Arc::clone(&env)))
                    .collect::<JankResult<Vec<_>>>()?;

                self.apply(&func_val, &args)
            }

            // Atoms are special
            Value::Atom(_) => Ok(form.clone()),

            // Other self-evaluating forms
            Value::SpecialForm(_) | Value::Ratio(_, _) | Value::BigInt(_) => Ok(form.clone()),

            // Recur should only appear during loop evaluation - if it escapes, it's an error
            Value::Recur(_) => Err(JankError::eval("recur outside of loop/fn")),
        }
    }

    /// Apply a function to arguments
    pub fn apply(&mut self, func: &Value, args: &[Value]) -> JankResult<Value> {
        match func {
            Value::Function(f) => self.apply_function(f, args),

            // Keywords can act as functions: (:key map) -> (get map :key)
            Value::Keyword(k) => {
                if args.len() != 1 && args.len() != 2 {
                    return Err(JankError::arity(
                        k.to_string(),
                        "1 or 2",
                        args.len(),
                    ));
                }
                match &args[0] {
                    Value::Map(m) => {
                        Ok(m.get(&Value::Keyword(k.clone()))
                            .cloned()
                            .or_else(|| args.get(1).cloned())
                            .unwrap_or(Value::Nil))
                    }
                    _ => Ok(args.get(1).cloned().unwrap_or(Value::Nil)),
                }
            }

            // Maps can act as functions: (map key) -> (get map key)
            Value::Map(m) => {
                if args.len() != 1 && args.len() != 2 {
                    return Err(JankError::arity(
                        "map-as-function",
                        "1 or 2",
                        args.len(),
                    ));
                }
                Ok(m.get(&args[0])
                    .cloned()
                    .or_else(|| args.get(1).cloned())
                    .unwrap_or(Value::Nil))
            }

            // Vectors can act as functions: (vec index) -> (nth vec index)
            Value::Vector(v) => {
                if args.len() != 1 {
                    return Err(JankError::arity(
                        "vector-as-function",
                        "1",
                        args.len(),
                    ));
                }
                match &args[0] {
                    Value::Integer(i) => {
                        let idx = *i as usize;
                        v.get(idx)
                            .cloned()
                            .ok_or_else(|| JankError::IndexOutOfBounds {
                                index: *i,
                                size: v.len(),
                            })
                    }
                    _ => Err(JankError::type_error("integer", args[0].type_name())),
                }
            }

            // Sets can act as functions: (set val) -> val if in set, nil otherwise
            Value::Set(s) => {
                if args.len() != 1 {
                    return Err(JankError::arity(
                        "set-as-function",
                        "1",
                        args.len(),
                    ));
                }
                if s.contains(&args[0]) {
                    Ok(args[0].clone())
                } else {
                    Ok(Value::Nil)
                }
            }

            _ => Err(JankError::type_error("function", func.type_name())),
        }
    }

    /// Expand a macro with unevaluated arguments
    fn expand_macro(&mut self, mac: &Function, args: &[Value]) -> JankResult<Value> {
        match mac {
            Function::Macro { name, params, body, env, is_variadic, variadic_param, defining_ns, .. } => {
                self.expand_macro_arity(name.name(), params, variadic_param.as_ref(), body, env, defining_ns.as_deref(), args)
            }
            Function::MacroMulti { name, arities, env, defining_ns, .. } => {
                // Find matching arity
                for (params, variadic_param, body) in arities {
                    let is_variadic = variadic_param.is_some();
                    let fixed_count = params.len();

                    let matches = if is_variadic {
                        args.len() >= fixed_count
                    } else {
                        args.len() == fixed_count
                    };

                    if matches {
                        return self.expand_macro_arity(
                            name.name(),
                            params,
                            variadic_param.as_ref(),
                            body,
                            env,
                            defining_ns.as_deref(),
                            args
                        );
                    }
                }

                // No matching arity found
                let arity_strs: Vec<String> = arities.iter()
                    .map(|(p, v, _)| if v.is_some() { format!("{}+", p.len()) } else { p.len().to_string() })
                    .collect();
                Err(JankError::arity(name.name(), &arity_strs.join(" or "), args.len()))
            }
            _ => Err(JankError::eval("expand_macro called on non-macro")),
        }
    }

    /// Expand a single arity of a macro
    fn expand_macro_arity(
        &mut self,
        name: &str,
        params: &[Symbol],
        variadic_param: Option<&Symbol>,
        body: &Value,
        env: &Arc<Environment>,
        defining_ns: Option<&str>,
        args: &[Value],
    ) -> JankResult<Value> {
        let local_env = Arc::new(Environment::with_parent(Arc::clone(env)));

        // Bind parameters to unevaluated arguments
        if variadic_param.is_some() {
            let fixed_count = params.len();
            if args.len() < fixed_count {
                return Err(JankError::arity(name, &format!("at least {}", fixed_count), args.len()));
            }
            for (param, arg) in params.iter().zip(args.iter()) {
                local_env.define(param.name(), arg.clone());
            }
            if let Some(rest_param) = variadic_param {
                let rest: Vec<Value> = args.iter().skip(fixed_count).cloned().collect();
                local_env.define(rest_param.name(), Value::list(rest));
            }
        } else {
            if args.len() != params.len() {
                return Err(JankError::arity(name, &params.len().to_string(), args.len()));
            }
            for (param, arg) in params.iter().zip(args.iter()) {
                local_env.define(param.name(), arg.clone());
            }
        }

        // Switch to defining namespace for alias resolution
        let prev_ns = self.namespaces.current_name().to_string();
        if let Some(def_ns) = defining_ns {
            self.namespaces.switch_to(def_ns);
        }

        // Evaluate the macro body to get the expanded form
        let result = self.eval_in_env(body, local_env);

        // Restore original namespace
        self.namespaces.switch_to(&prev_ns);

        result
    }

    /// Apply a Function to arguments
    fn apply_function(&mut self, func: &Function, args: &[Value]) -> JankResult<Value> {
        match func {
            Function::Native { name, func: f, arity, .. } => {
                if !arity.accepts(args.len()) {
                    return Err(JankError::arity(
                        name,
                        format!("{:?}", arity),
                        args.len(),
                    ));
                }
                f(args)
            }

            Function::Interpreted { name, params, body, env, is_variadic, variadic_param, defining_ns, .. } |
            Function::Closure { name, params, body, captured_env: env, is_variadic, variadic_param, defining_ns, .. } => {
                // Check arity
                if *is_variadic {
                    if args.len() < params.len() {
                        return Err(JankError::arity(
                            func.name().unwrap_or("anonymous"),
                            format!("{} or more", params.len()),
                            args.len(),
                        ));
                    }
                } else if args.len() != params.len() {
                    return Err(JankError::arity(
                        func.name().unwrap_or("anonymous"),
                        params.len().to_string(),
                        args.len(),
                    ));
                }

                // For named, non-variadic functions, try JIT
                if let Some(fn_name) = name {
                    let fn_name_str = fn_name.name();

                    // Check if already compiled
                    if let Some(result) = self.call_compiled(fn_name_str, args) {
                        return Ok(result);
                    }

                    // Check if should compile now
                    if !*is_variadic && self.should_compile(fn_name_str) {
                        // Try to JIT compile
                        let _ = self.try_jit_compile(fn_name_str, params, body);
                        // Try to use compiled version
                        if let Some(result) = self.call_compiled(fn_name_str, args) {
                            return Ok(result);
                        }
                    }
                }

                // Fall back to interpretation
                let local_env = Arc::new(Environment::with_fn_args(
                    Arc::clone(env),
                    params,
                    args,
                    variadic_param.as_ref(),
                )?);

                // Switch to defining namespace for alias resolution, then restore
                let prev_ns = self.namespaces.current_name().to_string();
                if let Some(def_ns) = defining_ns {
                    self.namespaces.switch_to(def_ns);
                }

                // Evaluate body
                let result = self.eval_in_env(body, local_env);

                // Restore original namespace
                self.namespaces.switch_to(&prev_ns);

                result
            }

            Function::InterpretedMulti { name, arities, env, defining_ns, .. } => {
                // Find the matching arity
                let arg_count = args.len();
                let mut matched_arity = None;
                let mut variadic_match = None;

                for (params, variadic_param, body) in arities {
                    if variadic_param.is_some() {
                        // Variadic arity - matches if arg_count >= params.len()
                        if arg_count >= params.len() {
                            variadic_match = Some((params, variadic_param, body));
                        }
                    } else if arg_count == params.len() {
                        matched_arity = Some((params, variadic_param, body));
                        break;
                    }
                }

                let (params, variadic_param, body) = matched_arity
                    .or(variadic_match)
                    .ok_or_else(|| JankError::arity(
                        name.as_ref().map(|s| s.name()).unwrap_or("anonymous"),
                        format!("multi-arity"),
                        arg_count,
                    ))?;

                // Create local environment with args
                let local_env = Arc::new(Environment::with_fn_args(
                    Arc::clone(env),
                    params,
                    args,
                    variadic_param.as_ref(),
                )?);

                // Switch to defining namespace for alias resolution
                let prev_ns = self.namespaces.current_name().to_string();
                if let Some(def_ns) = defining_ns {
                    self.namespaces.switch_to(def_ns);
                }

                let result = self.eval_in_env(body, local_env);

                self.namespaces.switch_to(&prev_ns);
                result
            }

            Function::Macro { .. } | Function::MacroMulti { .. } => {
                Err(JankError::eval("cannot call macro as function - must be expanded"))
            }

            Function::Partial { func, applied_args } => {
                let mut all_args = applied_args.clone();
                all_args.extend_from_slice(args);
                self.apply_function(func, &all_args)
            }

            Function::Composed { functions } => {
                if args.len() != 1 {
                    return Err(JankError::arity("composed-fn", "1", args.len()));
                }
                let mut result = args[0].clone();
                for f in functions.iter().rev() {
                    result = self.apply_function(f, &[result])?;
                }
                Ok(result)
            }
        }
    }

    // Special form evaluators

    fn eval_quote(&mut self, list: &List, _env: Arc<Environment>) -> JankResult<Value> {
        let args: Vec<Value> = list.iter().skip(1).cloned().collect();
        if args.len() != 1 {
            return Err(JankError::arity("quote", "1", args.len()));
        }
        Ok(args[0].clone())
    }

    fn eval_syntax_quote(&mut self, list: &List, env: Arc<Environment>) -> JankResult<Value> {
        let args: Vec<Value> = list.iter().skip(1).cloned().collect();
        if args.len() != 1 {
            return Err(JankError::arity("syntax-quote", "1", args.len()));
        }
        self.syntax_quote_expand(&args[0], env)
    }

    /// Recursively expand a syntax-quoted form
    fn syntax_quote_expand(&mut self, form: &Value, env: Arc<Environment>) -> JankResult<Value> {
        match form {
            // Lists need special handling for unquote and unquote-splicing
            Value::List(list) => {
                if list.is_empty() {
                    return Ok(Value::list(vec![]));
                }

                // Check for (unquote x)
                if let Some(Value::Symbol(sym)) = list.head() {
                    if sym.name() == "unquote" {
                        let args: Vec<Value> = list.iter().skip(1).cloned().collect();
                        if args.len() != 1 {
                            return Err(JankError::arity("unquote", "1", args.len()));
                        }
                        // Evaluate the unquoted expression
                        return self.eval_in_env(&args[0], env);
                    }
                }

                // Process list elements, handling unquote-splicing
                let mut result = Vec::new();
                for item in list.iter() {
                    if let Value::List(inner_list) = &item {
                        if let Some(Value::Symbol(sym)) = inner_list.head() {
                            if sym.name() == "unquote-splicing" {
                                let splice_args: Vec<Value> = inner_list.iter().skip(1).cloned().collect();
                                if splice_args.len() != 1 {
                                    return Err(JankError::arity("unquote-splicing", "1", splice_args.len()));
                                }
                                // Evaluate and splice
                                let spliced = self.eval_in_env(&splice_args[0], Arc::clone(&env))?;
                                // Convert to sequence and add elements
                                match spliced {
                                    Value::List(l) => {
                                        for v in l.iter() {
                                            result.push(v.clone());
                                        }
                                    }
                                    Value::Vector(v) => {
                                        for item in v.iter() {
                                            result.push(item.clone());
                                        }
                                    }
                                    Value::Nil => {} // Empty splice
                                    other => result.push(other),
                                }
                                continue;
                            }
                        }
                    }
                    // Regular element - recursively expand
                    result.push(self.syntax_quote_expand(&item, Arc::clone(&env))?);
                }
                Ok(Value::list(result))
            }

            // Vectors: recursively expand elements
            Value::Vector(v) => {
                let mut result = imbl::Vector::new();
                for item in v.iter() {
                    if let Value::List(inner_list) = item {
                        if let Some(Value::Symbol(sym)) = inner_list.head() {
                            if sym.name() == "unquote-splicing" {
                                let splice_args: Vec<Value> = inner_list.iter().skip(1).cloned().collect();
                                if splice_args.len() != 1 {
                                    return Err(JankError::arity("unquote-splicing", "1", splice_args.len()));
                                }
                                let spliced = self.eval_in_env(&splice_args[0], Arc::clone(&env))?;
                                match spliced {
                                    Value::List(l) => {
                                        for v in l.iter() {
                                            result.push_back(v.clone());
                                        }
                                    }
                                    Value::Vector(vec) => {
                                        for item in vec.iter() {
                                            result.push_back(item.clone());
                                        }
                                    }
                                    Value::Nil => {}
                                    other => result.push_back(other),
                                }
                                continue;
                            }
                        }
                    }
                    result.push_back(self.syntax_quote_expand(item, Arc::clone(&env))?);
                }
                Ok(Value::Vector(Arc::new(result)))
            }

            // Maps: recursively expand keys and values
            Value::Map(m) => {
                let mut result = imbl::HashMap::new();
                for (k, v) in m.iter() {
                    let key = self.syntax_quote_expand(k, Arc::clone(&env))?;
                    let val = self.syntax_quote_expand(v, Arc::clone(&env))?;
                    result.insert(key, val);
                }
                Ok(Value::Map(Arc::new(result)))
            }

            // Sets: recursively expand elements
            Value::Set(s) => {
                let mut result = imbl::HashSet::new();
                for item in s.iter() {
                    result.insert(self.syntax_quote_expand(item, Arc::clone(&env))?);
                }
                Ok(Value::Set(Arc::new(result)))
            }

            // Symbols in syntax-quote should be fully qualified (optional enhancement for later)
            // For now, just return as-is like regular quote
            _ => Ok(form.clone()),
        }
    }

    fn eval_if(&mut self, list: &List, env: Arc<Environment>) -> JankResult<Value> {
        let args: Vec<Value> = list.iter().skip(1).cloned().collect();
        if args.len() < 2 || args.len() > 3 {
            return Err(JankError::arity("if", "2 or 3", args.len()));
        }

        let condition = self.eval_in_env(&args[0], Arc::clone(&env))?;

        if condition.is_truthy() {
            self.eval_in_env(&args[1], env)
        } else if args.len() == 3 {
            self.eval_in_env(&args[2], env)
        } else {
            Ok(Value::Nil)
        }
    }

    fn eval_do(&mut self, list: &List, env: Arc<Environment>) -> JankResult<Value> {
        let mut result = Value::Nil;
        for form in list.iter().skip(1) {
            result = self.eval_in_env(&form, Arc::clone(&env))?;
        }
        Ok(result)
    }

    fn eval_let(&mut self, list: &List, env: Arc<Environment>) -> JankResult<Value> {
        let args: Vec<Value> = list.iter().skip(1).cloned().collect();
        if args.is_empty() {
            return Err(JankError::parse("let requires a binding vector", 0, 0));
        }

        // Get bindings vector
        let bindings = match &args[0] {
            Value::Vector(v) => v,
            _ => return Err(JankError::type_error("vector", args[0].type_name())),
        };

        if bindings.len() % 2 != 0 {
            return Err(JankError::parse("let bindings must be even", 0, 0));
        }

        // Create new environment
        let let_env = Arc::new(Environment::with_parent(env));

        // Process bindings sequentially
        let bindings_vec: Vec<_> = bindings.iter().cloned().collect();
        for chunk in bindings_vec.chunks(2) {
            let name = match &chunk[0] {
                Value::Symbol(s) => s.name().to_string(),
                _ => return Err(JankError::type_error("symbol", chunk[0].type_name())),
            };
            let value = self.eval_in_env(&chunk[1], Arc::clone(&let_env))?;
            let_env.define(&name, value);
        }

        // Evaluate body forms
        let mut result = Value::Nil;
        for form in args.iter().skip(1) {
            result = self.eval_in_env(form, Arc::clone(&let_env))?;
        }

        Ok(result)
    }

    fn eval_fn(&mut self, list: &List, env: Arc<Environment>) -> JankResult<Value> {
        let args: Vec<Value> = list.iter().skip(1).cloned().collect();
        if args.is_empty() {
            return Err(JankError::parse("fn requires parameters", 0, 0));
        }

        // Check for optional name
        let (name, params_idx) = match &args[0] {
            Value::Symbol(s) => (Some(s.clone()), 1),
            Value::Vector(_) | Value::List(_) => (None, 0),
            _ => return Err(JankError::type_error("symbol, vector, or list", args[0].type_name())),
        };

        if params_idx >= args.len() {
            return Err(JankError::parse("fn requires parameters vector", 0, 0));
        }

        // Check if this is multi-arity or single-arity
        // Multi-arity: (fn name ([p1] b1) ([p2] b2) ...)
        // Single-arity: (fn name [params] body)
        let is_multi_arity = matches!(&args[params_idx], Value::List(_));

        if is_multi_arity {
            // Multi-arity function
            let mut arities: Vec<(Vec<Symbol>, Option<Symbol>, Arc<Value>)> = Vec::new();

            for arity_form in args.iter().skip(params_idx) {
                let arity_list = match arity_form {
                    Value::List(l) => l,
                    _ => return Err(JankError::parse("multi-arity clause must be a list", 0, 0)),
                };

                if arity_list.is_empty() {
                    return Err(JankError::parse("arity clause cannot be empty", 0, 0));
                }

                let arity_vec: Vec<Value> = arity_list.iter().cloned().collect();

                // First element is parameters vector
                let params_vec = match &arity_vec[0] {
                    Value::Vector(v) => v,
                    _ => return Err(JankError::type_error("vector", arity_vec[0].type_name())),
                };

                // Parse parameters
                let mut params = Vec::new();
                let mut variadic_param = None;
                let mut iter = params_vec.iter().peekable();
                while let Some(param) = iter.next() {
                    match param {
                        Value::Symbol(s) if s.name() == "&" => {
                            if let Some(Value::Symbol(rest)) = iter.next() {
                                variadic_param = Some(rest.clone());
                            } else {
                                return Err(JankError::parse("& must be followed by symbol", 0, 0));
                            }
                            break;
                        }
                        Value::Symbol(s) => params.push(s.clone()),
                        _ => return Err(JankError::type_error("symbol", param.type_name())),
                    }
                }

                // Rest is the body
                let body = if arity_vec.len() == 2 {
                    arity_vec[1].clone()
                } else {
                    let mut do_list = vec![Value::Symbol(Symbol::new("do"))];
                    do_list.extend(arity_vec.iter().skip(1).cloned());
                    Value::list(do_list)
                };

                arities.push((params, variadic_param, Arc::new(body)));
            }

            Ok(Value::Function(Arc::new(Function::InterpretedMulti {
                name,
                arities,
                env,
                doc: None,
                defining_ns: Some(self.namespaces.current_name().to_string()),
            })))
        } else {
            // Single-arity function
            let params_vec = match &args[params_idx] {
                Value::Vector(v) => v,
                _ => return Err(JankError::type_error("vector", args[params_idx].type_name())),
            };

            // Parse parameters, looking for & for variadic
            let mut params = Vec::new();
            let mut is_variadic = false;
            let mut variadic_param = None;

            let mut iter = params_vec.iter().peekable();
            while let Some(param) = iter.next() {
                match param {
                    Value::Symbol(s) if s.name() == "&" => {
                        is_variadic = true;
                        if let Some(Value::Symbol(rest)) = iter.next() {
                            variadic_param = Some(rest.clone());
                        } else {
                            return Err(JankError::parse("& must be followed by symbol", 0, 0));
                        }
                        break;
                    }
                    Value::Symbol(s) => params.push(s.clone()),
                    _ => return Err(JankError::type_error("symbol", param.type_name())),
                }
            }

            // Create body (do block if multiple expressions)
            let body = if args.len() - params_idx - 1 == 1 {
                args[params_idx + 1].clone()
            } else {
                let mut do_list = vec![Value::Symbol(Symbol::new("do"))];
                do_list.extend(args.iter().skip(params_idx + 1).cloned());
                Value::list(do_list)
            };

            Ok(Value::Function(Arc::new(Function::Closure {
                name,
                params,
                body: Arc::new(body),
                captured_env: env,
                is_variadic,
                variadic_param,
                defining_ns: Some(self.namespaces.current_name().to_string()),
            })))
        }
    }

    /// Evaluate (macro name [params] body...) - creates a macro
    /// Similar to fn but creates a Function::Macro
    fn eval_macro(&mut self, list: &List, env: Arc<Environment>) -> JankResult<Value> {
        let args: Vec<Value> = list.iter().skip(1).cloned().collect();
        if args.is_empty() {
            return Err(JankError::parse("macro requires name and parameters", 0, 0));
        }

        // Macro requires a name (for error messages and debugging)
        let (name, params_idx) = match &args[0] {
            Value::Symbol(s) => (s.clone(), 1),
            Value::Vector(_) => {
                // Anonymous macro - create a gensym name
                (Symbol::new(&format!("macro__{}", self.gensym_counter())), 0)
            }
            _ => return Err(JankError::type_error("symbol or vector", args[0].type_name())),
        };

        if params_idx >= args.len() && params_idx > 0 {
            return Err(JankError::parse("macro requires parameters vector", 0, 0));
        }

        // Check if this is multi-arity or single-arity
        let actual_params_idx = if params_idx == 0 { 0 } else { params_idx };
        let is_multi_arity = matches!(&args[actual_params_idx], Value::List(_));

        if is_multi_arity {
            // Multi-arity macro
            let mut arities: Vec<(Vec<Symbol>, Option<Symbol>, Arc<Value>)> = Vec::new();

            for arity_form in args.iter().skip(actual_params_idx) {
                let arity_list = match arity_form {
                    Value::List(l) => l,
                    _ => return Err(JankError::parse("multi-arity clause must be a list", 0, 0)),
                };

                let arity_items: Vec<Value> = arity_list.iter().cloned().collect();
                if arity_items.is_empty() {
                    return Err(JankError::parse("arity clause cannot be empty", 0, 0));
                }

                let params_vec = match &arity_items[0] {
                    Value::Vector(v) => v,
                    _ => return Err(JankError::type_error("vector", arity_items[0].type_name())),
                };

                let (params, variadic_param) = self.parse_params(params_vec)?;

                let body = if arity_items.len() == 2 {
                    arity_items[1].clone()
                } else {
                    let mut do_list = vec![Value::Symbol(Symbol::new("do"))];
                    do_list.extend(arity_items.iter().skip(1).cloned());
                    Value::list(do_list)
                };

                arities.push((params, variadic_param, Arc::new(body)));
            }

            Ok(Value::Function(Arc::new(Function::MacroMulti {
                name,
                arities,
                env,
                doc: None,
                defining_ns: Some(self.namespaces.current_name().to_string()),
            })))
        } else {
            // Single-arity macro
            let params_vec = match &args[actual_params_idx] {
                Value::Vector(v) => v,
                _ => return Err(JankError::type_error("vector", args[actual_params_idx].type_name())),
            };

            let (params, variadic_param) = self.parse_params(params_vec)?;
            let is_variadic = variadic_param.is_some();

            let body = if args.len() - actual_params_idx - 1 == 1 {
                args[actual_params_idx + 1].clone()
            } else {
                let mut do_list = vec![Value::Symbol(Symbol::new("do"))];
                do_list.extend(args.iter().skip(actual_params_idx + 1).cloned());
                Value::list(do_list)
            };

            Ok(Value::Function(Arc::new(Function::Macro {
                name,
                params,
                body: Arc::new(body),
                env,
                is_variadic,
                variadic_param,
                doc: None,
                defining_ns: Some(self.namespaces.current_name().to_string()),
            })))
        }
    }

    fn eval_def(&mut self, list: &List, env: Arc<Environment>) -> JankResult<Value> {
        let args: Vec<Value> = list.iter().skip(1).cloned().collect();
        if args.is_empty() || args.len() > 2 {
            return Err(JankError::arity("def", "1 or 2", args.len()));
        }

        // Handle metadata: (def (with-meta sym meta) value) -> extract sym
        // The ^{:doc "..."} reader macro expands to (with-meta sym {:doc "..."})
        let (name, _metadata) = match &args[0] {
            Value::Symbol(s) => (s.name().to_string(), None),
            Value::List(l) => {
                // Check if it's (with-meta sym meta)
                let items: Vec<Value> = l.iter().cloned().collect();
                if items.len() == 3 {
                    if let Value::Symbol(s) = &items[0] {
                        if s.name() == "with-meta" {
                            if let Value::Symbol(name_sym) = &items[1] {
                                // Extract the symbol name and metadata
                                (name_sym.name().to_string(), Some(items[2].clone()))
                            } else {
                                return Err(JankError::type_error("symbol", items[1].type_name()));
                            }
                        } else {
                            return Err(JankError::type_error("symbol", args[0].type_name()));
                        }
                    } else {
                        return Err(JankError::type_error("symbol", args[0].type_name()));
                    }
                } else {
                    return Err(JankError::type_error("symbol", args[0].type_name()));
                }
            }
            _ => return Err(JankError::type_error("symbol", args[0].type_name())),
        };

        let value = if args.len() == 2 {
            self.eval_in_env(&args[1], Arc::clone(&env))?
        } else {
            Value::Nil
        };

        // Define in the current namespace for qualified and unqualified access
        // We intentionally DON'T define in global_env to maintain namespace isolation
        self.namespaces.define(&name, value.clone());

        // EAGER JIT: If defining a function, try to compile it immediately!
        // This makes the first call fast (no compilation overhead)
        if let Value::Function(ref f) = value {
            match f.as_ref() {
                Function::Closure { params, body, is_variadic: false, .. } |
                Function::Interpreted { params, body, is_variadic: false, .. } => {
                    let _ = self.try_jit_compile(&name, params, body);
                }
                Function::InterpretedMulti { arities, .. } => {
                    // Try to compile the first non-variadic arity
                    for (params, variadic_param, body) in arities {
                        if variadic_param.is_none() {
                            let _ = self.try_jit_compile(&name, params, body);
                            break;
                        }
                    }
                }
                _ => {}
            }
        }

        Ok(Value::Symbol(Symbol::new(&name)))
    }

    fn eval_defn(&mut self, list: &List, env: Arc<Environment>) -> JankResult<Value> {
        let args: Vec<Value> = list.iter().skip(1).cloned().collect();
        if args.len() < 2 {
            return Err(JankError::parse("defn requires name and params", 0, 0));
        }

        let name = match &args[0] {
            Value::Symbol(s) => s.clone(),
            _ => return Err(JankError::type_error("symbol", args[0].type_name())),
        };

        // Check for docstring
        let (doc, body_start_idx) = match &args[1] {
            Value::String(s) if args.len() > 2 => (Some(s.as_ref().clone()), 2),
            _ => (None, 1),
        };

        // Check if this is multi-arity or single-arity
        // Multi-arity: (defn name ([params1] body1) ([params2] body2) ...)
        // Single-arity: (defn name [params] body ...)
        let is_multi_arity = matches!(&args[body_start_idx], Value::List(_));

        if is_multi_arity {
            // Parse each arity clause
            let mut arities: Vec<(Vec<Symbol>, Option<Symbol>, Arc<Value>)> = Vec::new();

            for arity_form in args.iter().skip(body_start_idx) {
                let arity_list = match arity_form {
                    Value::List(l) => l,
                    _ => return Err(JankError::parse("multi-arity clause must be a list", 0, 0)),
                };

                let arity_items: Vec<Value> = arity_list.iter().cloned().collect();
                if arity_items.is_empty() {
                    return Err(JankError::parse("arity clause cannot be empty", 0, 0));
                }

                // First element is params vector
                let params_vec = match &arity_items[0] {
                    Value::Vector(v) => v,
                    _ => return Err(JankError::type_error("vector", arity_items[0].type_name())),
                };

                // Parse parameters
                let (params, variadic_param) = self.parse_params(params_vec)?;

                // Rest is body
                let body = if arity_items.len() == 2 {
                    arity_items[1].clone()
                } else {
                    let mut do_list = vec![Value::Symbol(Symbol::new("do"))];
                    do_list.extend(arity_items.iter().skip(1).cloned());
                    Value::list(do_list)
                };

                arities.push((params, variadic_param, Arc::new(body)));
            }

            let func = Function::InterpretedMulti {
                name: Some(name.clone()),
                arities,
                env,
                doc,
                defining_ns: Some(self.namespaces.current_name().to_string()),
            };

            let func_val = Value::Function(Arc::new(func));
            self.namespaces.define(name.name(), func_val);
        } else {
            // Single-arity: (defn name [params] body ...)
            let params_vec = match &args[body_start_idx] {
                Value::Vector(v) => v,
                _ => return Err(JankError::type_error("vector", args[body_start_idx].type_name())),
            };

            let (params, variadic_param) = self.parse_params(params_vec)?;
            let is_variadic = variadic_param.is_some();

            // Create body
            let body = if args.len() - body_start_idx - 1 == 1 {
                args[body_start_idx + 1].clone()
            } else {
                let mut do_list = vec![Value::Symbol(Symbol::new("do"))];
                do_list.extend(args.iter().skip(body_start_idx + 1).cloned());
                Value::list(do_list)
            };

            let func = Function::Interpreted {
                name: Some(name.clone()),
                params: params.clone(),
                body: Arc::new(body.clone()),
                env,
                is_variadic,
                variadic_param,
                doc,
                defining_ns: Some(self.namespaces.current_name().to_string()),
            };

            // Define in the current namespace for qualified access
            let func_val = Value::Function(Arc::new(func));
            self.namespaces.define(name.name(), func_val);

            // EAGER JIT: Compile immediately if eligible!
            // This makes the first call fast (no compilation overhead)
            if !is_variadic {
                let _ = self.try_jit_compile(name.name(), &params, &body);
            }
        }

        Ok(Value::Symbol(name))
    }

    fn eval_defmacro(&mut self, list: &List, env: Arc<Environment>) -> JankResult<Value> {
        let args: Vec<Value> = list.iter().skip(1).cloned().collect();
        if args.len() < 2 {
            return Err(JankError::parse("defmacro requires name and params", 0, 0));
        }

        let name = match &args[0] {
            Value::Symbol(s) => s.clone(),
            _ => return Err(JankError::type_error("symbol", args[0].type_name())),
        };

        // Check for docstring
        let (doc, body_start_idx) = match &args[1] {
            Value::String(s) if args.len() > 2 => (Some(s.as_ref().clone()), 2),
            _ => (None, 1),
        };

        // Check if this is multi-arity or single-arity
        // Multi-arity: (defmacro name ([params1] body1) ([params2] body2) ...)
        // Single-arity: (defmacro name [params] body ...)
        let is_multi_arity = matches!(&args[body_start_idx], Value::List(_));

        if is_multi_arity {
            // Parse each arity clause
            let mut arities: Vec<(Vec<Symbol>, Option<Symbol>, Arc<Value>)> = Vec::new();

            for arity_form in args.iter().skip(body_start_idx) {
                let arity_list = match arity_form {
                    Value::List(l) => l,
                    _ => return Err(JankError::parse("multi-arity clause must be a list", 0, 0)),
                };

                let arity_items: Vec<Value> = arity_list.iter().cloned().collect();
                if arity_items.is_empty() {
                    return Err(JankError::parse("arity clause cannot be empty", 0, 0));
                }

                // First element is params vector
                let params_vec = match &arity_items[0] {
                    Value::Vector(v) => v,
                    _ => return Err(JankError::type_error("vector", arity_items[0].type_name())),
                };

                // Parse parameters
                let (params, variadic_param) = self.parse_params(params_vec)?;

                // Rest is body
                let body = if arity_items.len() == 2 {
                    arity_items[1].clone()
                } else {
                    let mut do_list = vec![Value::Symbol(Symbol::new("do"))];
                    do_list.extend(arity_items.iter().skip(1).cloned());
                    Value::list(do_list)
                };

                arities.push((params, variadic_param, Arc::new(body)));
            }

            let mac = Function::MacroMulti {
                name: name.clone(),
                arities,
                env,
                doc,
                defining_ns: Some(self.namespaces.current_name().to_string()),
            };

            let macro_val = Value::Function(Arc::new(mac));
            self.namespaces.define(name.name(), macro_val);
        } else {
            // Single-arity: (defmacro name [params] body ...)
            let params_vec = match &args[body_start_idx] {
                Value::Vector(v) => v,
                _ => return Err(JankError::type_error("vector", args[body_start_idx].type_name())),
            };

            let (params, variadic_param) = self.parse_params(params_vec)?;
            let is_variadic = variadic_param.is_some();

            // Create body
            let body = if args.len() - body_start_idx - 1 == 1 {
                args[body_start_idx + 1].clone()
            } else {
                let mut do_list = vec![Value::Symbol(Symbol::new("do"))];
                do_list.extend(args.iter().skip(body_start_idx + 1).cloned());
                Value::list(do_list)
            };

            let mac = Function::Macro {
                name: name.clone(),
                params,
                body: Arc::new(body),
                env,
                is_variadic,
                variadic_param,
                doc,
                defining_ns: Some(self.namespaces.current_name().to_string()),
            };

            let macro_val = Value::Function(Arc::new(mac));
            self.namespaces.define(name.name(), macro_val);
        }

        Ok(Value::Symbol(name))
    }

    /// Parse a parameter vector into (params, variadic_param)
    fn parse_params(&self, params_vec: &imbl::Vector<Value>) -> JankResult<(Vec<Symbol>, Option<Symbol>)> {
        let mut params = Vec::new();
        let mut variadic_param = None;

        let mut iter = params_vec.iter().peekable();
        while let Some(param) = iter.next() {
            match param {
                Value::Symbol(s) if s.name() == "&" => {
                    if let Some(Value::Symbol(rest)) = iter.next() {
                        variadic_param = Some(rest.clone());
                    } else {
                        return Err(JankError::parse("& must be followed by symbol", 0, 0));
                    }
                    break;
                }
                Value::Symbol(s) => params.push(s.clone()),
                _ => return Err(JankError::type_error("symbol", param.type_name())),
            }
        }

        Ok((params, variadic_param))
    }

    fn eval_recur(&mut self, list: &List, env: Arc<Environment>) -> JankResult<Value> {
        // Evaluate all the recur arguments
        let args: Vec<Value> = list.iter()
            .skip(1)
            .map(|arg| self.eval_in_env(&arg, Arc::clone(&env)))
            .collect::<JankResult<Vec<_>>>()?;

        // Return a Recur value that will be caught by eval_loop
        Ok(Value::Recur(args))
    }

    fn eval_loop(&mut self, list: &List, env: Arc<Environment>) -> JankResult<Value> {
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

        // Extract binding names and initial values
        let bindings_vec: Vec<_> = bindings.iter().cloned().collect();
        let mut binding_names = Vec::new();
        let mut values: Vec<Value> = Vec::new();

        for chunk in bindings_vec.chunks(2) {
            let name = match &chunk[0] {
                Value::Symbol(s) => s.name().to_string(),
                _ => return Err(JankError::type_error("symbol", chunk[0].type_name())),
            };
            binding_names.push(name);
            values.push(self.eval_in_env(&chunk[1], Arc::clone(&env))?);
        }

        // Body forms
        let body_forms: Vec<_> = args.iter().skip(1).cloned().collect();

        // Loop execution
        loop {
            // Create environment with current bindings
            let loop_env = Arc::new(Environment::with_parent(Arc::clone(&env)));
            for (name, value) in binding_names.iter().zip(values.iter()) {
                loop_env.define(name, value.clone());
            }

            // Evaluate body forms
            let mut result = Value::Nil;
            let mut did_recur = false;
            for form in &body_forms {
                result = self.eval_in_env(form, Arc::clone(&loop_env))?;

                // Check if we got a Recur signal
                if let Value::Recur(ref new_values) = result {
                    if new_values.len() != binding_names.len() {
                        return Err(JankError::arity(
                            "recur",
                            binding_names.len().to_string(),
                            new_values.len(),
                        ));
                    }
                    values = new_values.clone();
                    did_recur = true;
                    break;
                }
            }

            // If the last result was not a Recur, we're done
            if !did_recur {
                return Ok(result);
            }
        }
    }

    /// Evaluate (ns name (:require ...)) form
    /// Sets up a namespace with optional requires
    fn eval_ns(&mut self, list: &List, _env: Arc<Environment>) -> JankResult<Value> {
        let args: Vec<Value> = list.iter().skip(1).cloned().collect();
        if args.is_empty() {
            return Err(JankError::parse("ns requires a namespace name", 0, 0));
        }

        // First arg must be a symbol (the namespace name)
        let ns_name = match &args[0] {
            Value::Symbol(sym) => sym.name().to_string(),
            _ => return Err(JankError::parse("ns name must be a symbol", 0, 0)),
        };

        // Switch to the namespace (creates it if needed)
        self.namespaces.switch_to(&ns_name);

        // Process remaining forms (typically :require clauses)
        for arg in args.iter().skip(1) {
            if let Value::List(clause) = arg {
                if let Some(Value::Keyword(kw)) = clause.head() {
                    match kw.name() {
                        "require" => {
                            // Process each require spec
                            for spec in clause.iter().skip(1) {
                                self.process_require_spec(&spec)?;
                            }
                        }
                        "use" => {
                            // For now, treat :use like :require with :refer :all
                            // This is a simplified version
                            return Err(JankError::eval(":use is not yet supported, use :require instead"));
                        }
                        _ => {
                            // Ignore unknown clauses
                        }
                    }
                }
            }
        }

        Ok(Value::Nil)
    }

    /// Process a single require spec like [other.ns :as alias] or [other.ns :refer [foo bar]]
    fn process_require_spec(&mut self, spec: &Value) -> JankResult<()> {
        match spec {
            // Simple symbol: (require other.ns)
            Value::Symbol(sym) => {
                self.load_namespace(sym.name())?;
            }
            // Vector spec: [other.ns :as alias] or [other.ns :refer [foo bar]]
            Value::Vector(v) => {
                if v.is_empty() {
                    return Err(JankError::parse("require spec cannot be empty", 0, 0));
                }

                // First element is the namespace name
                let ns_name = match &v[0] {
                    Value::Symbol(sym) => sym.name().to_string(),
                    _ => return Err(JankError::parse("require spec must start with a symbol", 0, 0)),
                };

                // Load the namespace
                self.load_namespace(&ns_name)?;

                // Process options (:as, :refer)
                let mut i = 1;
                while i < v.len() {
                    if let Value::Keyword(kw) = &v[i] {
                        match kw.name() {
                            "as" => {
                                if i + 1 >= v.len() {
                                    return Err(JankError::parse(":as requires an alias", 0, 0));
                                }
                                if let Value::Symbol(alias) = &v[i + 1] {
                                    self.namespaces.current_mut().add_alias(alias.name(), &ns_name);
                                    i += 2;
                                } else {
                                    return Err(JankError::parse(":as alias must be a symbol", 0, 0));
                                }
                            }
                            "refer" => {
                                if i + 1 >= v.len() {
                                    return Err(JankError::parse(":refer requires a vector of symbols", 0, 0));
                                }
                                if let Value::Vector(refers) = &v[i + 1] {
                                    for refer in refers.iter() {
                                        if let Value::Symbol(sym) = refer {
                                            self.namespaces.current_mut().add_refer(
                                                sym.name(),
                                                &ns_name,
                                                sym.name(),
                                            );
                                        }
                                    }
                                    i += 2;
                                } else {
                                    return Err(JankError::parse(":refer must be followed by a vector", 0, 0));
                                }
                            }
                            _ => {
                                i += 1;
                            }
                        }
                    } else {
                        i += 1;
                    }
                }
            }
            _ => return Err(JankError::parse("invalid require spec", 0, 0)),
        }
        Ok(())
    }

    /// Load a namespace from a .jrs file
    fn load_namespace(&mut self, ns_name: &str) -> JankResult<()> {
        // Check if already loaded
        if self.namespaces.is_loaded(ns_name) {
            return Ok(());
        }

        // Check for circular dependency
        if self.namespaces.is_loading(ns_name) {
            return Err(JankError::eval(format!("circular dependency detected: {}", ns_name)));
        }

        // Find the file
        let file_path = self.namespaces.find_ns_file(ns_name)
            .ok_or_else(|| JankError::eval(format!("namespace not found: {}", ns_name)))?;

        // Mark as loading
        self.namespaces.start_loading(ns_name);

        // Read and parse the file
        let source = std::fs::read_to_string(&file_path)
            .map_err(|e| JankError::eval(format!("failed to read {}: {}", file_path.display(), e)))?;

        // Save current namespace
        let prev_ns = self.namespaces.current_name().to_string();

        // Parse and evaluate each form
        let result = self.eval_source(&source);

        // Restore previous namespace
        self.namespaces.switch_to(&prev_ns);

        // Mark as done loading
        self.namespaces.finish_loading(ns_name);

        result?;
        Ok(())
    }

    /// Evaluate a source string containing multiple forms
    fn eval_source(&mut self, source: &str) -> JankResult<Value> {
        // For now, parse and eval forms one at a time
        // A more sophisticated reader would handle this better
        let mut result = Value::Nil;
        let mut remaining = source.trim();

        while !remaining.is_empty() {
            // Try to parse a form
            match read_string(remaining) {
                Ok(form) => {
                    result = self.eval(&form)?;
                    // Skip past the parsed form
                    // This is a simplified approach - a proper reader would track position
                    remaining = skip_form(remaining);
                }
                Err(e) => {
                    if remaining.trim().is_empty() {
                        break;
                    }
                    return Err(e);
                }
            }
        }

        Ok(result)
    }

    /// Evaluate (require ...) form - load namespaces
    fn eval_require(&mut self, list: &List, _env: Arc<Environment>) -> JankResult<Value> {
        for spec in list.iter().skip(1) {
            self.process_require_spec(&spec)?;
        }
        Ok(Value::Nil)
    }

    /// Evaluate (in-ns 'namespace) - switch to a namespace
    fn eval_in_ns(&mut self, list: &List, env: Arc<Environment>) -> JankResult<Value> {
        let args: Vec<Value> = list.iter().skip(1).cloned().collect();
        if args.len() != 1 {
            return Err(JankError::arity("in-ns", "1", args.len()));
        }

        // Evaluate the argument (should be a quoted symbol)
        let ns_arg = self.eval_in_env(&args[0], Arc::clone(&env))?;

        let ns_name = match &ns_arg {
            Value::Symbol(sym) => sym.name().to_string(),
            _ => return Err(JankError::eval("in-ns requires a symbol")),
        };

        self.namespaces.switch_to(&ns_name);
        Ok(Value::Nil)
    }
}

/// Skip past a form in source text (simplified)
fn skip_form(source: &str) -> &str {
    let s = source.trim_start();
    if s.is_empty() {
        return "";
    }

    let mut depth = 0;
    let mut in_string = false;
    let mut escape = false;
    let mut chars = s.char_indices().peekable();

    while let Some((i, c)) = chars.next() {
        if escape {
            escape = false;
            continue;
        }

        if c == '\\' && in_string {
            escape = true;
            continue;
        }

        if c == '"' {
            in_string = !in_string;
            if !in_string && depth == 0 {
                // End of string at top level
                return &s[i + 1..];
            }
            continue;
        }

        if in_string {
            continue;
        }

        match c {
            '(' | '[' | '{' => depth += 1,
            ')' | ']' | '}' => {
                depth -= 1;
                if depth == 0 {
                    return &s[i + 1..];
                }
            }
            _ if depth == 0 && c.is_whitespace() => {
                // End of atom
                return &s[i..];
            }
            ';' if depth == 0 => {
                // Comment - skip to end of line
                while let Some((j, ch)) = chars.next() {
                    if ch == '\n' {
                        return skip_form(&s[j + 1..]);
                    }
                }
                return "";
            }
            _ => {}
        }
    }

    ""
}

impl Default for Evaluator {
    fn default() -> Self {
        Evaluator::new()
    }
}

/// Check if a function body is eligible for JIT compilation
/// A body is eligible if it only uses:
/// - Integer literals
/// - Boolean literals
/// - Parameter references
/// - Supported operations (+, -, *, /, inc, dec, <, >, <=, >=, =)
/// - Control flow (if, do, loop, recur)
/// - Logical (not, and, or)
/// - Predicates (zero?, pos?, neg?)
fn is_jit_eligible(expr: &Value) -> bool {
    match expr {
        Value::Integer(_) => true,
        Value::Bool(_) => true,
        Value::Symbol(_) => true, // Assume all symbols are parameters
        Value::List(list) => {
            if list.is_empty() {
                return false;
            }

            let head = list.head().unwrap();

            // Check if it's a supported operation
            if let Value::Symbol(sym) = &head {
                // Only bare symbols are supported (no namespace)
                // This prevents native/zero? from matching "zero?"
                if sym.has_namespace() {
                    return false;
                }

                let supported = matches!(
                    sym.name(),
                    "+" | "-" | "*" | "/" | "inc" | "dec" |
                    "<" | ">" | "<=" | ">=" | "=" |
                    "if" | "do" | "loop" | "recur" |
                    "not" | "and" | "or" |
                    "zero?" | "pos?" | "neg?"
                );

                if !supported {
                    return false;
                }

                // Check that all arguments are also eligible
                list.iter().skip(1).all(|arg| is_jit_eligible(&arg))
            } else {
                false
            }
        }
        Value::Vector(v) => {
            // Vectors in loop bindings should be checked for eligible init expressions
            v.iter().all(|elem| {
                matches!(elem, Value::Symbol(_) | Value::Integer(_) | Value::Bool(_))
                    || is_jit_eligible(elem)
            })
        }
        _ => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::reader::read_string;

    fn eval(input: &str) -> JankResult<Value> {
        let form = read_string(input)?;
        let mut evaluator = Evaluator::new();
        evaluator.eval(&form)
    }

    #[test]
    fn test_eval_literals() {
        assert_eq!(eval("42").unwrap(), Value::Integer(42));
        assert_eq!(eval("3.14").unwrap(), Value::Float(3.14));
        assert_eq!(eval("true").unwrap(), Value::Bool(true));
        assert_eq!(eval("nil").unwrap(), Value::Nil);
    }

    #[test]
    fn test_eval_arithmetic() {
        assert_eq!(eval("(+ 1 2)").unwrap(), Value::Integer(3));
        assert_eq!(eval("(- 10 4)").unwrap(), Value::Integer(6));
        assert_eq!(eval("(* 3 4)").unwrap(), Value::Integer(12));
        assert_eq!(eval("(/ 10 2)").unwrap(), Value::Integer(5));
    }

    #[test]
    fn test_eval_nested() {
        assert_eq!(eval("(+ 1 (+ 2 3))").unwrap(), Value::Integer(6));
        assert_eq!(eval("(* 2 (+ 1 3))").unwrap(), Value::Integer(8));
    }

    #[test]
    fn test_eval_if() {
        assert_eq!(eval("(if true 1 2)").unwrap(), Value::Integer(1));
        assert_eq!(eval("(if false 1 2)").unwrap(), Value::Integer(2));
        assert_eq!(eval("(if nil 1 2)").unwrap(), Value::Integer(2));
        assert_eq!(eval("(if 0 1 2)").unwrap(), Value::Integer(1)); // 0 is truthy
    }

    #[test]
    fn test_eval_let() {
        assert_eq!(eval("(let [x 10] x)").unwrap(), Value::Integer(10));
        assert_eq!(eval("(let [x 1 y 2] (+ x y))").unwrap(), Value::Integer(3));
        assert_eq!(eval("(let [x 1 y (+ x 1)] y)").unwrap(), Value::Integer(2));
    }

    #[test]
    fn test_eval_def() {
        let mut evaluator = Evaluator::new();
        evaluator.eval(&read_string("(def x 42)").unwrap()).unwrap();
        assert_eq!(
            evaluator.eval(&read_string("x").unwrap()).unwrap(),
            Value::Integer(42)
        );
    }

    #[test]
    fn test_eval_fn() {
        assert_eq!(
            eval("((fn [x] x) 42)").unwrap(),
            Value::Integer(42)
        );
        assert_eq!(
            eval("((fn [x y] (+ x y)) 1 2)").unwrap(),
            Value::Integer(3)
        );
    }

    #[test]
    fn test_eval_defn() {
        let mut evaluator = Evaluator::new();
        evaluator.eval(&read_string("(defn square [x] (* x x))").unwrap()).unwrap();
        assert_eq!(
            evaluator.eval(&read_string("(square 5)").unwrap()).unwrap(),
            Value::Integer(25)
        );
    }

    #[test]
    fn test_jit_eager_compilation() {
        // Test that functions get JIT compiled at DEFINITION time (not call time!)
        let mut evaluator = Evaluator::new();

        // Define a simple numeric function - this IMMEDIATELY compiles it!
        evaluator.eval(&read_string("(defn double [x] (* x 2))").unwrap()).unwrap();

        // ALREADY compiled at definition time! No waiting for first call!
        assert!(evaluator.is_compiled("double"));
        assert_eq!(evaluator.call_count("double"), 0); // Not called yet!

        // FIRST call is already fast - no compilation overhead!
        let result = evaluator.eval(&read_string("(double 50)").unwrap()).unwrap();
        assert_eq!(result, Value::Integer(100));

        // All calls go through the JIT path - native speed from the start!
        for i in 1..=100 {
            let result = evaluator.eval(&read_string(&format!("(double {})", i)).unwrap()).unwrap();
            assert_eq!(result, Value::Integer(i * 2));
        }
    }

    #[test]
    fn test_jit_factorial_eager() {
        // Test with a more complex function using loop/recur
        let mut evaluator = Evaluator::new();

        // Define factorial using loop/recur - compiles at definition time!
        evaluator.eval(&read_string(
            "(defn factorial [n] (loop [i n acc 1] (if (<= i 1) acc (recur (dec i) (* acc i)))))"
        ).unwrap()).unwrap();

        // ALREADY compiled at definition time!
        assert!(evaluator.is_compiled("factorial"));

        // First call is already native speed - no compilation overhead!
        let result = evaluator.eval(&read_string("(factorial 10)").unwrap()).unwrap();
        assert_eq!(result, Value::Integer(3628800)); // 10!

        // Verify correct results with compiled version - native speed!
        assert_eq!(
            evaluator.eval(&read_string("(factorial 5)").unwrap()).unwrap(),
            Value::Integer(120)
        );
        assert_eq!(
            evaluator.eval(&read_string("(factorial 20)").unwrap()).unwrap(),
            Value::Integer(2432902008176640000)
        );
    }

    #[test]
    fn test_jit_not_eligible() {
        // Test that functions with unsupported operations don't get compiled
        let mut evaluator = Evaluator::new();

        // Define a function that uses println (not JIT eligible)
        evaluator.eval(&read_string("(defn greet [x] (str \"Hello \" x))").unwrap()).unwrap();

        // Call it many times
        for _ in 0..150 {
            let _ = evaluator.eval(&read_string("(greet \"world\")").unwrap());
        }

        // Should NOT be compiled (str is not a supported JIT operation)
        assert!(!evaluator.is_compiled("greet"));
    }

    #[test]
    fn test_jit_native_speed_factorial() {
        use std::time::Instant;

        let mut evaluator = Evaluator::new();

        // Define factorial using loop/recur
        evaluator.eval(&read_string(
            "(defn factorial [n] (loop [i n acc 1] (if (<= i 1) acc (recur (dec i) (* acc i)))))"
        ).unwrap()).unwrap();

        // Warm up (this compiles the function on first call)
        evaluator.eval(&read_string("(factorial 20)").unwrap()).unwrap();
        assert!(evaluator.is_compiled("factorial"));

        // Benchmark: call factorial(20) 100,000 times
        let iterations = 100_000;
        let start = Instant::now();
        for _ in 0..iterations {
            let result = evaluator.eval(&read_string("(factorial 20)").unwrap()).unwrap();
            assert_eq!(result, Value::Integer(2432902008176640000));
        }
        let elapsed = start.elapsed();

        println!(
            "\nJIT factorial(20) x {}: {:?} ({:?} per call)",
            iterations, elapsed, elapsed / iterations as u32
        );

        // Should complete in under 1 second for 100k calls (that's ~10μs per call)
        // Native code is fast!
        assert!(elapsed.as_secs() < 2, "factorial(20) too slow: {:?}", elapsed);
    }

    #[test]
    fn test_jit_native_speed_fibonacci() {
        use std::time::Instant;

        let mut evaluator = Evaluator::new();

        // Define iterative fibonacci using loop/recur
        evaluator.eval(&read_string(
            "(defn fib [n] (loop [i n a 0 b 1] (if (<= i 0) a (recur (dec i) b (+ a b)))))"
        ).unwrap()).unwrap();

        // Warm up (this compiles the function on first call)
        evaluator.eval(&read_string("(fib 40)").unwrap()).unwrap();
        assert!(evaluator.is_compiled("fib"));

        // Benchmark: call fib(40) 100,000 times
        let iterations = 100_000;
        let start = Instant::now();
        for _ in 0..iterations {
            let result = evaluator.eval(&read_string("(fib 40)").unwrap()).unwrap();
            assert_eq!(result, Value::Integer(102334155));
        }
        let elapsed = start.elapsed();

        println!(
            "\nJIT fib(40) x {}: {:?} ({:?} per call)",
            iterations, elapsed, elapsed / iterations as u32
        );

        // Native code is FAST!
        assert!(elapsed.as_secs() < 2, "fib(40) too slow: {:?}", elapsed);
    }

    /// Pure Rust fibonacci for comparison
    fn rust_fib(n: i64) -> i64 {
        let mut i = n;
        let mut a = 0i64;
        let mut b = 1i64;
        while i > 0 {
            let temp = a + b;
            a = b;
            b = temp;
            i -= 1;
        }
        a
    }

    /// Pure Rust factorial for comparison
    fn rust_factorial(n: i64) -> i64 {
        let mut i = n;
        let mut acc = 1i64;
        while i > 1 {
            acc *= i;
            i -= 1;
        }
        acc
    }

    #[test]
    fn test_jit_vs_rust_speed_comparison() {
        use std::time::Instant;

        let mut evaluator = Evaluator::new();

        // Define jank functions
        evaluator.eval(&read_string(
            "(defn jank-fib [n] (loop [i n a 0 b 1] (if (<= i 0) a (recur (dec i) b (+ a b)))))"
        ).unwrap()).unwrap();
        evaluator.eval(&read_string(
            "(defn jank-factorial [n] (loop [i n acc 1] (if (<= i 1) acc (recur (dec i) (* acc i)))))"
        ).unwrap()).unwrap();

        // Warm up JIT (compiles on first call)
        evaluator.eval(&read_string("(jank-fib 40)").unwrap()).unwrap();
        evaluator.eval(&read_string("(jank-factorial 20)").unwrap()).unwrap();
        assert!(evaluator.is_compiled("jank-fib"));
        assert!(evaluator.is_compiled("jank-factorial"));

        let iterations = 1_000_000;

        // ========== RUST BENCHMARKS ==========
        println!("\n========== SPEED COMPARISON ==========");
        println!("Iterations: {}", iterations);
        println!();

        // Rust fib(40)
        let start = Instant::now();
        for _ in 0..iterations {
            std::hint::black_box(rust_fib(40));
        }
        let rust_fib_time = start.elapsed();
        println!("RUST fib(40) x {}: {:?} ({:?} per call)",
            iterations, rust_fib_time, rust_fib_time / iterations as u32);

        // Rust factorial(20)
        let start = Instant::now();
        for _ in 0..iterations {
            std::hint::black_box(rust_factorial(20));
        }
        let rust_factorial_time = start.elapsed();
        println!("RUST factorial(20) x {}: {:?} ({:?} per call)",
            iterations, rust_factorial_time, rust_factorial_time / iterations as u32);

        // ========== JANK JIT BENCHMARKS ==========
        println!();

        // JIT fib(40) - direct function call (no eval overhead)
        let fib_ptr_raw = evaluator.get_compiled_ptr("jank-fib").unwrap();
        let fib_ptr: extern "C" fn(i64) -> i64 = unsafe { std::mem::transmute(fib_ptr_raw) };

        let start = Instant::now();
        for _ in 0..iterations {
            std::hint::black_box(fib_ptr(40));
        }
        let jit_fib_time = start.elapsed();
        println!("JANK JIT fib(40) x {}: {:?} ({:?} per call)",
            iterations, jit_fib_time, jit_fib_time / iterations as u32);

        // JIT factorial(20) - direct function call (no eval overhead)
        let factorial_ptr_raw = evaluator.get_compiled_ptr("jank-factorial").unwrap();
        let factorial_ptr: extern "C" fn(i64) -> i64 = unsafe { std::mem::transmute(factorial_ptr_raw) };

        let start = Instant::now();
        for _ in 0..iterations {
            std::hint::black_box(factorial_ptr(20));
        }
        let jit_factorial_time = start.elapsed();
        println!("JANK JIT factorial(20) x {}: {:?} ({:?} per call)",
            iterations, jit_factorial_time, jit_factorial_time / iterations as u32);

        // ========== COMPARISON ==========
        println!();
        println!("========== RESULTS ==========");
        let fib_ratio = jit_fib_time.as_nanos() as f64 / rust_fib_time.as_nanos() as f64;
        let factorial_ratio = jit_factorial_time.as_nanos() as f64 / rust_factorial_time.as_nanos() as f64;

        println!("fib(40): JANK JIT is {:.2}x vs Rust", fib_ratio);
        println!("factorial(20): JANK JIT is {:.2}x vs Rust", factorial_ratio);

        // Verify correctness
        assert_eq!(rust_fib(40), 102334155);
        assert_eq!(rust_factorial(20), 2432902008176640000);
        assert_eq!(fib_ptr(40), 102334155);
        assert_eq!(factorial_ptr(20), 2432902008176640000);

        println!("==============================\n");
    }

    // ==================== NAMESPACE TESTS ====================

    #[test]
    fn test_ns_declaration() {
        let mut evaluator = Evaluator::new();

        // Declare a namespace
        evaluator.eval(&read_string("(ns myapp.core)").unwrap()).unwrap();

        // Check that we switched to the new namespace
        assert_eq!(evaluator.namespaces().current_name(), "myapp.core");
    }

    #[test]
    fn test_in_ns() {
        let mut evaluator = Evaluator::new();

        // Start in user namespace
        assert_eq!(evaluator.namespaces().current_name(), "user");

        // Switch to a new namespace using in-ns
        evaluator.eval(&read_string("(in-ns 'myapp.core)").unwrap()).unwrap();
        assert_eq!(evaluator.namespaces().current_name(), "myapp.core");

        // Define something in this namespace
        evaluator.eval(&read_string("(def x 42)").unwrap()).unwrap();

        // Switch back to user
        evaluator.eval(&read_string("(in-ns 'user)").unwrap()).unwrap();
        assert_eq!(evaluator.namespaces().current_name(), "user");

        // x should not be visible here
        let result = evaluator.eval(&read_string("x").unwrap());
        assert!(result.is_err());

        // But we can access it with qualified name
        let result = evaluator.eval(&read_string("myapp.core/x").unwrap()).unwrap();
        assert_eq!(result, Value::Integer(42));
    }

    #[test]
    fn test_qualified_symbol_resolution() {
        let mut evaluator = Evaluator::new();

        // Create another namespace with a definition
        evaluator.eval(&read_string("(ns other.ns)").unwrap()).unwrap();
        evaluator.eval(&read_string("(def answer 42)").unwrap()).unwrap();

        // Switch to user namespace
        evaluator.eval(&read_string("(in-ns 'user)").unwrap()).unwrap();

        // Access via qualified symbol
        let result = evaluator.eval(&read_string("other.ns/answer").unwrap()).unwrap();
        assert_eq!(result, Value::Integer(42));
    }

    #[test]
    fn test_defn_in_namespace() {
        let mut evaluator = Evaluator::new();

        // Create a math namespace with a function
        evaluator.eval(&read_string("(ns myapp.math)").unwrap()).unwrap();
        evaluator.eval(&read_string("(defn square [x] (* x x))").unwrap()).unwrap();

        // Switch to user namespace
        evaluator.eval(&read_string("(in-ns 'user)").unwrap()).unwrap();

        // Call the function using qualified name
        let result = evaluator.eval(&read_string("(myapp.math/square 5)").unwrap()).unwrap();
        assert_eq!(result, Value::Integer(25));
    }

    #[test]
    fn test_namespace_isolation() {
        let mut evaluator = Evaluator::new();

        // Define x in user namespace
        evaluator.eval(&read_string("(def x 1)").unwrap()).unwrap();

        // Define x in another namespace
        evaluator.eval(&read_string("(ns other)").unwrap()).unwrap();
        evaluator.eval(&read_string("(def x 2)").unwrap()).unwrap();

        // Check they're different
        assert_eq!(
            evaluator.eval(&read_string("x").unwrap()).unwrap(),
            Value::Integer(2)
        );
        evaluator.eval(&read_string("(in-ns 'user)").unwrap()).unwrap();
        assert_eq!(
            evaluator.eval(&read_string("x").unwrap()).unwrap(),
            Value::Integer(1)
        );
    }

    // ==================== .JRS FILE LOADING TESTS ====================

    #[test]
    fn test_load_simple_jrs_file() {
        let mut evaluator = Evaluator::new();

        // Add test resources to source path
        evaluator.add_source_path("test_resources");

        // Require the simple namespace
        evaluator.eval(&read_string("(require simple)").unwrap()).unwrap();

        // Access the defined value through qualified symbol
        let result = evaluator.eval(&read_string("simple/answer").unwrap()).unwrap();
        assert_eq!(result, Value::Integer(42));
    }

    #[test]
    fn test_load_jrs_with_functions() {
        let mut evaluator = Evaluator::new();
        evaluator.add_source_path("test_resources");

        // Require myapp.math namespace
        evaluator.eval(&read_string("(require myapp.math)").unwrap()).unwrap();

        // Call functions from the loaded namespace
        let result = evaluator.eval(&read_string("(myapp.math/square 5)").unwrap()).unwrap();
        assert_eq!(result, Value::Integer(25));

        let result = evaluator.eval(&read_string("(myapp.math/cube 3)").unwrap()).unwrap();
        assert_eq!(result, Value::Integer(27));
    }

    #[test]
    fn test_load_jrs_with_alias() {
        let mut evaluator = Evaluator::new();
        evaluator.add_source_path("test_resources");

        // Require with alias
        evaluator.eval(&read_string("(require [myapp.math :as m])").unwrap()).unwrap();

        // Use the alias
        let result = evaluator.eval(&read_string("(m/square 7)").unwrap()).unwrap();
        assert_eq!(result, Value::Integer(49));
    }

    #[test]
    fn test_ns_with_require() {
        let mut evaluator = Evaluator::new();
        evaluator.add_source_path("test_resources");

        // Load myapp.core which requires myapp.math
        evaluator.eval(&read_string("(require myapp.core)").unwrap()).unwrap();

        // Call functions from myapp.core that use myapp.math
        let result = evaluator.eval(&read_string("(myapp.core/area 6)").unwrap()).unwrap();
        assert_eq!(result, Value::Integer(36));

        let result = evaluator.eval(&read_string("(myapp.core/volume 4)").unwrap()).unwrap();
        assert_eq!(result, Value::Integer(64));
    }

    #[test]
    fn test_ns_form_with_require() {
        let mut evaluator = Evaluator::new();
        evaluator.add_source_path("test_resources");

        // Use ns form with :require clause
        evaluator.eval(&read_string("(ns my.test (:require [myapp.math :as math]))").unwrap()).unwrap();

        // Should be in my.test namespace now
        assert_eq!(evaluator.namespaces().current_name(), "my.test");

        // Can use the alias
        let result = evaluator.eval(&read_string("(math/square 10)").unwrap()).unwrap();
        assert_eq!(result, Value::Integer(100));
    }

    // ==================== MACRO TESTS ====================

    #[test]
    fn test_syntax_quote_basic() {
        let mut evaluator = Evaluator::new();

        // Simple syntax-quote returns the form
        let result = evaluator.eval(&read_string("`(a b c)").unwrap()).unwrap();
        assert_eq!(result.to_string(), "(a b c)");

        // With unquote
        let result = evaluator.eval(&read_string("(let [x 42] `(a ~x c))").unwrap()).unwrap();
        assert_eq!(result.to_string(), "(a 42 c)");
    }

    #[test]
    fn test_syntax_quote_unquote_splice() {
        let mut evaluator = Evaluator::new();

        // Unquote-splicing
        let result = evaluator.eval(&read_string("(let [xs [1 2 3]] `(a ~@xs b))").unwrap()).unwrap();
        assert_eq!(result.to_string(), "(a 1 2 3 b)");
    }

    #[test]
    fn test_defmacro_basic() {
        let mut evaluator = Evaluator::new();

        // Define a simple macro
        evaluator.eval(&read_string("(defmacro when [test body] `(if ~test ~body nil))").unwrap()).unwrap();

        // Use the macro - should expand and evaluate
        let result = evaluator.eval(&read_string("(when true 42)").unwrap()).unwrap();
        assert_eq!(result, Value::Integer(42));

        let result = evaluator.eval(&read_string("(when false 42)").unwrap()).unwrap();
        assert_eq!(result, Value::Nil);
    }

    #[test]
    fn test_defmacro_with_body() {
        let mut evaluator = Evaluator::new();

        // Define 'when' macro with multiple body forms
        evaluator.eval(&read_string(
            "(defmacro my-when [test & body] `(if ~test (do ~@body) nil))"
        ).unwrap()).unwrap();

        // Use it
        evaluator.eval(&read_string("(def result 0)").unwrap()).unwrap();
        evaluator.eval(&read_string("(my-when true (def result 1) (def result 2))").unwrap()).unwrap();
        let result = evaluator.eval(&read_string("result").unwrap()).unwrap();
        assert_eq!(result, Value::Integer(2));
    }

    #[test]
    fn test_defmacro_when_not() {
        let mut evaluator = Evaluator::new();

        // Define when-not macro
        evaluator.eval(&read_string(
            "(defmacro when-not [test & body] `(if ~test nil (do ~@body)))"
        ).unwrap()).unwrap();

        let result = evaluator.eval(&read_string("(when-not false 42)").unwrap()).unwrap();
        assert_eq!(result, Value::Integer(42));

        let result = evaluator.eval(&read_string("(when-not true 42)").unwrap()).unwrap();
        assert_eq!(result, Value::Nil);
    }

    // ==================== CORE.JRS TESTS ====================

    #[test]
    fn test_core_jrs_load() {
        let mut evaluator = Evaluator::new();
        evaluator.add_source_path("test_resources");

        // Load clojure.core
        evaluator.eval(&read_string("(require clojure.core)").unwrap()).unwrap();

        // Test that it loaded
        assert!(evaluator.namespaces().get("clojure.core").is_some());
    }

    #[test]
    fn test_core_jrs_when_macro() {
        let mut evaluator = Evaluator::new();
        evaluator.add_source_path("test_resources");
        evaluator.eval(&read_string("(require [clojure.core :refer [when when-not]])").unwrap()).unwrap();

        let result = evaluator.eval(&read_string("(when true 42)").unwrap()).unwrap();
        assert_eq!(result, Value::Integer(42));

        let result = evaluator.eval(&read_string("(when false 42)").unwrap()).unwrap();
        assert_eq!(result, Value::Nil);

        let result = evaluator.eval(&read_string("(when-not false 42)").unwrap()).unwrap();
        assert_eq!(result, Value::Integer(42));
    }

    #[test]
    fn test_core_jrs_sequence_functions() {
        let mut evaluator = Evaluator::new();
        evaluator.add_source_path("test_resources");
        evaluator.eval(&read_string("(require [clojure.core :refer [second ffirst nnext]])").unwrap()).unwrap();

        let result = evaluator.eval(&read_string("(second [1 2 3])").unwrap()).unwrap();
        assert_eq!(result, Value::Integer(2));

        let result = evaluator.eval(&read_string("(ffirst [[1 2] [3 4]])").unwrap()).unwrap();
        assert_eq!(result, Value::Integer(1));

        // nnext returns a list (from next), not a vector
        let result = evaluator.eval(&read_string("(nnext [1 2 3 4])").unwrap()).unwrap();
        match result {
            Value::List(l) => assert_eq!(l.len(), 2),
            Value::Vector(v) => assert_eq!(v.len(), 2),
            _ => panic!("Expected list or vector, got {:?}", result),
        }
    }

    #[test]
    fn test_core_jrs_map_function() {
        let mut evaluator = Evaluator::new();
        evaluator.add_source_path("test_resources");
        evaluator.eval(&read_string("(require [clojure.core :refer [map]])").unwrap()).unwrap();

        // Define a simple function to use
        evaluator.eval(&read_string("(defn double [x] (* x 2))").unwrap()).unwrap();

        let result = evaluator.eval(&read_string("(map double [1 2 3])").unwrap()).unwrap();
        if let Value::Vector(v) = result {
            assert_eq!(v.len(), 3);
            assert_eq!(v[0], Value::Integer(2));
            assert_eq!(v[1], Value::Integer(4));
            assert_eq!(v[2], Value::Integer(6));
        } else {
            panic!("Expected vector, got {:?}", result);
        }
    }

    #[test]
    fn test_core_jrs_filter_function() {
        let mut evaluator = Evaluator::new();
        evaluator.add_source_path("test_resources");
        evaluator.eval(&read_string("(require [clojure.core :refer [filter]])").unwrap()).unwrap();

        let result = evaluator.eval(&read_string("(filter even? [1 2 3 4 5 6])").unwrap()).unwrap();
        if let Value::Vector(v) = result {
            assert_eq!(v.len(), 3);
            assert_eq!(v[0], Value::Integer(2));
            assert_eq!(v[1], Value::Integer(4));
            assert_eq!(v[2], Value::Integer(6));
        } else {
            panic!("Expected vector, got {:?}", result);
        }
    }

    #[test]
    fn test_core_jrs_reduce_function() {
        let mut evaluator = Evaluator::new();
        evaluator.add_source_path("test_resources");
        evaluator.eval(&read_string("(require [clojure.core :refer [reduce]])").unwrap()).unwrap();

        let result = evaluator.eval(&read_string("(reduce + 0 [1 2 3 4 5])").unwrap()).unwrap();
        assert_eq!(result, Value::Integer(15));

        let result = evaluator.eval(&read_string("(reduce * 1 [1 2 3 4 5])").unwrap()).unwrap();
        assert_eq!(result, Value::Integer(120));
    }

    #[test]
    fn test_core_jrs_every_and_some() {
        let mut evaluator = Evaluator::new();
        evaluator.add_source_path("test_resources");
        evaluator.eval(&read_string("(require [clojure.core :refer [every? some]])").unwrap()).unwrap();

        let result = evaluator.eval(&read_string("(every? even? [2 4 6 8])").unwrap()).unwrap();
        assert_eq!(result, Value::Bool(true));

        let result = evaluator.eval(&read_string("(every? even? [2 3 6 8])").unwrap()).unwrap();
        assert_eq!(result, Value::Bool(false));

        let result = evaluator.eval(&read_string("(some even? [1 3 5 6])").unwrap()).unwrap();
        assert_eq!(result, Value::Bool(true));

        let result = evaluator.eval(&read_string("(some even? [1 3 5 7])").unwrap()).unwrap();
        assert_eq!(result, Value::Nil);
    }

    #[test]
    fn test_core_jrs_take_drop() {
        let mut evaluator = Evaluator::new();
        evaluator.add_source_path("test_resources");
        evaluator.eval(&read_string("(require [clojure.core :refer [take drop]])").unwrap()).unwrap();

        // take builds a new vector
        let result = evaluator.eval(&read_string("(take 3 [1 2 3 4 5])").unwrap()).unwrap();
        match &result {
            Value::Vector(v) => {
                assert_eq!(v.len(), 3);
                assert_eq!(v[0], Value::Integer(1));
                assert_eq!(v[2], Value::Integer(3));
            }
            Value::List(l) => {
                assert_eq!(l.len(), 3);
            }
            _ => panic!("Expected vector or list, got {:?}", result),
        }

        // drop returns remaining sequence (list from rest)
        let result = evaluator.eval(&read_string("(drop 2 [1 2 3 4 5])").unwrap()).unwrap();
        match &result {
            Value::Vector(v) => {
                assert_eq!(v.len(), 3);
                assert_eq!(v[0], Value::Integer(3));
            }
            Value::List(l) => {
                assert_eq!(l.len(), 3);
                // First element should be 3
                if let Some(first) = l.head() {
                    assert_eq!(first.clone(), Value::Integer(3));
                }
            }
            _ => panic!("Expected vector or list, got {:?}", result),
        }
    }
}
