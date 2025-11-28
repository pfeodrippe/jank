#!/bin/bash
# Quick start script for jank WASM Hot-Reload Server
#
# Usage: ./start_hot_reload.sh
#
# This starts the server and opens the browser to the hot-reload page.

set -e

cd "$(dirname "$0")"

echo "=== jank WASM Hot-Reload ==="
echo ""

# Check if required node modules are installed
if ! node -e "require('ws')" 2>/dev/null; then
  echo "Installing dependencies..."
  npm install ws bencode
  echo ""
fi

# Check if WASM build exists
WASM_FILE="/Users/pfeodrippe/dev/jank/compiler+runtime/build-wasm/eita.wasm"
if [ ! -f "$WASM_FILE" ]; then
  echo "ERROR: WASM build not found at $WASM_FILE"
  echo ""
  echo "Build it with:"
  echo "  cd /Users/pfeodrippe/dev/jank/compiler+runtime"
  echo "  HOT_RELOAD=1 ./bin/emscripten-bundle wasm-examples/eita.jank"
  exit 1
fi

echo "Starting hot-reload server..."
echo ""

# Start server
node hot_reload_server.cjs &
SERVER_PID=$!

# Wait for server to start
sleep 2

# Open browser (macOS)
if command -v open &> /dev/null; then
  open "http://localhost:8080/eita_hot_reload.html"
fi

echo ""
echo "Press Ctrl+C to stop the server"
echo ""

# Wait for server to exit
wait $SERVER_PID
