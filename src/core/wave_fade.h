#pragma once

#include <algorithm>

namespace komira {

/// P/S 波前「什么时候消失」的口径。《NATIVE_PORT_SPEC》 §12。
///
/// 三种口径共用同一条渐隐曲线（前 95% 完全不透明，最后 5% 线性淡到 0），差别只在于
/// **走到哪算消失**：
/// - `Intensity`（默认）：波前半径到达「CSIS 烈度衰减到 level」的震中距时消失。
///   这是物理含义最直白的一种 —— 圆扫到哪里，哪里就刚好处在可感下限。
/// - `Distance`：半径到达用户指定的公里数时消失。与震级无关，纯几何边界。
/// - `Time`：发震后经过用户指定的秒数时消失。与半径无关，P 与 S 同时隐去。
///
/// 之所以把「消失边界」抽象成 **progress ∈ [0,1]** 而不是各自算一套透明度：
/// 三条曲线若各写一遍，淡出的手感（在何处开始变淡、剩多少时归零）就会分叉，
/// 而用户切换口径时看到的应当是同一件事 —— 只是边界不同。
///
/// 本头文件**不依赖** `IntensityCalculator`：烈度口径的边界半径由调用方算好传进来
/// （见 `AppController::updateWaveRadii`），这样非烈度口径也就不用白跑一次
/// 40 次迭代的衰减反解。
enum class WaveFadeMode { Intensity, Distance, Time };

struct WaveFadeSettings {
    WaveFadeMode mode = WaveFadeMode::Intensity;
    /// Intensity 口径的烈度阈值（CSIS 刻度）。默认 1.0 = 可感下限 I。
    double intensityLevel = 1.0;
    /// Distance 口径的消失半径（km）。
    double distanceKm = 1000.0;
    /// Time 口径的消失时长（秒）。
    double seconds = 300.0;

    bool operator==(const WaveFadeSettings& other) const {
        return mode == other.mode && intensityLevel == other.intensityLevel
               && distanceKm == other.distanceKm && seconds == other.seconds;
    }
    bool operator!=(const WaveFadeSettings& other) const { return !(*this == other); }
};

class WaveFade {
public:
    /// 前 95% 保持不透明，只在最后 5% 淡出（与历史行为一致，切换口径后手感相同）。
    static constexpr double kFadeStartRatio = 0.95;
    /// 走时表的量程上限：任何口径都不可能让波前画到这个半径之外。
    static constexpr double kMaxRadiusKm = 10000.0;
    /// Time 口径允许的最长时长：与「波前只在发震 60 min 内计算」同一条窗口。
    static constexpr double kMaxSeconds = 3600.0;
    /// S 波径向渐变填充的峰值不透明度（与历史行为一致）。
    static constexpr double kFillPeakOpacity = 0.25;

    /// 把口径映射成「消失半径」（km）。
    ///
    /// `intensityRadiusKm` 只在 `Intensity` 口径下被使用（调用方按当前震级/深度算好）：
    /// **负数表示「算不出来」**（震级未知），此时退回量程上限，保证波前仍可见；
    /// **0 表示「震中处的烈度就已经低于阈值」**，此时边界为 0、波前直接不画 ——
    /// 用户把阈值调到「强震才显示」时，远处的小震本来就不该画出圆。
    ///
    /// `Distance` 直接取用户值；`Time` 不用距离边界，返回量程上限，调用方应改看
    /// `progressForTime`。
    static double fadeEndKm(const WaveFadeSettings& settings, double intensityRadiusKm) {
        switch (settings.mode) {
        case WaveFadeMode::Intensity:
            if (intensityRadiusKm < 0.0) return kMaxRadiusKm;
            return std::min(intensityRadiusKm, kMaxRadiusKm);
        case WaveFadeMode::Distance:
            return std::clamp(settings.distanceKm, 1.0, kMaxRadiusKm);
        case WaveFadeMode::Time:
            break;
        }
        return kMaxRadiusKm;
    }

    /// Time 口径的消失时长（秒），夹到 (0, 3600]。
    static double fadeSeconds(const WaveFadeSettings& settings) {
        if (!(settings.seconds > 0.0)) return kMaxSeconds;
        return std::min(settings.seconds, kMaxSeconds);
    }

    /// 由半径得到「走到消失边界的比例」。非正半径视为尚未出发（0）；
    /// 边界非正（阈值高于近场烈度，见 `fadeEndKm`）时一律视为已越过（1），
    /// 宁可不画也不要画一个永远不会消失的圆。
    static double progressForRadius(double radiusKm, double fadeKm) {
        const double end = std::min(fadeKm, kMaxRadiusKm);
        if (!(end > 0.0)) return 1.0;
        if (!(radiusKm > 0.0)) return 0.0;
        return radiusKm / end;
    }

    /// 由经过时间得到「走到消失边界的比例」。
    static double progressForTime(double elapsedSeconds, double fadeSeconds) {
        if (!(elapsedSeconds > 0.0)) return 0.0;
        if (!(fadeSeconds > 0.0)) return 1.0;
        return elapsedSeconds / fadeSeconds;
    }

    /// 描边不透明度：前 95% 恒为 1，最后 5% 线性归零；到达/越过边界即 0。
    static double strokeOpacity(double progress) {
        if (!(progress < 1.0)) return 0.0;
        if (progress <= kFadeStartRatio) return 1.0;
        return std::clamp((1.0 - progress) / (1.0 - kFadeStartRatio), 0.0, 1.0);
    }

    /// 填充不透明度：与描边共用同一条曲线，峰值 0.25。
    static double fillOpacity(double progress) { return kFillPeakOpacity * strokeOpacity(progress); }
};

} // namespace komira
