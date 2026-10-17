#pragma once

namespace komira {

/// S-Net 海底地震观测网实时测站通道（经 Jian Project 转发）的端点与常量。
///
/// 该通道**免鉴权**：与 `/kmoni`、`/kma-station` 同属实时测站路径，不需要访问令牌，
/// 也不进入 `/all` 聚合。数据为日本近海 S-Net 海底地震仪的实时震度（由色带反演），
/// 仅供显示，不作为官方台站正式报值。
namespace SnetStationProtocol {

/// 测站通道端点（Jian Project / api.sismotide.top）。
inline const char* const kWsUrl = "wss://api.sismotide.top/s-net";

/// 上游约每 30s 发一帧 heartbeat；连续缺失 90s 判定链路异常并重连。
inline constexpr int kHeartbeatTimeoutMs = 90 * 1000;

/// 缺测哨兵：与上游一致，`int[i] == -3` 表示该站无感/未观测。
inline constexpr double kMissing = -3.0;

/// 饱和值：上游用 `99` 表示超出色带量程，显示时按 JMA 震度上限处理。
inline constexpr double kSaturation = 99.0;

} // namespace SnetStationProtocol

} // namespace komira
