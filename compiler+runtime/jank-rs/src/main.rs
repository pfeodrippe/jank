//! jank-rs REPL
//!
//! A simple Read-Eval-Print Loop for jank-rs.

use std::io::{self, Write};

use rustyline::error::ReadlineError;
use rustyline::{DefaultEditor, Result as RustylineResult};

use jank_rs::{Evaluator, read_all, VERSION};

fn main() {
    println!("jank-rs v{}", VERSION);
    println!("A Clojure dialect in Rust");
    println!("Type :help for help, :quit to exit\n");

    if let Err(e) = run_repl() {
        eprintln!("REPL error: {}", e);
        std::process::exit(1);
    }
}

fn run_repl() -> RustylineResult<()> {
    let mut rl = DefaultEditor::new()?;
    let mut evaluator = Evaluator::new();

    // Try to load history
    let history_path = dirs::home_dir()
        .map(|h| h.join(".jank_history"))
        .unwrap_or_else(|| ".jank_history".into());

    let _ = rl.load_history(&history_path);

    loop {
        let readline = rl.readline("jank> ");

        match readline {
            Ok(line) => {
                let line = line.trim();

                if line.is_empty() {
                    continue;
                }

                // Add to history
                let _ = rl.add_history_entry(line);

                // Check for REPL commands
                if line.starts_with(':') {
                    match line {
                        ":quit" | ":q" | ":exit" => {
                            println!("Goodbye!");
                            break;
                        }
                        ":help" | ":h" => {
                            print_help();
                            continue;
                        }
                        ":version" | ":v" => {
                            println!("jank-rs v{}", VERSION);
                            continue;
                        }
                        ":clear" => {
                            // Clear screen
                            print!("\x1B[2J\x1B[1;1H");
                            io::stdout().flush().unwrap();
                            continue;
                        }
                        _ => {
                            // Might be a keyword, try to eval
                        }
                    }
                }

                // Try to read and evaluate
                match read_and_eval(&mut evaluator, line) {
                    Ok(results) => {
                        for result in results {
                            println!("{}", result);
                        }
                    }
                    Err(e) => {
                        eprintln!("Error: {}", e);
                    }
                }
            }
            Err(ReadlineError::Interrupted) => {
                println!("^C");
                continue;
            }
            Err(ReadlineError::Eof) => {
                println!("\nGoodbye!");
                break;
            }
            Err(err) => {
                eprintln!("Readline error: {:?}", err);
                break;
            }
        }
    }

    // Save history
    let _ = rl.save_history(&history_path);

    Ok(())
}

fn read_and_eval(evaluator: &mut Evaluator, input: &str) -> Result<Vec<String>, String> {
    // Read all forms
    let forms = read_all(input).map_err(|e| e.to_string())?;

    // Evaluate each form
    let mut results = Vec::new();
    for form in forms {
        let result = evaluator.eval(&form).map_err(|e| e.to_string())?;
        results.push(format_result(&result));
    }

    Ok(results)
}

fn format_result(value: &jank_rs::Value) -> String {
    value.to_string()
}

fn print_help() {
    println!(r#"
jank-rs REPL Commands:
  :help, :h     - Show this help message
  :quit, :q     - Exit the REPL
  :version, :v  - Show version
  :clear        - Clear the screen

Examples:
  (+ 1 2 3)                    ; => 6
  (def x 42)                   ; => x
  x                            ; => 42
  (defn square [x] (* x x))    ; => square
  (square 5)                   ; => 25
  (let [a 1 b 2] (+ a b))      ; => 3
  (if (> 5 3) "yes" "no")      ; => "yes"
  (loop [n 5 acc 1]            ; => 120
    (if (<= n 1)
      acc
      (recur (dec n) (* acc n))))

Data structures:
  [1 2 3]                      ; vector
  {{:a 1 :b 2}}                ; map
  #{{1 2 3}}                   ; set
  '(1 2 3)                     ; list

Core functions:
  +, -, *, /                   ; arithmetic
  =, <, >, <=, >=              ; comparison
  first, rest, cons, conj      ; sequences
  get, assoc, dissoc           ; collections
  map, filter, reduce          ; higher-order (use loop/recur for now)
  str, println                 ; I/O
"#);
}
