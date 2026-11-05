#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p target/cell-tests
c++ -std=c++20 -Wall -Wextra -Werror -I. tools/cell/tests/test_topology.cpp -o target/cell-tests/topology
target/cell-tests/topology
c++ -std=c++20 -Wall -Wextra -Werror -I. tools/cell/tests/test_master.cpp -o target/cell-tests/master
env -u ARMSX3_CELL target/cell-tests/master
ARMSX3_CELL=0 target/cell-tests/master
ARMSX3_CELL=true target/cell-tests/master
ARMSX3_CELL=1 target/cell-tests/master expected
c++ -std=c++20 -Wall -Wextra -Werror -Wno-unused-parameter -DANDROID -I. tools/cell/tests/test_master.cpp -o target/cell-tests/master-android
env -u ARMSX3_CELL target/cell-tests/master-android
printf 'Master switch: absent, 0, true, 1, pre-driver-env Android initialization passed\n'
