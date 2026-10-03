#pragma once

// 由 CMake 通过 target_compile_definitions 注入（project() VERSION 的唯一来源）。
// 独立编译单个文件时回退到占位值，保证仍可编译。
#ifndef KOMIRA_VERSION
#define KOMIRA_VERSION "0.0.0"
#endif
