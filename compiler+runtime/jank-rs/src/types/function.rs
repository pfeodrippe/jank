//! Function types for jank-rs
//!
//! This module defines both native Rust functions and interpreted Clojure functions.

use std::fmt;
use std::sync::Arc;

use crate::types::symbol::Symbol;

/// A native Rust function that can be called from Clojure
pub type NativeFn = fn(&[super::value::Value]) -> Result<super::value::Value, crate::error::JankError>;

/// Arity specification for a function
#[derive(Clone, Debug)]
pub enum Arity {
    /// Fixed number of arguments
    Fixed(usize),
    /// Minimum args before variadic rest parameter
    Variadic(usize),
    /// Multiple fixed arities (like multi-arity defn)
    Multi(Vec<usize>),
}

impl Arity {
    /// Check if the given argument count is valid for this arity
    pub fn accepts(&self, arg_count: usize) -> bool {
        match self {
            Arity::Fixed(n) => arg_count == *n,
            Arity::Variadic(min) => arg_count >= *min,
            Arity::Multi(arities) => arities.contains(&arg_count),
        }
    }

    /// Get the minimum number of arguments required
    pub fn min_args(&self) -> usize {
        match self {
            Arity::Fixed(n) => *n,
            Arity::Variadic(min) => *min,
            Arity::Multi(arities) => arities.iter().copied().min().unwrap_or(0),
        }
    }

    /// Get the maximum number of arguments (None for variadic)
    pub fn max_args(&self) -> Option<usize> {
        match self {
            Arity::Fixed(n) => Some(*n),
            Arity::Variadic(_) => None,
            Arity::Multi(arities) => arities.iter().copied().max(),
        }
    }
}

/// A function in jank (either native or interpreted)
#[derive(Clone)]
pub enum Function {
    /// A native Rust function
    Native {
        name: String,
        func: NativeFn,
        arity: Arity,
        doc: Option<String>,
    },

    /// An interpreted Clojure function (defined with fn or defn)
    Interpreted {
        name: Option<Symbol>,
        params: Vec<Symbol>,
        body: Arc<super::value::Value>,
        env: Arc<crate::runtime::env::Environment>,
        is_variadic: bool,
        variadic_param: Option<Symbol>,
        doc: Option<String>,
        /// The namespace where this function was defined (for alias resolution)
        defining_ns: Option<String>,
    },

    /// A closure (captures environment)
    Closure {
        name: Option<Symbol>,
        params: Vec<Symbol>,
        body: Arc<super::value::Value>,
        captured_env: Arc<crate::runtime::env::Environment>,
        is_variadic: bool,
        variadic_param: Option<Symbol>,
        /// The namespace where this closure was defined (for alias resolution)
        defining_ns: Option<String>,
    },

    /// A macro (like a function but transforms code)
    Macro {
        name: Symbol,
        params: Vec<Symbol>,
        body: Arc<super::value::Value>,
        env: Arc<crate::runtime::env::Environment>,
        is_variadic: bool,
        variadic_param: Option<Symbol>,
        doc: Option<String>,
        /// The namespace where this macro was defined (for alias resolution)
        defining_ns: Option<String>,
    },

    /// A multi-arity macro
    MacroMulti {
        name: Symbol,
        /// Each arity is: (params, variadic_param, body)
        arities: Vec<(Vec<Symbol>, Option<Symbol>, Arc<super::value::Value>)>,
        env: Arc<crate::runtime::env::Environment>,
        doc: Option<String>,
        defining_ns: Option<String>,
    },

    /// A multi-arity interpreted function (defined with defn with multiple arities)
    InterpretedMulti {
        name: Option<Symbol>,
        /// Each arity is: (params, variadic_param, body)
        arities: Vec<(Vec<Symbol>, Option<Symbol>, Arc<super::value::Value>)>,
        env: Arc<crate::runtime::env::Environment>,
        doc: Option<String>,
        defining_ns: Option<String>,
    },

