#!/usr/bin/env bash
set -euo pipefail
test_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
test_build="$(mktemp -d)"
trap 'rm -rf -- "$test_build"' EXIT
"${CXX:-c++}" -std=c++20 -O1 -g -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$test_dir/protocol_tests.cpp" "$test_dir/../../Emu/Io/ThrustmasterT500RS.cpp" \
  -o "$test_build/protocol_tests"
"$test_build/protocol_tests"
