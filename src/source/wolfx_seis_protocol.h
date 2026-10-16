#pragma once

namespace komira {

/// Wolfx SeisJS 实时测站通道（`wss://seisjs.wolfx.jp/all_seis`）端点与常量。
///
/// 实时报文**自带测站坐标与震度**（按站约 1 Hz 推送），但完整的测站名录、展示位置与
/// 启用状态来自官方测站列表 `https://api.wolfx.jp/seis_list.json`（每 10 分钟更新）。
/// 本通道**只用于地图测站显示**，不产生地震事件、不参与告警 / 去重 / 历史。
/// 服务端每分钟（及连接建立时）发一次 `heartbeat`，客户端回 `ping`（文档推荐）。
namespace WolfxSeisProtocol {

/// 实时测站 WebSocket 端点（seisjs.wolfx.jp）。
inline const char* const kWsUrl = "wss://seisjs.wolfx.jp/all_seis";

/// 测站列表端点（api.wolfx.jp），提供测站名录、位置与启用状态。
inline const char* const kListUrl = "https://api.wolfx.jp/seis_list.json";

/// 测站列表刷新周期：与上游更新周期（10 分钟）对齐。
inline constexpr int kListRefreshMs = 10 * 60 * 1000;

/// 上游每分钟发一次 heartbeat；连续 90s 无任何帧判链路异常并重连。
inline constexpr int kHeartbeatTimeoutMs = 90 * 1000;

/// 收到 heartbeat 时回给服务端的探活文本。
inline const char* const kPingPayload = "ping";

/// 缺测哨兵：与 Kmoni 一致，`raw <= -3` 视为无观测/低于基线。
inline constexpr double kMissingShindo = -3.0;

/// 实时帧 `Intensity`（中国地震烈度，整数 0–12）的缺测哨兵：`< 0` 表示无该字段/无观测。
inline constexpr int kMissingIntensity = -1;

/// 单站静默超过该时长即置回缺测（上游按站约 1 Hz 推送；掉线测站不残留旧色）。
inline constexpr int kLiveTtlMs = 120 * 1000;

/// 缺测扫描周期。
inline constexpr int kExpiryCheckMs = 30 * 1000;

} // namespace WolfxSeisProtocol

} // namespace komira
