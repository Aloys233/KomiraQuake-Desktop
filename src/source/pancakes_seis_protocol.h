#pragma once

namespace komira {

/// PancakesAPI 实时测站通道（`/api/v1/seis/stream`）端点与常量。
///
/// 与聚合告警通道 `/api/v1/alert/ws/all` 分离：本通道在**同一个** WebSocket 上推送
/// 两类消息 —— 测站档案（含 `"type":"stations"`）与逐站实时帧（不含 `type`，以 `"s"`
/// 开头）。它**只用于地图测站显示**，不产生地震事件、不参与告警 / 去重 / 历史。
///
/// 实时帧是「每秒窗口峰值」的紧凑格式：`p`（PGA，Gal）、`v`（PGV，cm/s）、
/// `i`（仪器烈度，GB/T 17742-2020，设备端计算）**必现**，`m` 为设备端 2 分钟
/// 滑动窗口最大烈度，`ec` 为测站健康码（非 0 时该站该秒必须剔除）。
///
/// 上游已移除 `x`/`y`/`z` 三轴峰值加速度（2026-10-18 实测帧只剩 `s`/`p`/`i`/`m`/`t`），
/// 下游不再解析这三个字段；老报文里若还带着它们会被忽略。
namespace PancakesSeisProtocol {

/// 测站通道端点（api.aloys23.link）。
inline const char* const kWsUrl = "wss://api.aloys23.link/api/v1/seis/stream";

/// 在线测站列表（HTTP）。字段与广播的 `type:"stations"` 档案一致（`sid`/`name`/
/// `location`）。桌面端以 WS 首包档案为准，不额外轮询此接口。
inline const char* const kStationsUrl = "https://api.aloys23.link/api/v1/seis/stations";

/// 上游每个在线测站每 1 秒一帧（该秒窗口峰值）；连续 30s 无任何帧即判链路异常并重连。
inline constexpr int kHeartbeatTimeoutMs = 30 * 1000;

/// 缺测哨兵：尚未收到该站数据（或已静默超时）时的仪器烈度。真实烈度恒为 0 以上，
/// 不会与观测值混淆。
inline constexpr double kMissingIntensity = -1.0;

/// 单站静默超过该时长即置回缺测（上游 1 Hz 推送；掉线测站不残留旧色）。
inline constexpr int kLiveTtlMs = 15 * 1000;

/// 缺测扫描周期。
inline constexpr int kExpiryCheckMs = 5 * 1000;

} // namespace PancakesSeisProtocol

} // namespace komira
