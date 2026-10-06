{
  pkgs ? import <nixpkgs> { },
}:
# KomiraQuake Linux (Qt 6 + QML) 构建环境
#
# NixOS 上 Qt6 / CMake 不在默认 PATH，需在 dev shell 里构建：
#   nix-shell
#   cmake -B build -G Ninja
#   cmake --build build
#   ./build/komiraquake
#
# 仅验证纯 C++ 核心引擎（无需 Qt）：
#   ./run_core_tests.sh
pkgs.mkShell {
  buildInputs = with pkgs; [
    cmake
    ninja
    pkg-config

    qt6.qtbase
    qt6.qtdeclarative
    qt6.qtwebsockets
    qt6.qtsvg
    qt6.qtmultimedia
  ];

  shellHook = ''
    echo "KomiraQuake Linux 构建环境就绪 (Qt $(qmake6 -query QT_VERSION 2>/dev/null || echo '?') )"
    echo "  cmake -B build -G Ninja && cmake --build build"
    echo "  ./run_core_tests.sh   # 纯 C++ 引擎单测"
  '';
}
