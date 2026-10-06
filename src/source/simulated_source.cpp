#include "source/simulated_source.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

#include <algorithm>
#include <chrono>

#include "source/simulated_parser.h"
#include "source/simulated_protocol.h"

namespace komira {

namespace {
const QString kBaseDescription = QStringLiteral("本地模拟源（开发自测用，机构 SIM，不参与真实预警）");
} // namespace

SimulatedSource::SimulatedSource(QObject* parent) : EarthquakeSource(parent) {
    info_.id = SourceIds::kSimulated;
    info_.name = QStringLiteral("模拟数据源");
    info_.region = QStringLiteral("自测");
    info_.description = kBaseDescription;

    reconnectTimer_.setSingleShot(true);
    connect(&reconnectTimer_, &QTimer::timeout, this, [this]() {
        if (!running_) return;
        connectSocket();
    });
    heartbeatWatch_.setSingleShot(true);
    connect(&heartbeatWatch_, &QTimer::timeout, this, [this]() {
        if (!running_) return;
        // 退休本次尝试，避免随后到达的 disconnected 再次调度重连。
        ++attempt_;
        setStatus(ConnectionStatus::Error, QStringLiteral("心跳超时"));
        if (socket_) socket_->abort();
        scheduleReconnect();
    });
}

long long SimulatedSource::nowMs() const {
    return nowProvider_ ? nowProvider_() : QDateTime::currentMSecsSinceEpoch();
}

long long SimulatedSource::monoMs() const {
    if (monoProvider_) return monoProvider_();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

EewParser::UserLocation SimulatedSource::userLocation() const {
    if (!hasLocation_) return std::nullopt;
    return std::make_pair(userLat_, userLon_);
}

void SimulatedSource::setUserLocation(double lat, double lon) {
    userLat_ = lat;
    userLon_ = lon;
    hasLocation_ = true;
}

void SimulatedSource::clearUserLocation() {
    hasLocation_ = false;
    userLat_ = 0.0;
    userLon_ = 0.0;
}

void SimulatedSource::setStatus(ConnectionStatus status, const QString& note) {
    info_.status = status;
    info_.description = note.isEmpty() ? kBaseDescription : note;
    if (status != ConnectionStatus::Connected) info_.latencyMs = -1;
    emit infoChanged();
}

void SimulatedSource::setUrl(const QString& url) {
    const QString next = url.trimmed();
    if (url_ == next) return;
    url_ = next;
    if (!running_) return;
    ++generation_;
    ++attempt_;
    reconnectTimer_.stop();
    heartbeatWatch_.stop();
    if (socket_) {
        socket_->disconnect(this);
        socket_->abort();
        socket_->deleteLater();
        socket_ = nullptr;
    }
    connectSocket();
}

void SimulatedSource::start() {
    if (running_) return;
    running_ = true;
    ++generation_;
    retryCount_ = 0;
    connectSocket();
}

void SimulatedSource::stop() {
    running_ = false;
    ++generation_;
    ++attempt_;
    reconnectTimer_.stop();
    heartbeatWatch_.stop();
    if (socket_) {
        socket_->disconnect(this);
        socket_->abort();
        socket_->deleteLater();
        socket_ = nullptr;
    }
    if (info_.status != ConnectionStatus::Disconnected) setStatus(ConnectionStatus::Disconnected);
}

void SimulatedSource::connectSocket() {
    if (!running_) return;
    if (!isConfigured()) {
        // 双保险：上层已在 isConfigured() 为假时跳过 start()，走到这里说明状态刚被翻转。
        setStatus(ConnectionStatus::Disconnected,
                  devMode_ ? QStringLiteral("未配置地址：请在设置中填入模拟源地址")
                           : QStringLiteral("开发者模式未开启"));
        return;
    }
    lastUrl_ = url_;
    const quint64 generation = generation_;
    setStatus(ConnectionStatus::Connecting);
    if (!running_ || generation != generation_) return;
    qInfo() << "[simulated] ws connecting:" << lastUrl_;

    if (socket_) {
        socket_->disconnect(this);
        socket_->abort();
        socket_->deleteLater();
    }
    // Each connection owns its callbacks; a previous run cannot affect a new one.
    auto* socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    socket_ = socket;
    attachSocketHandlers(socket, generation_, ++attempt_);
    socket->open(QUrl(lastUrl_));
}

void SimulatedSource::attachSocketHandlers(QWebSocket* socket, quint64 generation,
                                           quint64 socketAttempt) {
    // socketAttempt 令牌 + socket_ 指针双保险：过期尝试的迟到回调在此被丢弃。
    const auto current = [this, socket, generation, socketAttempt]() {
        return running_ && generation == generation_ && socketAttempt == attempt_ && socket == socket_;
    };

    connect(socket, &QWebSocket::connected, this, [this, current]() {
        if (!current()) return;
        retryCount_ = 0;
        reconnectTimer_.stop();
        info_.lastHeartbeat = nowMs();
        setStatus(ConnectionStatus::Connected);
        heartbeatWatch_.start(SimulatedProtocol::kHeartbeatTimeoutMs);
        // 报个到，服务端控制台据此显示是哪个客户端接入了。
        if (socket_) {
            QJsonObject hello;
            hello.insert(QStringLiteral("type"), QStringLiteral("hello"));
            hello.insert(QStringLiteral("client"), QStringLiteral("linux"));
            socket_->sendTextMessage(
                QString::fromUtf8(QJsonDocument(hello).toJson(QJsonDocument::Compact)));
        }
    });

    connect(socket, &QWebSocket::disconnected, this, [this, current]() {
        if (!current()) return;
        ++attempt_;
        heartbeatWatch_.stop();
        setStatus(ConnectionStatus::Disconnected);
        scheduleReconnect();
    });

    connect(socket, &QWebSocket::textMessageReceived, this, [this, current](const QString& text) {
        if (current()) handleMessage(text);
    });

    connect(socket, &QWebSocket::errorOccurred, this,
            [this, socket, current](QAbstractSocket::SocketError) {
        if (!current()) return;
        // 与 disconnected 都会触发：谁先跑谁 ++attempt_，另一个因此跳过调度。
        ++attempt_;
        heartbeatWatch_.stop();
        setStatus(ConnectionStatus::Error,
                  QStringLiteral("WS 连接失败：%1").arg(socket->errorString()));
        scheduleReconnect();
    });

    connect(socket, &QWebSocket::pong, this, [this, current](quint64 elapsed, const QByteArray&) {
        if (!current()) return;
        info_.latencyMs = static_cast<qint64>(elapsed);
        info_.lastHeartbeat = nowMs();
        emit infoChanged();
    });
}

void SimulatedSource::scheduleReconnect(const QString& note) {
    if (!running_) return;
    if (!note.isEmpty()) setStatus(ConnectionStatus::Error, note);
    const int delayMs = std::min(15, std::max(3, 3 + retryCount_)) * 1000;
    ++retryCount_;
    qInfo() << "[simulated] reconnect in" << delayMs << "ms (retry" << retryCount_ << ")";
    reconnectTimer_.start(delayMs);
}

void SimulatedSource::refreshDirectory() {
    // 服务端按自己的节奏推报文，无需主动请求列表。
}

void SimulatedSource::handleMessage(const QString& text) {
    if (!running_) return;
    heartbeatWatch_.start(SimulatedProtocol::kHeartbeatTimeoutMs);
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
    if (doc.isArray()) {
        // 批量快照：逐条处理，每条都重新校验会话。
        for (const auto& v : doc.array()) {
            if (!running_) return;
            handleFrame(v.toObject());
        }
    } else if (doc.isObject()) {
        handleFrame(doc.object());
    }
}

void SimulatedSource::handleFrame(const QJsonObject& frame) {
    if (!running_) return;
    const QString type = frame.value(QStringLiteral("type")).toString();

    // 控制帧：不产生事件，只刷新心跳。
    if (type == QStringLiteral("ping") || type == QStringLiteral("hello")) {
        info_.lastHeartbeat = nowMs();
        emit infoChanged();
        return;
    }

    const SourceEventKind kind = type == QStringLiteral("directory") ? SourceEventKind::Directory
                                                                    : SourceEventKind::Live;
    const long long now = nowMs();
    auto event = SimulatedParser::parseReport(frame, kind, userLocation(), standard_, now);
    if (!event) {
        qInfo() << "[simulated] drop malformed frame";
        return;
    }

    // 活跃窗口：陈旧帧丢弃。服务端会钳制未来时刻，但客户端这一侧仍要独立把关，
    // 且回执要让作者看到「这一帧为什么没生效」。
    if (kind == SourceEventKind::Live && event->timestamp > 0 &&
        now - event->timestamp > SimulatedProtocol::kEewLiveWindowMs) {
        sendAck(*event, QStringLiteral("stale"));
        return;
    }
    // 未来时刻：这帧一出生就判过期（与 Android EventLifecycle 同规则）。
    if (event->timestamp > now + 60'000) {
        sendAck(*event, QStringLiteral("future"));
        return;
    }

    emit eventReceived(*event, kind);
    // 不在这里回执：墓碑 / 重复这些判定在仓库层（AppController::handleEvent），
    // 源无从得知。由 onAdmission 回调补发，回执才如实反映最终处置。
    lastEmitted_ = *event;
    lastKind_ = kind;
}

void SimulatedSource::onAdmission(const EarthquakeEvent& event, AdmissionStatus status) {
    // 只回自己刚发出且未被更晚一帧顶替的那一帧，避免乱序回执张冠李戴。
    if (event.identity() != lastEmitted_.identity() || event.reportNum != lastEmitted_.reportNum)
        return;
    QString label;
    switch (status) {
    case AdmissionStatus::Applied: label = QStringLiteral("applied"); break;
    case AdmissionStatus::Tombstoned: label = QStringLiteral("tombstoned"); break;
    case AdmissionStatus::Duplicate: label = QStringLiteral("duplicate"); break;
    case AdmissionStatus::Ended: label = lastEmitted_.isCanceled ? QStringLiteral("canceled")
                                                                : QStringLiteral("ended"); break;
    }
    sendAck(event, label);
}

void SimulatedSource::sendAck(const EarthquakeEvent& event, const QString& status) {
    if (!socket_ || socket_->state() != QAbstractSocket::ConnectedState) return;
    QJsonObject ack;
    ack.insert(QStringLiteral("type"), QStringLiteral("ack"));
    ack.insert(QStringLiteral("eventId"), QString::fromStdString(event.eventId));
    ack.insert(QStringLiteral("reportNum"), event.reportNum);
    ack.insert(QStringLiteral("status"), status);
    socket_->sendTextMessage(QString::fromUtf8(QJsonDocument(ack).toJson(QJsonDocument::Compact)));
}

} // namespace komira
