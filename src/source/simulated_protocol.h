#pragma once

#include <QString>

#include "source/source_event_kind.h"

namespace komira {

/// 模拟数据源常量。协议 sim-eew/1，契约见《NATIVE_PORT_SPEC》 §2.7。
///
/// 服务端在仓库 `Simulated/`（Go + WebSocket + 浏览器控制台），仅供开发自测。
struct SimulatedProtocol {
    /// 机构代号。**必须是 SIM，不得改用真实机构代号**：合并键是
    /// `sourceAgency|eventId`，若与真实报文（如 CENC）撞键，EventGate 会判进同一条目，
    /// 告警不触发，用户看到的是真实数据被假数据静默覆盖。
    static QString agency() { return QStringLiteral("SIM"); }

    static QString provider() { return QStringLiteral("Simulated"); }

    /// 端点路径；主机与端口由用户在设置页自填。
    static QString wsPath() { return QStringLiteral("/ws"); }

    static constexpr int kHeartbeatTimeoutMs = 90 * 1000;
    /// Live 帧的活跃窗口：超此年龄的帧由本源丢弃（上游会回放「最近一次」预警）。
    static constexpr long long kEewLiveWindowMs = 30LL * 60LL * 1000LL;
};

/// 报文展示名（HUD 标题）。带上「模拟」字样，避免测试时被误认为真实预警。
QString simulatedTitleFor(SourceEventKind kind);

} // namespace komira
