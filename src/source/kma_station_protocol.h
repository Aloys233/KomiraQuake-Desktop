#pragma once

namespace komira {

/// KMA PEWS 实时测站通道（经 Jian Project 转发）的端点与常量。
///
/// 该通道**免鉴权**：与 `/kmoni`、`/s-net` 同属实时测站路径，不需要访问令牌，
/// 也不进入 `/all` 聚合。数据来自韩国气象厅 PEWS，`mmi` 为韩国 MMI 烈度
/// （约 0–10），仅供显示，不参与预警。
namespace KmaStationProtocol {

/// 测站通道端点（Jian Project / api.sismotide.top）。
inline const char* const kWsUrl = "wss://api.sismotide.top/kma-station";

/// 上游约每 30s 发一帧 heartbeat；连续缺失 90s 判定链路异常并重连。
inline constexpr int kHeartbeatTimeoutMs = 90 * 1000;

/// 缺测哨兵：与上游一致，`mmi[i] == -3` 表示该站无数据。
inline constexpr double kMissing = -3.0;

} // namespace KmaStationProtocol

} // namespace komira
