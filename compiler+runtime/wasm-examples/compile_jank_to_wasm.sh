#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: ./compile_jank_to_wasm.sh <file.jank> [em++ args...]

Generates a self-contained C++ translation unit from the given .jank file using
`../build/jank --codegen cpp run`, links it against the minimal runtime stub,
and emits <file>.{js,wasm} via em++.

Additional arguments are forwarded to em++.
USAGE
}

if [[ $# -lt 1 ]]; then
  usage >&2
  exit 1
fi

script_dir="$(cd "$(dirname "$0")" && pwd)"
jank_src="$1"
shift || true

if [[ ! -f "$script_dir/$jank_src" ]]; then
  echo "error: $jank_src does not exist relative to $script_dir" >&2
  exit 1
fi

base_name="$(basename "$jank_src" .jank)"
generated_cpp="$script_dir/build/${base_name}_wasm.cpp"

"$script_dir/gen_wasm_cpp.py" "$jank_src" --output "$generated_cpp"

echo "[jank→wasm] Generated $generated_cpp"

em++ -O2 "$generated_cpp" -o "$script_dir/${base_name}.js" \
  -s EXPORTED_FUNCTIONS='["_main","_jank_run_main"]' \
  -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap"]' \
  "$@"

echo "[jank→wasm] Built ${base_name}.js and ${base_name}.wasm"
