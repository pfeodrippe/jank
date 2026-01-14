//! Evaluator for jank-rs
//!
//! This module implements the interpreter that evaluates Clojure forms.

use std::sync::Arc;

use crate::types::{Value, Symbol, Function, Arity, List};
use crate::error::{JankError, JankResult};
use crate::runtime::env::Environment;

/// Maximum recursion depth before stack overflow
const MAX_RECURSION_DEPTH: usize = 10000;

/// The evaluator for Clojure expressions
pub struct Evaluator {
    /// Global environment with core functions
    global_env: Arc<Environment>,
    /// Current recursion depth
    depth: usize,
}

impl Evaluator {
    /// Create a new evaluator with standard library
    pub fn new() -> Self {
        let global_env = Arc::new(Environment::new());

        // Load core functions
        crate::runtime::core::load_core(&global_env);

        Evaluator {
            global_env,
            depth: 0,
        }
    }

    /// Create evaluator with a custom environment
    pub fn with_env(env: Arc<Environment>) -> Self {
        Evaluator {
            global_env: env,
            depth: 0,
        }
    }

    /// Get the global environment
    pub fn global_env(&self) -> Arc<Environment> {
        Arc::clone(&self.global_env)
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

            // Symbols are looked up in the environment
            Value::Symbol(sym) => {
                env.get_symbol(sym)
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
                        "if" => return self.eval_if(list, Arc::clone(&env)),
                        "do" => return self.eval_do(list, Arc::clone(&env)),
                        "let" => return self.eval_let(list, Arc::clone(&env)),
                        "fn" => return self.eval_fn(list, Arc::clone(&env)),
                        "def" => return self.eval_def(list, Arc::clone(&env)),
                        "defn" => return self.eval_defn(list, Arc::clone(&env)),
                        "defmacro" => return self.eval_defmacro(list, Arc::clone(&env)),
                        "loop" => return self.eval_loop(list, Arc::clone(&env)),
                        "recur" => return self.eval_recur(list, Arc::clone(&env)),
                        _ => {}
                    }
                }

                // Regular function call
                let func_val = self.eval_in_env(&head, Arc::clone(&env))?;

                // Collect arguments
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

            Function::Interpreted { params, body, env, is_variadic, variadic_param, .. } |
            Function::Closure { params, body, captured_env: env, is_variadic, variadic_param, .. } => {
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

                // Create new environment with parameters bound
                let local_env = Arc::new(Environment::with_fn_args(
                    Arc::clone(env),
                    params,
                    args,
                    variadic_param.as_ref(),
                )?);

                // Evaluate body
                self.eval_in_env(body, local_env)
            }

            Function::Macro { .. } => {
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
            Value::Vector(_) => (None, 0),
            _ => return Err(JankError::type_error("symbol or vector", args[0].type_name())),
        };

        if params_idx >= args.len() {
            return Err(JankError::parse("fn requires parameters vector", 0, 0));
        }

        // Get parameters
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
        })))
    }

    fn eval_def(&mut self, list: &List, env: Arc<Environment>) -> JankResult<Value> {
        let args: Vec<Value> = list.iter().skip(1).cloned().collect();
        if args.is_empty() || args.len() > 2 {
            return Err(JankError::arity("def", "1 or 2", args.len()));
        }

        let name = match &args[0] {
            Value::Symbol(s) => s.name().to_string(),
            _ => return Err(JankError::type_error("symbol", args[0].type_name())),
        };

        let value = if args.len() == 2 {
            self.eval_in_env(&args[1], Arc::clone(&env))?
        } else {
            Value::Nil
        };

        // Define in global environment
        self.global_env.define(&name, value.clone());

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
        let (doc, params_idx) = match &args[1] {
            Value::String(s) if args.len() > 2 => (Some(s.as_ref().clone()), 2),
            _ => (None, 1),
        };

        // Get parameters
        let params_vec = match &args[params_idx] {
            Value::Vector(v) => v,
            _ => return Err(JankError::type_error("vector", args[params_idx].type_name())),
        };

        // Parse parameters
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

        // Create body
        let body = if args.len() - params_idx - 1 == 1 {
            args[params_idx + 1].clone()
        } else {
            let mut do_list = vec![Value::Symbol(Symbol::new("do"))];
            do_list.extend(args.iter().skip(params_idx + 1).cloned());
            Value::list(do_list)
        };

        let func = Function::Interpreted {
            name: Some(name.clone()),
            params,
            body: Arc::new(body),
            env,
            is_variadic,
            variadic_param,
            doc,
        };

        // Define in global environment
        let func_val = Value::Function(Arc::new(func));
        self.global_env.define(name.name(), func_val);

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
        let (doc, params_idx) = match &args[1] {
            Value::String(s) if args.len() > 2 => (Some(s.as_ref().clone()), 2),
            _ => (None, 1),
        };

        // Get parameters
        let params_vec = match &args[params_idx] {
            Value::Vector(v) => v,
            _ => return Err(JankError::type_error("vector", args[params_idx].type_name())),
        };

        // Parse parameters
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

        // Create body
        let body = if args.len() - params_idx - 1 == 1 {
            args[params_idx + 1].clone()
        } else {
            let mut do_list = vec![Value::Symbol(Symbol::new("do"))];
            do_list.extend(args.iter().skip(params_idx + 1).cloned());
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
        };

        // Define in global environment
        let macro_val = Value::Function(Arc::new(mac));
        self.global_env.define(name.name(), macro_val);

        Ok(Value::Symbol(name))
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
}

impl Default for Evaluator {
    fn default() -> Self {
        Evaluator::new()
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
}
