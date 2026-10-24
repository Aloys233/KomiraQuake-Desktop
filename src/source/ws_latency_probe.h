#pragma once

#include <QAbstractSocket>
#include <QByteArray>
#include <QObject>
#include <QTimer>
#include <QWebSocket>

#include <functional>
#include <memory>
#include <utility>

namespace komira {

/// 协议级链路延迟探针：`DataSourceInfo::latencyMs` 的唯一来源。
///
/// **为什么统一走 RFC 6455 的 ping/pong**：
/// ① 五个事件源的上行协议各不相同 —— Wolfx 用 JSON `heartbeat`/`pong` 业务帧、Whews /
///    Jian / Pancakes 的业务帧里**没有「发出时刻」字段**（报文带的是发震时刻，不是发出
///    时刻）、模拟源是自建的 `ping` 帧。从业务报文里推不出链路 RTT，一个源一种算法必然
///    加一个源就漏一个；
/// ② ping/pong 是 WebSocket 协议**必选**的控制帧，服务端不支持时本探针静默不显示延迟，
///    不像自定义心跳那样要求每个上游单独适配；
/// ③ `pong` 携带的 `elapsedTime` 由 Qt 用单调时钟测量，不受本机校时偏移与系统时间跳变
///    影响。
///
/// **修复的原缺陷**：此前只有 `WolfxSource` 调用过 `socket_->ping()`；Whews 与 Simulated
/// 虽然连了 `QWebSocket::pong` 却从不 ping（死代码），Jian / Pancakes 连 pong 都没接 ——
/// 于是 `latencyMs` 恒为 -1，设置页的延迟对四个源永远不显示。
///
/// **判定「这个 pong 是我们问出来的」**：`QWebSocket::pong(elapsed)` 在收到**未经请求**
/// 的 pong 时 elapsed 为 0。但经由本机回环的链路真实 RTT 也可能 <1 ms、同样量到 0，
/// 仅凭 elapsed 判不出区别。故由探针自己记 `pending`：只有先发过 ping，下一个 pong
/// 才算一次测量（0 ms 也照收）。
///
/// **超时语义**：连续 3 轮都要不到答复 → 回调 -1（界面不显示延迟）。有些中间设备会吞
/// 控制帧，这时宁可显示「未知」，也不能让几分钟前的旧值一直挂在设置页上冒充当前状况。
///
/// **生命周期**：定时器与全部连接都以 `socket` 为父，socket 被 `deleteLater()` 时探针一起
/// 消失，源不需要自己清理。`current` 用来丢弃「已退休但尚未析构」的旧连接迟到的 pong，
/// 避免把上一条链路的延迟写进当前状态。
inline void attachLatencyProbe(QWebSocket* socket, std::function<bool()> current,
                               std::function<void(qint64)> onLatency) {
    if (!socket) return;

    /// 10 s 一轮：ping 帧是 6 字节头 + 4 字节 payload，五个源合计约 1 B/s，却足以让
    /// 「延迟」跟着网络状态变化，而不是停在连上时的那一个数。
    constexpr int kProbeIntervalMs = 10 * 1000;
    /// 连续这么多轮要不到答复 → 判为测不出（-1）。
    constexpr int kStaleAfterMisses = 3;

    struct State {
        int misses = 0;        ///< 连续多少轮没有答复
        bool pending = false;  ///< 是否有一个已发出、尚未被回答的 ping
        bool reportedUnknown = false;
    };
    auto state = std::make_shared<State>();

    auto* timer = new QTimer(socket);
    timer->setObjectName(QStringLiteral("latencyProbe"));
    timer->setInterval(kProbeIntervalMs);

    /// 一轮探测。未连接时不发 ping（`ping()` 在未连接状态下会静默失败，表现为
    /// 「连上了却没有延迟」，很难查），但**仍然记一轮**——按期要不到答复与连接态无关。
    const auto probe = [socket, state, onLatency]() {
        if (++state->misses > kStaleAfterMisses) {
            // 只报一次 -1，避免每 10 s 刷一条 infoChanged。
            if (!state->reportedUnknown) {
                state->reportedUnknown = true;
                state->pending = false;
                if (onLatency) onLatency(-1);
            }
            return;
        }
        if (socket->state() != QAbstractSocket::ConnectedState) return;
        state->pending = true;
        socket->ping();
    };

    QObject::connect(timer, &QTimer::timeout, socket, probe);
    QObject::connect(socket, &QWebSocket::connected, socket, [timer, probe]() {
        // 连上立刻测一次：不必等第一个探测周期，首帧业务数据到达前就有延迟可看。
        probe();
        timer->start();
    });
    QObject::connect(socket, &QWebSocket::disconnected, socket, [timer]() { timer->stop(); });
    QObject::connect(socket, &QWebSocket::errorOccurred, timer,
                     [timer](QAbstractSocket::SocketError) { timer->stop(); });

    QObject::connect(socket, &QWebSocket::pong, socket,
                     [current = std::move(current), onLatency = std::move(onLatency), state](
                         quint64 elapsed, const QByteArray&) {
        if (current && !current()) return;
        // 没发过 ping 就收到的 pong（服务端主动发的，或上一条连接迟到的）不算测量。
        if (!state->pending) return;
        state->misses = 0;
        state->pending = false;
        state->reportedUnknown = false;
        if (onLatency) onLatency(static_cast<qint64>(elapsed));
    });
}

} // namespace komira
