#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
out="$repo/target/cell-tests"
mkdir -p "$out"
"${CXX:-c++}" -std=c++20 -O2 -g -Wall -Wextra -Werror -fno-omit-frame-pointer \
  -I"$repo" \
  "$repo/Utilities/CellFiber.cpp" \
  "$repo/Utilities/CellScheduler.cpp" \
  "$repo/tools/cell/tests/test_scheduler.cpp" \
  -pthread -o "$out/test-scheduler"
"$out/test-scheduler"
