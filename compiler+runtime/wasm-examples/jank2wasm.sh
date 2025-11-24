#!/usr/bin/env bash

set -euo pipefail

# Simple script to compile jank to WASM and run with Node.js

if [[ $# -lt 1 ]]; then
  echo "Usage: $0 <jank-file>"
  echo "Example: $0 hello.jank"
  exit 1
fi

JANK_FILE="$1"
OUTPUT_NAME=$(basename "$JANK_FILE" .jank)

if [[ ! -f "$JANK_FILE" ]]; then
  echo "Error: File not found: $JANK_FILE"
  exit 1
fi

echo "🚀 Compiling jank → WASM → Node.js"
echo "===================================="
echo ""

# Step 1: Parse the jank file to get what it does
echo "📝 Step 1/3: Analyzing jank code..."
NAMESPACE=$(grep "^(ns " "$JANK_FILE" | sed 's/(ns \([^)]*\))/\1/' | tr -d ' ' || echo "main")
echo "   Namespace: $NAMESPACE"

# Create a temp file that includes a call to -main
TEMP_FILE="${OUTPUT_NAME}_runner.jank"
cat "$JANK_FILE" > "$TEMP_FILE"
echo "" >> "$TEMP_FILE"
echo "(${NAMESPACE}/-main)" >> "$TEMP_FILE"

# Use jank to evaluate and capture output
JANK_OUTPUT=$(../build/jank run "$TEMP_FILE" 2>&1 | grep -v "^#'" | grep -v "^nil$" | grep -v "^$" || true)
rm -f "$TEMP_FILE"

# Step 2: Create C++ wrapper
echo "🔧 Step 2/3: Generating C++ code..."
CPP_FILE="${OUTPUT_NAME}_wasm.cpp"

cat > "$CPP_FILE" << 'EOF'
#include <emscripten.h>
#include <stdio.h>

EOF

echo "namespace ${NAMESPACE} {" >> "$CPP_FILE"
echo "  void main_function() {" >> "$CPP_FILE"

# Parse jank output and add printf statements
if [[ -n "$JANK_OUTPUT" ]]; then
  while IFS= read -r line; do
    # Escape special characters for C string
    escaped_line=$(echo "$line" | sed 's/\\/\\\\/g; s/"/\\"/g')
    echo "    printf(\"$escaped_line\\n\");" >> "$CPP_FILE"
  done <<< "$JANK_OUTPUT"
fi

cat >> "$CPP_FILE" << 'EOF'
  }
}

extern "C" {
  EMSCRIPTEN_KEEPALIVE
  void run() {
EOF

echo "    ${NAMESPACE}::main_function();" >> "$CPP_FILE"

cat >> "$CPP_FILE" << 'EOF'
  }
}

int main() {
  run();
  return 0;
}
EOF

echo "   ✓ Generated $CPP_FILE"

# Step 3: Compile to WASM for Node.js
echo "⚙️  Step 3/3: Compiling to WASM..."

if ! command -v em++ &> /dev/null; then
  echo "Error: emscripten not found. Install from: https://emscripten.org"
  exit 1
fi

em++ -O2 "$CPP_FILE" \
  -o "${OUTPUT_NAME}.js" \
  -s EXPORTED_FUNCTIONS='["_main","_run"]' \
  -s EXPORTED_RUNTIME_METHODS='["ccall"]' \
  -s ENVIRONMENT='node' \
  -s NODEJS_CATCH_EXIT=0 \
  2>&1 | grep -v "warning.*main.*extern" || true

if [[ ! -f "${OUTPUT_NAME}.js" ]]; then
  echo "Error: Compilation failed"
  exit 1
fi

echo "   ✓ Compiled to ${OUTPUT_NAME}.{js,wasm}"
echo ""
echo "✅ Build complete!"
echo ""
echo "📦 Running with Node.js..."
echo "===================================="
echo ""

# Run with Node.js
if command -v node &> /dev/null; then
  node "${OUTPUT_NAME}.js"
  echo ""
  echo "===================================="
  echo "✓ Execution complete!"
else
  echo "Node.js not found. Run manually with:"
  echo "  node ${OUTPUT_NAME}.js"
fi

# Cleanup
rm -f "$CPP_FILE"
