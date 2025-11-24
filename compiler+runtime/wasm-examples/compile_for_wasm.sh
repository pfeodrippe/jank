#!/usr/bin/env bash
set -euo pipefail

# Script to compile jank code for WASM
#
# This script:
# 1. Uses jank to compile the my-ns module with C++ code generation
# 2. Saves the generated C++ code
# 3. Shows you what would be needed to compile it with emscripten

echo "=== Compiling hello.jank to generate C++ ==="

# First, let's try to compile the module itself and save C++
# The issue is that jank compile doesn't use cpp codegen for modules when building static binaries
# It only uses it for JIT/dynamic loading

# Let's examine what actually gets compiled
echo ""
echo "Current jank limitations for WASM compilation:"
echo "1. The 'compile' command with --codegen cpp doesn't actually save module C++ code"
echo "2. The C++ codegen path is only used during JIT compilation, not AOT"
echo "3. For WASM, we would need to:"
echo "   a) Extract the generated C++ code from modules"
echo "   b) Compile them with em++ along with the jank runtime"
echo ""

echo "What we CAN do now:"
echo "1. Build the WASM runtime: ./bin/emscripten-bundle"
echo "2. Link the runtime with hand-written C++ that calls jank functions"
echo ""

echo "The current hello.jank file would need jank's eval/compile functionality"
echo "to work in WASM, which requires the full compiler (not yet ported to WASM)"
echo ""

echo "For now, the best approach is to:"
echo "1. Use the existing jank_demo.cpp approach (call into minimal runtime)"
echo "2. Or write C++ wrappers that call pre-compiled jank functions"
echo ""

echo "To see the structure of what jank WOULD generate, run:"
echo "  JANK_SAVE_CPP=1 ../build/jank run --codegen cpp hello.jank"
echo "  (This will fail but might show code generation in progress)"