    /// A partially applied function
    Partial {
        func: Arc<Function>,
        applied_args: Vec<super::value::Value>,
    },

    /// A composed function (comp)
    Composed {
        functions: Vec<Arc<Function>>,
    },
}

impl Function {
    /// Create a new native function
    pub fn native(name: impl Into<String>, func: NativeFn, arity: Arity) -> Self {
        Function::Native {
            name: name.into(),
            func,
            arity,
            doc: None,
        }
    }

    /// Create a new native function with documentation
    pub fn native_with_doc(
        name: impl Into<String>,
        func: NativeFn,
        arity: Arity,
        doc: impl Into<String>,
    ) -> Self {
        Function::Native {
            name: name.into(),
            func,
            arity,
            doc: Some(doc.into()),
        }
    }

    /// Get the function's name
    pub fn name(&self) -> Option<&str> {
        match self {
            Function::Native { name, .. } => Some(name),
            Function::Interpreted { name, .. } => name.as_ref().map(|s| s.name()),
            Function::InterpretedMulti { name, .. } => name.as_ref().map(|s| s.name()),
            Function::Closure { name, .. } => name.as_ref().map(|s| s.name()),
            Function::Macro { name, .. } => Some(name.name()),
            Function::MacroMulti { name, .. } => Some(name.name()),
            Function::Partial { func, .. } => func.name(),
            Function::Composed { .. } => None,
        }
    }

    /// Get the function's arity
    pub fn arity(&self) -> Arity {
        match self {
            Function::Native { arity, .. } => arity.clone(),
            Function::Interpreted {
                params,
                is_variadic,
                ..
            } => {
                if *is_variadic {
                    Arity::Variadic(params.len().saturating_sub(1))
                } else {
                    Arity::Fixed(params.len())
                }
            }
            Function::Closure {
                params,
                is_variadic,
                ..
            } => {
                if *is_variadic {
                    Arity::Variadic(params.len().saturating_sub(1))
                } else {
                    Arity::Fixed(params.len())
                }
            }
            Function::Macro {
                params,
                is_variadic,
                ..
            } => {
                if *is_variadic {
                    Arity::Variadic(params.len().saturating_sub(1))
                } else {
                    Arity::Fixed(params.len())
                }
            }
            Function::MacroMulti { arities, .. } | Function::InterpretedMulti { arities, .. } => {
                // Collect all fixed arities + check for variadic
                let mut fixed_arities = Vec::new();
                let mut has_variadic = false;
                let mut variadic_min = 0;
                for (params, variadic_param, _) in arities {
                    if variadic_param.is_some() {
                        has_variadic = true;
                        variadic_min = params.len();
                    } else {
                        fixed_arities.push(params.len());
                    }
                }
                if has_variadic {
                    Arity::Variadic(variadic_min)
                } else {
                    Arity::Multi(fixed_arities)
                }
            }
            Function::Partial { func, applied_args } => {
                let base_arity = func.arity();
                match base_arity {
                    Arity::Fixed(n) => Arity::Fixed(n.saturating_sub(applied_args.len())),
                    Arity::Variadic(min) => {
                        Arity::Variadic(min.saturating_sub(applied_args.len()))
                    }
                    Arity::Multi(arities) => Arity::Multi(
                        arities
                            .into_iter()
                            .filter_map(|n| n.checked_sub(applied_args.len()))
                            .collect(),
                    ),
                }
            }
            Function::Composed { .. } => Arity::Fixed(1),
        }
    }

    /// Check if the function accepts the given number of arguments
    pub fn accepts_arity(&self, arg_count: usize) -> bool {
        self.arity().accepts(arg_count)
    }

    /// Check if this is a macro
    pub fn is_macro(&self) -> bool {
        matches!(self, Function::Macro { .. } | Function::MacroMulti { .. })
    }

