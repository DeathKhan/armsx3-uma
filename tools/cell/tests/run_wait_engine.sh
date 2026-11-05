#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
out="$repo/target/cell-tests"
mkdir -p "$out"
args=(-std=c++20 -O2 -g -pthread -DANDROID -I"$repo" -I"$repo/rpcs3")
case "${1:-}" in
  --android)
    compiler=${CXX:-$HOME/Android/sdk/ndk/29.0.14206865/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android35-clang++}
    output="$out/test-wait-engine-android-arm64"
    ;;
  "")
    compiler=${CXX:-clang++}
    output="$out/test-wait-engine"
    if [[ $(uname -m) == x86_64 ]]; then args+=(-mcx16); fi
    ;;
  *) printf 'Usage: %s [--android]\n' "$0" >&2; exit 2 ;;
esac
"$compiler" "${args[@]}" -include "$repo/tools/cell/tests/atomic_host_shim.h" \
  "$repo/tools/cell/tests/test_wait_engine.cpp" -o "$output"
if [[ ${1:-} == --android ]]; then
  printf 'Built %s (not executed)\n' "$output"
else
  ARMSX3_CELL=0 "$output"
  ARMSX3_CELL=1 "$output"
fi
