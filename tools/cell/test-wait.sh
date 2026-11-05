#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p target/cell-tests
c++ -std=c++20 -Wall -Wextra -Werror -pthread -I. tools/cell/tests/test_wait.cpp -o target/cell-tests/wait
target/cell-tests/wait
c++ -std=c++20 -Wall -Wextra -Werror -I. tools/cell/tests/test_wait_policy.cpp -o target/cell-tests/wait-policy
ARMSX3_CELL=1 target/cell-tests/wait-policy
