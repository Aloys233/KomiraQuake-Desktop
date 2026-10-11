#pragma once

#include <QColor>
#include <cmath>

#include "core/intensity_palette.h"
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
    /// 色阶本体在 core/intensity_palette.h（不依赖 Qt），此处只做 QColor 包装。
    static QColor intensityColor(double raw) {
        const auto c = intensity_palette::intensityColor(raw);
        return QColor(c.r, c.g, c.b);
    }

    static QColor magnitudeColor(double magnitude) {
        const auto c = intensity_palette::magnitudeColor(magnitude);
        return QColor(c.r, c.g, c.b);
    }

    static QColor on(const QColor& bg) {
        return bg.lightnessF() < 0.5 ? QColor(0xFF, 0xFF, 0xFF) : QColor(0x1A, 0x1C, 0x1E);
    }
};

} // namespace komira
