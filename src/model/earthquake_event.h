#pragma once

#include <cstdint>
#include <optional>
#include <string>
// `sourceTags` 是 std::vector<std::string>；这里显式包含，不依赖别的头间接带进来
// （事件头被大量 TU 引用，缺了它就会在「谁先包含谁」变化时随机编译失败）。
#include <vector>

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
    /// 抓取该记录的数据源 ID（`wolfx` / `pancakes` / `jian` / `whews`…），
    /// 供设置页的逐源「信息事件震级过滤」在**展示层**复核。旧库记录为空。
    std::string sourceId;
    /// 这份上游报文被哪些聚合源送达过，元素形如 `Wolfx·CENC`（`sourceTagFor` 的产物）。
    ///
    /// **必须是累积量，不能只放「本条记录自己的来源」**：两个聚合源转送同一份报文时
    /// `id` 完全相同（都按 `<频道>_<上游 id>` 拼），`history_` 按 id 归并后只剩后到的那
    /// 一条记录 —— 若标签只挂在单条记录上，先到的那个源就从列表里彻底消失了（实测
    /// 2026-10-25 19:24 西藏那曲 M4.8：列表只显示 `Jian·CENC`，详情却有两路报次）。
    /// 落库后重启仍完整（events 表的 `source_tags` 列）。
    std::vector<std::string> sourceTags;
    double distanceKm = 0.0;
    /// 本条报文的原始 JSON（紧凑序列化）。只用于详情页「原始数据」展示，
    /// 不进入 events 表；报次归档见 AppController 的 event_reports。
    std::string rawJson;
    /// 抓取到本条报文的本地时刻（epoch ms，校时时钟）。详情页按源展示接收时间。
    long long receivedAt = 0;
    std::string estimatedIntensity;   // 本地预估烈度（需定位；无定位为 "--"）
    double rawIntensity = 0.0;
    double maxIntensityRaw = 0.0;     // 源报最大烈度（数值；未知为 0）
    std::string maxIntensityText;     // 源报最大烈度（展示文本；未知为空）
    /// 测定状态（仅目录/情报报文有意义）：`reviewed`（正式测定）/ `automatic`
    /// （自动测定）/ 空（上游没有这个字段）。只用于详情页标注，不参与去重与合并。
    std::string determination;
    std::optional<long long> pWaveArrival;
    std::optional<long long> sWaveArrival;
    WarningLevel warningLevel = WarningLevel::Normal;
    int reportNum = 1;
    bool isFinal = false;
    bool isCanceled = false;
    /// 目录/情报报文（相对于实时预警）。情报没有「报次」语义：同一条上游电文只会
    /// 出现在列表与历史里，报次（第 N 报）只由实时预警产生。UI 据此决定标「情报」
    /// 还是「第 N 报」，不再把恒为 1 的 reportNum 当成报次显示。
    bool isInfo = false;
    /// 上游已发布**警报**（目前只有 JMA 的 `isWarn` 提供）。用于区分气象厅的
    /// 「緊急地震速報（警報）」与「（予報）」：前者是官方对强震动地区发布的警报，
    /// 后者只是速报。仅影响展示名与最低告警级别，不参与去重与合并。
    bool alertIssued = false;

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
