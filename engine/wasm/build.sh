#!/usr/bin/env bash
# Build the owned Wake engine as scalar or SIMD WebAssembly.
# Native and WASM builds share engine/sources.txt, generated language tables
# and strict floating-point flags. No SDK implements its own scorer.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
engine="$root/engine"
out="$root/sdk/web/dist"
simd=0
profile=scalar

for arg in "$@"; do
  case "$arg" in
    --simd) simd=1; profile=simd ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

: "${EMSDK:=$HOME/tools/emsdk}"
if ! command -v em++ >/dev/null 2>&1; then
  # shellcheck disable=SC1091
  source "$EMSDK/emsdk_env.sh" >/dev/null 2>&1
fi
command -v em++ >/dev/null 2>&1 || { echo "em++ not found; set EMSDK" >&2; exit 1; }

work="$here/build/$profile"
mkdir -p "$work" "$out"

# Same generated language table as the native build.
python3 - "$engine" "$work/languages.generated.h" <<'PY'
import runpy, sys
from pathlib import Path
engine, target = Path(sys.argv[1]), Path(sys.argv[2])
runpy.run_path(str(engine / "tools/generate_languages.py"))["generate"](
    engine / "inventories", target
)
PY

mapfile -t sources < <(grep -v '^[[:space:]]*$' "$engine/sources.txt")

# -ffp-contract=off and -fno-fast-math are what make the float32 FFT in
# frontend.cc reproduce the native mel bit for bit. Do not drop them.
flags=(
  -O3 -std=c++17
  -ffp-contract=off -fno-fast-math
  -fwasm-exceptions
  -I"$engine/include" -I"$root/include" -I"$engine/include/kws/detail" -I"$engine/src/session" -I"$work"
)
[[ $simd -eq 1 ]] && flags+=(-msimd128)

exports=$(python3 -c "
import json,sys
names=json.load(open('$here/exports.json'))
print(json.dumps(['_'+n for n in names]))
")

link=(
  -sMODULARIZE=1
  -sEXPORT_ES6=1
  -sEXPORT_NAME=createKwsModule
  -sENVIRONMENT=web,worker,node
  -sALLOW_MEMORY_GROWTH=1
  -sINITIAL_MEMORY=16MB
  -sSTACK_SIZE=1MB
  -sFORCE_FILESYSTEM=1
  -sEXPORTED_FUNCTIONS="$exports"
  -sEXPORTED_RUNTIME_METHODS='["FS","ENV","HEAP8","HEAPU8","HEAP16","HEAPU16","HEAP32","HEAPF64","UTF8ToString","stringToUTF8","lengthBytesUTF8"]'
  -sINCOMING_MODULE_JS_API='["locateFile","wasmBinary","print","printErr","preRun"]'
)

name=kws_engine
[[ $simd -eq 1 ]] && name=kws_engine.simd
echo "building $profile -> $out/$name.mjs"
EMCC_CORES=2 em++ "${flags[@]}" "${sources[@]/#/$engine/}" "${link[@]}" \
  -o "$out/$name.mjs"

size=$(stat -c%s "$out/$name.wasm")
echo "$name.wasm: $((size / 1024)) KB"
