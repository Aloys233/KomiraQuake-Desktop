#!/usr/bin/env bash
# 纯 C++ 核心引擎单测（无需 Qt / CMake）。
set -euo pipefail
cd "$(dirname "$0")"
OUT="$(mktemp -d)/core_engine_test"
g++ -std=c++17 -Wall -Wextra -I src \
  tests/core_engine_test.cpp \
  src/core/quake_calculator.cpp \
  src/core/intensity_calculator.cpp \
  src/core/coordinate_transform.cpp \
  src/core/travel_table.cpp \
  src/core/event_gate.cpp \
  src/core/time_sync.cpp \
  -o "$OUT"
"$OUT"
