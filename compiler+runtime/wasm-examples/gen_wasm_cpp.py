#!/usr/bin/env python3
"""Generate a self-contained C++ file from a .jank source that can be built with emscripten."""

import argparse
import os
import pathlib
import re
import subprocess
from textwrap import dedent


def extract_cpp(source: str) -> tuple[str, str, str]:
    """Return (sanitized_cpp, namespace, struct_name).
    
    The struct_name returned is the LAST jit_function struct found,
    as that's typically the entry point (main function).
    """
    # Remove everything after the C++ code ends
    # The C++ code ends after the last closing brace of a namespace
    lines = source.split('\n')
    cpp_lines = []
    for line in lines:
        # Stop at lines that don't look like C++ (e.g., "Hello from WASM!", "nil")
        # C++ code typically starts with whitespace, keywords, braces, etc.
        if cpp_lines and not line.strip().startswith(('namespace', 'struct', '{', '}', 'using', 
                                                       'auto', 'return', 'jank::', 'object_ref',
                                                       '//', '/*', '*', '#', 'explicit', 'virtual',
                                                       'final', 'override', '.', ',', ')', '(', 
                                                       ';', 'if', 'else', 'for', 'while')):
            if line and not line[0].isspace() and line.strip() != '}':
                break
        cpp_lines.append(line)
    
    source = '\n'.join(cpp_lines).strip()
    
    if not source:
        raise RuntimeError("jank did not emit any C++ code")

    ns_match = re.search(r"namespace\s+([A-Za-z0-9_]+)\s*\{", source)
    if not ns_match:
        raise RuntimeError("could not find namespace in generated C++")
    
    # Find ALL jit_function structs and use the LAST one (entry point)
    struct_matches = list(re.finditer(r"struct\s+([A-Za-z0-9_]+)\s*:\s*jank::runtime::obj::jit_function", source))
    if not struct_matches:
        raise RuntimeError("could not find jit_function struct in generated C++")

    namespace = ns_match.group(1)
    struct_name = struct_matches[-1].group(1)  # Use the LAST struct (entry point)
    return source, namespace, struct_name


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("jank_file", help="Path to the .jank source file")
    parser.add_argument("--output", required=True, help="Target C++ file to write")
    args = parser.parse_args()

    script_dir = pathlib.Path(__file__).resolve().parent
    repo_build = script_dir.parent / "build"
    jank_binary = repo_build / "jank"
    if not jank_binary.exists():
        raise RuntimeError(f"{jank_binary} not found; build the compiler first (./bin/compile)")

    jank_file = pathlib.Path(args.jank_file)
    if not jank_file.exists():
        raise RuntimeError(f"{jank_file} does not exist")

    proc = subprocess.run(
        [str(jank_binary), "--codegen", "cpp", "run", str(jank_file.name)],
        cwd=script_dir,
        check=True,
        capture_output=True,
        text=True,
    )
    cpp_source, namespace, struct_name = extract_cpp(proc.stdout or proc.stderr)

    output_path = pathlib.Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    header_path = pathlib.Path("minimal_jank_runtime.hpp")
    header_include = header_path
    if not header_path.is_absolute():
        header_include = pathlib.Path(
            os.path.relpath(header_path, output_path.parent)
        )

    driver = dedent(
        f"""
        #include \"{header_include.as_posix()}\"
        #ifdef __EMSCRIPTEN__
        #  include <emscripten/emscripten.h>
        #endif

        {cpp_source}

        namespace {{
          using jank_entry_t = ::{namespace}::{struct_name};
        }}

        extern \"C\" {{

        #ifdef __EMSCRIPTEN__
        EMSCRIPTEN_KEEPALIVE
        #endif
        void jank_run_main() {{
          auto fn = jank::runtime::make_box<jank_entry_t>();
          fn->call();
        }}

        int main() {{
          jank_run_main();
          return 0;
        }}

        }}
        """
    ).strip() + "\n"

    output_path.write_text(driver, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
