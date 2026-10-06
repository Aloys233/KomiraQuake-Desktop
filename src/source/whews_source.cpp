#include "source/whews_source.h"

#include <QDateTime>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <chrono>

#include "source/whews_parser.h"
#include "source/whews_protocol.h"

namespace komira {

namespace {
const QString kBaseDescription =
    QStringLiteral("Whews 聚合（CENC/CEA/JMA/CWA/USGS/EMSC 等 43 频道，优先国内站）");
}

WhewsSource::WhewsSource(QObject* parent) : EarthquakeSource(parent) {
    info_.id = SourceIds::kWhews;
    info_.name = QStringLiteral("Whews");
    info_.region = QStringLiteral("全球");
    info_.description = kBaseDescription;

    reconnectTimer_.setSingleShot(true);
    connect(&reconnectTimer_, &QTimer::timeout, this, [this]() {
        if (!running_) return;
        // 被服务端拒绝鉴权后停摆，等用户换令牌再恢复：setToken 会重新连接。
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

long long WhewsSource::nowMs() const { return nowProvider_ ? nowProvider_() : QDateTime::currentMSecsSinceEpoch(); }

long long WhewsSource::monoMs() const {
    if (monoProvider_) return monoProvider_();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

EewParser::UserLocation WhewsSource::userLocation() const {
    if (!hasLocation_) return std::nullopt;
    return std::make_pair(userLat_, userLon_);
}

void WhewsSource::setUserLocation(double lat, double lon) {
    userLat_ = lat;
    userLon_ = lon;
    hasLocation_ = true;
}

void WhewsSource::clearUserLocation() {
    hasLocation_ = false;
    userLat_ = 0.0;
    userLon_ = 0.0;
}

void WhewsSource::setStatus(ConnectionStatus status, const QString& note) {
    info_.status = status;
    info_.description = note.isEmpty() ? kBaseDescription : note;
    if (status != ConnectionStatus::Connected) info_.latencyMs = -1;
    emit infoChanged();
}

void WhewsSource::setToken(const QString& token) {
    const QString next = token.trimmed();
    if (token_ == next) return;
    token_ = next;
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

void WhewsSource::start() {
    if (running_) return;
    running_ = true;
    ++generation_;
    retryCount_ = 0;
    connectSocket();
}

void WhewsSource::stop() {
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

bool WhewsSource::isFatalClose(QWebSocketProtocol::CloseCode closeCode) {
    // 4401 令牌无效 / 4403 被封禁。这两个码不在 Qt 的 CloseCode 枚举里，
    // Qt 会原样保存线上码值，故按整数比较。
    const int raw = static_cast<int>(closeCode);
    return raw == WhewsProtocol::CloseCode::kUnauthorized || raw == WhewsProtocol::CloseCode::kBanned;
}

void WhewsSource::connectSocket() {
    if (!running_) return;
    if (token_.isEmpty()) {
        setStatus(ConnectionStatus::Disconnected, QStringLiteral("未配置令牌：请在设置中填入 whews 令牌"));
        return;
    }
    const QStringList hosts = WhewsProtocol::wsHosts();
    lastUrl_ = hosts.at(urlIndex_ % hosts.size());
    const quint64 generation = generation_;
    setStatus(ConnectionStatus::Connecting);
    if (!running_ || generation != generation_) return;
    qInfo() << "[whews] ws connecting:" << lastUrl_;
    if (socket_) {
        socket_->disconnect(this);
        socket_->abort();
        socket_->deleteLater();
    }
    // Each connection owns its callbacks; a previous run cannot affect a new one.
    auto* socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    socket_ = socket;
    attachSocketHandlers(socket, generation_, ++attempt_);

    QUrl url(lastUrl_ + WhewsProtocol::wsPath());
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("token"), token_);
    url.setQuery(query);
    socket->open(url);
}

void WhewsSource::attachSocketHandlers(QWebSocket* socket, quint64 generation, quint64 socketAttempt) {
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
        heartbeatWatch_.start(WhewsProtocol::kHeartbeatTimeoutMs);
        emit lastHostChanged();
    });

    // 服务端明确拒绝鉴权时停止重连（4401 令牌无效 / 4403 被封禁）——文档要求，
    // 否则重连过频会被智能封禁。QWebSocket::disconnected 无参数，关闭码取自 closeReason()。
    connect(socket, &QWebSocket::disconnected, this, [this, socket, current]() {
        if (!current()) return;
        if (isFatalClose(socket->closeCode())) {
            ++attempt_;
            heartbeatWatch_.stop();
            setStatus(ConnectionStatus::Error,
                      QStringLiteral("鉴权被拒绝：请检查令牌是否有效或已被吊销"));
            return;
        }
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
        ++attempt_;
        ++urlIndex_;   // 传输失败才前进站点：优先国内站，不可用才切主站
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

void WhewsSource::scheduleReconnect(const QString& note, bool fatal) {
    if (!running_ || fatal) return;
    if (!note.isEmpty()) setStatus(ConnectionStatus::Error, note);
    const int delayMs = std::min(15, std::max(3, 3 + retryCount_)) * 1000;
    ++retryCount_;
    qInfo() << "[whews] reconnect in" << delayMs << "ms (retry" << retryCount_ << ")";
    reconnectTimer_.start(delayMs);
}

void WhewsSource::refreshDirectory() {
    // 情报类端点首连即补发近期缓存，无需主动请求列表。
}

void WhewsSource::handleMessage(const QString& text) {
    if (!running_) return;
    heartbeatWatch_.start(WhewsProtocol::kHeartbeatTimeoutMs);
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
    if (doc.isArray()) {
        // 首连快照：各源最新一条的数组。
        for (const auto& v : doc.array()) {
            if (!running_) return;
            handleFrame(v.toObject());
        }
    } else if (doc.isObject()) {
        handleFrame(doc.object());
    }
}

void WhewsSource::handleFrame(const QJsonObject& frame) {
    if (!running_) return;
    info_.lastHeartbeat = nowMs();
    emit infoChanged();
    if (!running_) return;
    const QJsonObject data = frame.value(QStringLiteral("Data")).toObject();
    if (data.isEmpty()) return;
    const WhewsChannel* channel =
        whewsChannelFor(frame.value(QStringLiteral("source")).toString());
    if (!channel) return;
    auto event = WhewsParser::parseRecord(*channel, data, userLocation(), standard_);
    if (!event) return;
    if (channel->kind == SourceEventKind::Live) {
        // 上游会回放「最近一次」EEW（可能是几小时前的），不能当作实时预警。
        const long long now = nowMs();
        if (event->timestamp > 0 && now - event->timestamp > WhewsProtocol::kEewLiveWindowMs) {
            qInfo() << "[whews] drop stale eew" << QString::fromStdString(event->id) << "age(s)"
                    << (now - event->timestamp) / 1000;
            return;
        }
    }
    emit eventReceived(*event, channel->kind);
}

} // namespace komira