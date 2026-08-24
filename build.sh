#!/bin/bash
# Build the C++ Game Boy Color core to WebAssembly (output: web/gbc.js + web/gbc.wasm).
set -euo pipefail
cd "$(dirname "$0")"

emcc -O3 -std=c++17 \
  core/cpu.cpp core/ppu.cpp core/apu.cpp core/cartridge.cpp core/gb.cpp core/chromatic.cpp core/ymfm/ymfm_opm.cpp \
  -o web/gbc.js \
  -sMODULARIZE=1 \
  -sEXPORT_NAME=createGbModule \
  -sALLOW_MEMORY_GROWTH=1 \
  -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU32,HEAPF32 \
  -sENVIRONMENT=web \
  --no-entry

# A backup suffix works with both BSD and GNU sed; remove it after stamping.
VER=$(date +%s)
sed -i.bak -E "s/(\\?v=|GB_VER=')[0-9a-zA-Z]+/\\1${VER}/g" web/index.html
rm -f web/index.html.bak

echo "Build OK: web/gbc.js web/gbc.wasm (v=${VER})"
