#pragma once

#include <QColor>
#include <cmath>

#include "model/warning_level.h"

namespace komira {

/// 语义配色令牌。《NATIVE_PORT_SPEC》 §9。
struct SeismicColors {
    static QColor severity(WarningLevel level, bool dark) {
        switch (level) {
        case WarningLevel::Normal: return dark ? QColor(0x4D, 0xDA, 0xD7) : QColor(0x00, 0x68, 0x74);
        case WarningLevel::Watch: return dark ? QColor(0xFF, 0xBA, 0x28) : QColor(0x7A, 0x59, 0x00);
        case WarningLevel::Warning: return dark ? QColor(0xFF, 0x8C, 0x66) : QColor(0xBC, 0x28, 0x00);
        case WarningLevel::Critical: return dark ? QColor(0xFF, 0xB4, 0xAB) : QColor(0xBA, 0x1A, 0x1A);
        }
        return QColor(0x00, 0x68, 0x74);
    }

    static QColor pWave() { return QColor(0x02, 0x88, 0xD1); }
    static QColor sWave() { return QColor(0xE6, 0x51, 0x00); }

    /// 中国地震烈度色阶（对齐 kanameishi CSIS 配色）。
    static QColor intensityColor(double raw) {
        const int level = static_cast<int>(std::lround(raw));
        switch (level) {
        case 1: return QColor(0x9F, 0x9F, 0x9F); // 灰
        case 2: return QColor(0xCF, 0xCF, 0xCF); // 浅灰
        case 3: return QColor(0x5F, 0xCF, 0xFF); // 天蓝
        case 4: return QColor(0x3F, 0xAF, 0xFF); // 蓝
        case 5: return QColor(0x5F, 0xDF, 0x8F); // 绿
        case 6: return QColor(0xF7, 0xE7, 0x57); // 黄
        case 7: return QColor(0xFF, 0x8F, 0x00); // 橙
        case 8: return QColor(0xFF, 0x4F, 0x00); // 橙红
        case 9: return QColor(0xDF, 0x0F, 0x0F); // 红
        default:
            if (level >= 10) return QColor(0x7F, 0x00, 0x7F); // 紫
            return QColor(0x9F, 0x9F, 0x9F);
        }
    }

    static QColor magnitudeColor(double magnitude) {
        if (magnitude < 3.0) return QColor(0x00, 0x79, 0x6B);
        if (magnitude < 4.5) return QColor(0xF5, 0x7F, 0x17);
        if (magnitude < 6.0) return QColor(0xE6, 0x4A, 0x19);
        return QColor(0xC2, 0x18, 0x5B);
    }

    static QColor on(const QColor& bg) {
        return bg.lightnessF() < 0.5 ? QColor(0xFF, 0xFF, 0xFF) : QColor(0x1A, 0x1C, 0x1E);
    }
};

} // namespace komira
