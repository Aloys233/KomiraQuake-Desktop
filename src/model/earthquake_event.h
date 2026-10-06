#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "model/warning_level.h"

namespace komira {

/// 地震事件。字段与《NATIVE_PORT_SPEC》 §1.1 一一对应；时间统一为 epoch 毫秒。
struct EarthquakeEvent {
    std::string id;
    /// 数据源原始事件 ID（跨报次/跨链路稳定，用于同一地震的合并）。
    std::string eventId;
    double magnitude = 0.0;
    double latitude = 0.0;
    double longitude = 0.0;
    double depth = 10.0;
    std::string location;
    long long timestamp = 0;
    long long reportTime = 0;
    /// 数据源 / 报文展示名（HUD 标题），如 "中国地震预警网 地震预警"。
    std::string source;
    /// 数据源提供方，如 "Wolfx"。
    std::string sourceProvider;
    /// 报数机构，如 "CENC"、"JMA"。UI 以 `<provider>·<agency>` 标明来源。
    std::string sourceAgency;
    double distanceKm = 0.0;
    std::string estimatedIntensity;   // 本地预估烈度（需定位；无定位为 "--"）
    double rawIntensity = 0.0;
    double maxIntensityRaw = 0.0;     // 源报最大烈度（数值；未知为 0）
    std::string maxIntensityText;     // 源报最大烈度（展示文本；未知为空）
    std::optional<long long> pWaveArrival;
    std::optional<long long> sWaveArrival;
    WarningLevel warningLevel = WarningLevel::Normal;
    int reportNum = 1;
    bool isFinal = false;
    bool isCanceled = false;

    int remainingSeconds(long long nowMs) const {
        if (!sWaveArrival) return -1;
        const long long diff = (*sWaveArrival - nowMs) / 1000;
        return diff > 0 ? static_cast<int>(diff) : 0;
    }

    int remainingPSeconds(long long nowMs) const {
        if (!pWaveArrival) return -1;
        const long long diff = (*pWaveArrival - nowMs) / 1000;
        return diff > 0 ? static_cast<int>(diff) : 0;
    }

    bool isPWaveArrived(long long nowMs) const { return pWaveArrival && nowMs >= *pWaveArrival; }
    bool isSWaveArrived(long long nowMs) const { return sWaveArrival && nowMs >= *sWaveArrival; }

    /// 同一物理报文的跨聚合商合并键：**报数机构 + 频道化 eventId**，刻意不含 sourceProvider。
    /// Wolfx 与 Pancakes 会各自转发同一份上游报文，只有键不含聚合商，二者才会落进同一会话
    /// / 同一去重条目（互为备份、不重复告警）。eventId 必须带频道前缀（如 `jma_eew:`）以区分
    /// 共用上游 ID 的 EEW 与速报。
    std::string identity() const {
        return (sourceAgency.empty() ? source : sourceAgency) + "|" + (eventId.empty() ? id : eventId);
    }

    bool expired(long long nowMs) const {
        // 未来时刻同样判过期：与 Android EventLifecycle 同规则，否则同一帧在两端
        // 一个丢弃一个接受，倒计时会挂起一个小时的「有效」预警。
        return timestamp <= 0 || timestamp > nowMs + 60'000 ||
               nowMs >= timestamp + 30LL * 60 * 1000 ||
               nowMs >= (sWaveArrival ? *sWaveArrival + 60LL * 1000
                                      : timestamp + 5LL * 60 * 1000);
    }
};

} // namespace komira
