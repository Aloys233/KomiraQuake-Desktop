#pragma once

#include <cmath>
#include <cstdint>

namespace komira {

/// 中国地震烈度色阶（对齐 kanameishi CSIS 配色）。
///
/// 刻意不依赖 Qt：`src/core/` 的纯算法模块要能在 run_core_tests.sh 的 g++ 直编路径下
/// 编译（矢量地图的区县着色就在 core 里），故色阶以 RGB 三元组表达，
/// `theme/seismic_colors.h` 只是它的 QColor 薄包装。
namespace intensity_palette {

struct Rgb {
    int r = 0;
    int g = 0;
    int b = 0;
};

/// 按 CSIS 原始值取色。分档规则与既有实现一致：round(raw) 后落入 1..9，>=10 为紫。
inline Rgb intensityColor(double raw) {
    const int level = static_cast<int>(std::lround(raw));
    switch (level) {
    case 1: return {0x9F, 0x9F, 0x9F}; // 灰
    case 2: return {0xCF, 0xCF, 0xCF}; // 浅灰
    case 3: return {0x5F, 0xCF, 0xFF}; // 天蓝
    case 4: return {0x3F, 0xAF, 0xFF}; // 蓝
    case 5: return {0x5F, 0xDF, 0x8F}; // 绿
    case 6: return {0xF7, 0xE7, 0x57}; // 黄
    case 7: return {0xFF, 0x8F, 0x00}; // 橙
    case 8: return {0xFF, 0x4F, 0x00}; // 橙红
    case 9: return {0xDF, 0x0F, 0x0F}; // 红
    default:
        if (level >= 10) return {0x7F, 0x00, 0x7F}; // 紫
        return {0x9F, 0x9F, 0x9F};
    }
}

/// 打包成 0xRRGGBB，便于写入顶点色。
inline uint32_t intensityColorRgbHex(double raw) {
    const Rgb c = intensityColor(raw);
    return (static_cast<uint32_t>(c.r) << 16) | (static_cast<uint32_t>(c.g) << 8) |
           static_cast<uint32_t>(c.b);
}

/// 震级配色。
inline Rgb magnitudeColor(double magnitude) {
    if (magnitude < 3.0) return {0x00, 0x79, 0x6B};
    if (magnitude < 4.5) return {0xF5, 0x7F, 0x17};
    if (magnitude < 6.0) return {0xE6, 0x4A, 0x19};
    return {0xC2, 0x18, 0x5B};
}

} // namespace intensity_palette

} // namespace komira