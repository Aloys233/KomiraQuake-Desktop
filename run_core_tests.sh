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
  src/core/geo_geometry.cpp \
  src/core/vector_map_data.cpp \
  src/core/travel_table.cpp \
  src/core/event_gate.cpp \
  src/core/time_sync.cpp \
  src/core/source_estimator.cpp \
  src/core/region_names.cpp \
  -o "$OUT"
# .kvb 矢量数据：可选，存在才校验（core 层不依赖 Qt，故路径由脚本传入而非运行时推导）。
# 注意 set -e：不能用 `[ -f x ] && ...` 形式，测试为假时会让整个脚本提前退出。
KVB=""
if [ -f "assets/map_bounds.kvb" ]; then
  KVB="$PWD/assets/map_bounds.kvb"
fi
"$OUT" $KVB
