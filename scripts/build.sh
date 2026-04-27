#!/usr/bin/env bash
# Build the DSWP pass plugin.
set -euo pipefail

PROJ_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Locate LLVM 15. Prefer $LLVM_PREFIX, then Homebrew (macOS), then
# anything `llvm-config` in $PATH points at (Linux / cluster).
if [[ -z "${LLVM_PREFIX:-}" ]]; then
  if command -v brew >/dev/null 2>&1 && brew --prefix llvm@15 >/dev/null 2>&1; then
    LLVM_PREFIX="$(brew --prefix llvm@15)"
  elif command -v llvm-config >/dev/null 2>&1; then
    LLVM_PREFIX="$(llvm-config --prefix)"
  else
    echo "Cannot find LLVM 15. Set LLVM_PREFIX to its install dir." >&2
    exit 1
  fi
fi

cd "$PROJ_ROOT"

CMAKE_FLAGS=(
  -B build
  -S .
  -DCMAKE_BUILD_TYPE=Debug
  -DCMAKE_C_COMPILER="$LLVM_PREFIX/bin/clang"
  -DCMAKE_CXX_COMPILER="$LLVM_PREFIX/bin/clang++"
  -DLLVM_DIR="$LLVM_PREFIX/lib/cmake/llvm"
)

if command -v ninja >/dev/null 2>&1; then
  CMAKE_FLAGS+=(-G Ninja)
fi

cmake "${CMAKE_FLAGS[@]}"
cmake --build build --parallel

PLUGIN="$PROJ_ROOT/build/pass/DSWPAnalyzer"
if [[ -f "$PLUGIN.dylib" ]]; then
  echo "Built: $PLUGIN.dylib"
elif [[ -f "$PLUGIN.so" ]]; then
  echo "Built: $PLUGIN.so"
else
  echo "Plugin not found at $PLUGIN.{dylib,so}"
  ls -la "$PROJ_ROOT/build/pass/"
  exit 1
fi
