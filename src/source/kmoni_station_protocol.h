#pragma once

namespace komira {

/// NIED Kmoni 实时测站通道（经 Jian Project 转发）的端点与常量。
///
/// 该通道**免鉴权**：Jian 的 web 文档《适用范围》明确「实时测站 /kmoni、/s-net、
/// /kma-station 等不需要访问令牌」，与需要 token 的公开业务源/`/all` 不同。
/// 它也不进入 `/all` 聚合，须单独连接。数据来自 NIED 強震モニタ（色带反演），
/// 仅供显示，不作为官方台站正式报值。
namespace KmoniStationProtocol {

/// 测站通道端点（Jian Project / api.sismotide.top）。
inline const char* const kWsUrl = "wss://api.sismotide.top/kmoni";

/// 上游约每 30s 发一帧 heartbeat；连续缺失 90s 判定链路异常并重连。
inline constexpr int kHeartbeatTimeoutMs = 90 * 1000;

/// 缺测哨兵值：与上游一致，`int[i] == -3` 表示该站无数据。
inline constexpr double kMissing = -3.0;

} // namespace KmoniStationProtocol

} // namespace komira
