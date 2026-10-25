#pragma once

#include <string>

namespace komira {

enum class IntensityStandard { Csis, Jma };

/// 《NATIVE_PORT_SPEC》 §4.3 烈度。
class IntensityCalculator {
public:
    static double rawCsis(double magnitude, double distanceKm, double depthKm = 10.0);
    /// 实测 PGA（Gal，cm/s²）→ 仪器烈度（GB/T 17742-2020，按设备口径保留 1 位小数）：
    /// `PGA <= 0.01` 为 0；`0.01~0.22` 线性映射 `PGA/0.22`；`>= 0.22` 用
    /// `1.0 + 1.66·log10(PGA/0.22)`，上限 12。
    /// **测站显示不要用它**：PancakesSeis 的实时帧已由设备算好 `i`，应直接用设备值；
    /// 本函数只用于缺 `i` 时的兜底与换算校验。
    static double csisFromPga(double pgaGal);
    /// 波前"影响半径"：CSIS 烈度衰减到 level（默认 I，可感下限）时的最大震中距（km）。
    /// 有意取代参考实现 kanameishi 的经验式 clamp(50·M², 200, 2000)：这里用衰减关系按
    /// 震级 + 深度反解"可感边界"，比纯震级的平方经验式更贴合物理。
    static double distanceForCsis(double magnitude, double depthKm, double level = 1.0);
    /// 波前描边的**按半径**不透明度：前 95% 不透明，最后 5% 快速淡到 0。
    /// 三种消失口径（烈度 / 距离 / 时间）共用 `core/wave_fade.h` 的曲线，这里只是
    /// 「距离口径」那个特例的转发；按时间求透明度请直接用 `WaveFade`。
    static double waveOpacity(double radiusKm, double fadeKm, double hardMaxKm = 10000.0);
    /// S 波径向渐变填充的不透明度：与描边同一条曲线，峰值 0.25。
    static double waveFillOpacity(double radiusKm, double fadeKm);
    static std::string formatCsis(double raw);
    /// 显示烈度（罗马数字的阿拉伯数字形式）：formatCsis 取整所用的同一个值，下限 0。
    /// 「本地烈度过滤」按它比较而非按raw 原始值，否则 HUD 显示Ⅲ度而阈值 3.0 却判定为未达到。
    static int displayLevel(double raw);
    /// JMA 震度的连续值 s（未分档）。formatJma 的分档与 jmaLevel 均由它派生。
    static double jmaValue(double magnitude, double distanceKm, double depthKm);
    /// JMA 显示震度（0–7 的整数，5弱/5强 同为 5、6弱/6强 同为 6）。
    /// 与 formatJma 用同一组分档边界，故「显示值」与「过滤比较值」必然一致。
    static int jmaLevel(double magnitude, double distanceKm, double depthKm);
    /// 当前烈度制式下的显示级数：CSIS 取 rawCsis 的取整档，JMA 取震度分档。
    /// 「本地烈度过滤」按它与阈值比较，使过滤语义始终跟随设置页选择的显示标准。
    static double displayedLevel(double magnitude, double rawCsis, double distanceKm, double depthKm,
                                 IntensityStandard standard);
    static std::string formatJma(double magnitude, double distanceKm, double depthKm = 10.0);
    static std::string description(double raw);
};

} // namespace komira