    /// Get the documentation string
    pub fn doc(&self) -> Option<&str> {
        match self {
            Function::Native { doc, .. } => doc.as_deref(),
            Function::Interpreted { doc, .. } => doc.as_deref(),
            Function::InterpretedMulti { doc, .. } => doc.as_deref(),
            Function::Closure { .. } => None,
            Function::Macro { doc, .. } => doc.as_deref(),
            Function::MacroMulti { doc, .. } => doc.as_deref(),
            Function::Partial { func, .. } => func.doc(),
            Function::Composed { .. } => None,
        }
    }

    /// Get the defining namespace
    pub fn defining_ns(&self) -> Option<&str> {
        match self {
            Function::Native { .. } => None,
            Function::Interpreted { defining_ns, .. } => defining_ns.as_deref(),
            Function::InterpretedMulti { defining_ns, .. } => defining_ns.as_deref(),
            Function::Closure { defining_ns, .. } => defining_ns.as_deref(),
            Function::Macro { defining_ns, .. } => defining_ns.as_deref(),
            Function::MacroMulti { defining_ns, .. } => defining_ns.as_deref(),
            Function::Partial { func, .. } => func.defining_ns(),
            Function::Composed { .. } => None,
        }
    }
}

impl fmt::Display for Function {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Function::Native { name, arity, .. } => {
                write!(f, "#<native-fn {} {:?}>", name, arity)
            }
            Function::Interpreted { name, params, .. } => {
                let name_str = name.as_ref().map(|s| s.to_string()).unwrap_or_else(|| "anonymous".to_string());
                write!(f, "#<fn {} [{} args]>", name_str, params.len())
            }
            Function::Closure { name, params, .. } => {
                let name_str = name.as_ref().map(|s| s.to_string()).unwrap_or_else(|| "anonymous".to_string());
                write!(f, "#<closure {} [{} args]>", name_str, params.len())
            }
            Function::Macro { name, params, .. } => {
                write!(f, "#<macro {} [{} args]>", name, params.len())
            }
            Function::MacroMulti { name, arities, .. } => {
                let arity_strs: Vec<String> = arities.iter()
                    .map(|(params, var, _)| {
                        if var.is_some() {
                            format!("{}+", params.len())
                        } else {
                            params.len().to_string()
                        }
                    })
                    .collect();
                write!(f, "#<macro {} [{}]>", name, arity_strs.join(","))
            }
            Function::InterpretedMulti { name, arities, .. } => {
                let name_str = name.as_ref().map(|s| s.to_string()).unwrap_or_else(|| "anonymous".to_string());
                let arity_strs: Vec<String> = arities.iter()
                    .map(|(params, var, _)| {
                        if var.is_some() {
                            format!("{}+", params.len())
                        } else {
                            params.len().to_string()
                        }
                    })
                    .collect();
                write!(f, "#<fn {} [{}]>", name_str, arity_strs.join(","))
            }
            Function::Partial { func, applied_args } => {
                write!(f, "#<partial {} [{} applied]>", func, applied_args.len())
            }
            Function::Composed { functions } => {
                write!(f, "#<composed [{} fns]>", functions.len())
            }
        }
    }
}

impl fmt::Debug for Function {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_arity_fixed() {
        let arity = Arity::Fixed(2);
        assert!(arity.accepts(2));
        assert!(!arity.accepts(1));
        assert!(!arity.accepts(3));
    }

    #[test]
    fn test_arity_variadic() {
        let arity = Arity::Variadic(1);
        assert!(!arity.accepts(0));
        assert!(arity.accepts(1));
        assert!(arity.accepts(2));
        assert!(arity.accepts(100));
    }

    #[test]
    fn test_arity_multi() {
        let arity = Arity::Multi(vec![1, 2, 3]);
        assert!(!arity.accepts(0));
        assert!(arity.accepts(1));
        assert!(arity.accepts(2));
        assert!(arity.accepts(3));
        assert!(!arity.accepts(4));
    }
}
