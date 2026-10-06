#pragma once

#include <string>

namespace komira {

enum class IntensityStandard { Csis, Jma };

/// 《NATIVE_PORT_SPEC》 §4.3 烈度。
class IntensityCalculator {
public:
    static double rawCsis(double magnitude, double distanceKm, double depthKm = 10.0);
    /// 波前"影响半径"：CSIS 烈度衰减到 level（默认 I，可感下限）时的最大震中距（km）。
    /// 有意取代参考实现 kanameishi 的经验式 clamp(50·M², 200, 2000)：这里用衰减关系按
    /// 震级 + 深度反解"可感边界"，比纯震级的平方经验式更贴合物理。
    static double distanceForCsis(double magnitude, double depthKm, double level = 1.0);
    /// 波前描边不透明度：分段线性（形状借用 kanameishi 的 calcOpacity）——fadeKm 内由 1 渐隐到
    /// 0.25，其后由 0.25 渐隐到 0，超过 hardMaxKm 完全隐藏。
    static double waveOpacity(double radiusKm, double fadeKm, double hardMaxKm = 10000.0);
    /// S 波径向渐变填充的不透明度：影响半径 fadeKm 内由 0.25 递减到 0（对齐 kanameishi 的
    /// sWaveFill），超过 fadeKm 不再填充。需与中心透明、边缘着色的径向渐变叠加使用。
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
