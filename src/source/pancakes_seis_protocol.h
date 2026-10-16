#pragma once

namespace komira {

/// PancakesAPI 实时测站通道（`/api/v1/seis/stream`）端点与常量。
///
/// 与聚合告警通道 `/api/v1/alert/ws/all` 分离：本通道逐站推送三轴加速度，每条报文
/// 只含一个测站，不进入 `/all` 聚合。它**只用于地图测站显示**，不产生地震事件、
/// 不参与告警 / 去重 / 历史。报文里的 `e`（芯片温度）不解析。
namespace PancakesSeisProtocol {

/// 测站通道端点（api.aloys23.link）。
inline const char* const kWsUrl = "wss://api.aloys23.link/api/v1/seis/stream";

/// 上游按测站逐帧推送（约每站 1 Hz）；连续 30s 无任何帧即判链路异常并重连。
inline constexpr int kHeartbeatTimeoutMs = 30 * 1000;

/// 缺测哨兵：尚未收到该站数据时的 PGA（Gal）。真实 PGA 恒为正，不会与观测值混淆。
inline constexpr double kMissingPga = -1.0;

} // namespace PancakesSeisProtocol

} // namespace komira
