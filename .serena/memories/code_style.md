# jank Code Style and Conventions

## Naming
- Classes/structs: `snake_case` (e.g., `local_binding`, `var_deref`)
- Functions: `snake_case` (e.g., `analyze_let`, `expression_type`)
- Constants: `snake_case` with descriptive names
- Namespaces: `jank::analyze`, `jank::runtime`, `jank::codegen`

## Types
- Use `jtl::option<T>` for optional values (similar to `std::optional`)
- Use `jtl::ref<T>` for reference-counted objects
- Use `jtl::ptr<T>` for raw pointers
- Use `native_vector<T>` instead of `std::vector`

## Patterns
- Visitor pattern via `visit_expr()` for expression traversal
- Expression position tracked (`value`, `statement`, `tail`)
- `needs_box` boolean for boxing/unboxing decisions

## Error Handling
- Use `result<T, E>` for fallible operations
- `.is_err()` / `.is_ok()` / `.expect_ok()` / `.expect_err()`

## Testing Best Practices
- **ALWAYS** use `./bin/test 2>&1 | tee .tests-new.txt | tail -N` to capture full output
- Never run tests twice - capture output once and grep from the saved file

## Format String Escaping
- **IMPORTANT**: `{{` or `}}` do NOT work with `util::format_to` to escape literal braces
- Use string concatenation or `push_back` to add literal braces instead
- **IMPORTANT**: `util::format_to` does NOT support format specifiers like `{:g}`, `{:d}`, etc. - only plain `{}` is supported

## Testing
- Doctest framework
- Tests in `test/cpp/jank/`
