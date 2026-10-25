#include "source/jian_source.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <chrono>

#include "source/jian_parser.h"
#include "source/jian_protocol.h"
#include "source/raw_payload.h"
#include "source/ws_latency_probe.h"

namespace komira {

namespace {
const QString kBaseDescription =
    QStringLiteral("Jian（api.sismotide.top）聚合（CEA/JMA/CWA/KMA/SA 等 46 路地震频道）");
}

JianSource::JianSource(QObject* parent) : EarthquakeSource(parent) {
    info_.id = SourceIds::kJian;
    info_.name = QStringLiteral("Jian");
    info_.region = QStringLiteral("全球");
    info_.description = kBaseDescription;

    reconnectTimer_.setSingleShot(true);
    connect(&reconnectTimer_, &QTimer::timeout, this, &JianSource::ensureAccessAndConnect);
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

long long JianSource::nowMs() const { return nowProvider_ ? nowProvider_() : QDateTime::currentMSecsSinceEpoch(); }

long long JianSource::monoMs() const {
    if (monoProvider_) return monoProvider_();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

EewParser::UserLocation JianSource::userLocation() const {
    if (!hasLocation_) return std::nullopt;
    return std::make_pair(userLat_, userLon_);
}

void JianSource::setUserLocation(double lat, double lon) {
    userLat_ = lat;
    userLon_ = lon;
    hasLocation_ = true;
}

void JianSource::clearUserLocation() { hasLocation_ = false; }

void JianSource::setStatus(ConnectionStatus status, const QString& note) {
    info_.status = status;
    info_.description = note.isEmpty() ? kBaseDescription : note;
    if (status != ConnectionStatus::Connected) info_.latencyMs = -1;
    emit infoChanged();
}

void JianSource::setRefreshToken(const QString& token) {
    if (refreshToken_ == token) return;
    refreshToken_ = token;
    accessToken_.clear();
    accessExpiryMonoMs_ = 0;
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
    ensureAccessAndConnect();
}

void JianSource::start() {
    if (running_) return;
    running_ = true;
    ++generation_;
    retryCount_ = 0;
    ensureAccessAndConnect();
}

void JianSource::stop() {
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

void JianSource::ensureAccessAndConnect() {
    if (!running_) return;
    if (refreshToken_.isEmpty()) {
        setStatus(ConnectionStatus::Disconnected, QStringLiteral("未登录：请在设置中填入登录密钥（lk_…）"));
        return;
    }
    if (accessToken_.isEmpty() || monoMs() >= accessExpiryMonoMs_) {
        fetchAccessToken();
        return;
    }
    connectSocket();
}

void JianSource::fetchAccessToken() {
    if (!running_) return;
    const quint64 generation = generation_;
    setStatus(ConnectionStatus::Connecting, QStringLiteral("获取访问令牌…"));
    QNetworkRequest request{QUrl(JianProtocol::accessUrl())};
    request.setTransferTimeout(15000);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("komiraquake/2.0"));
    request.setRawHeader("Authorization", "Bearer " + refreshToken_.toUtf8());
    QNetworkReply* reply = network_.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation]() {
        const QByteArray body = reply->readAll();
        const bool httpOk = reply->error() == QNetworkReply::NoError;
        const QString errorText = reply->errorString();
        reply->deleteLater();
        if (!running_ || generation != generation_) return;
        const auto token = JianParser::parseAuthToken(body);
        if (!httpOk || !token) {
            const QString detail = httpOk ? QStringLiteral("刷新令牌无效或已过期") : errorText;
            setStatus(ConnectionStatus::Error, QStringLiteral("获取访问令牌失败：") + detail);
            scheduleReconnect();
            return;
        }
        accessToken_ = *token;
        // at_ 约 1 小时；留 5 分钟余量，避免握手瞬间过期。
        accessExpiryMonoMs_ = monoMs() + 55LL * 60 * 1000;
        connectSocket();
    });
}

void JianSource::connectSocket() {
    if (!running_) return;
    const quint64 generation = generation_;
    if (socket_) {
        socket_->disconnect(this);
        socket_->abort();
        socket_->deleteLater();
        socket_ = nullptr;
    }
    setStatus(ConnectionStatus::Connecting);
    auto* socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    socket_ = socket;
    attachSocketHandlers(socket, generation, ++attempt_);

    QUrl url(JianProtocol::wsUrl());
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("key"), accessToken_);
    url.setQuery(query);
    socket->open(url);
}

void JianSource::attachSocketHandlers(QWebSocket* socket, quint64 generation, quint64 socketAttempt) {
    const auto current = [this, socket, generation, socketAttempt]() {
        return running_ && generation == generation_ && socketAttempt == attempt_ && socket == socket_;
    };

    connect(socket, &QWebSocket::connected, this, [this, current]() {
        if (!current()) return;
        retryCount_ = 0;
        reconnectTimer_.stop();
        setStatus(ConnectionStatus::Connected);
        heartbeatWatch_.start(JianProtocol::kHeartbeatTimeoutMs);
        // 请求历史列表：/all 会依次回推 cenc/cwa/jma/hko 的 *list_response。
        socket_->sendTextMessage(JianProtocol::listCommand());
    });

    connect(socket, &QWebSocket::disconnected, this, [this, current]() {
        if (!current()) return;
        ++attempt_;
        heartbeatWatch_.stop();
        scheduleReconnect();
    });

    connect(socket, &QWebSocket::textMessageReceived, this,
            [this, current](const QString& text) {
        if (current()) handleMessage(text);
    });

    connect(socket, &QWebSocket::errorOccurred, this,
            [this, socket, current](QAbstractSocket::SocketError) {
        if (!current()) return;
        ++attempt_;
        heartbeatWatch_.stop();
        setStatus(ConnectionStatus::Error,
                  QStringLiteral("WS 连接失败：%1").arg(socket->errorString()));
        scheduleReconnect();
    });

    // 链路延迟：协议级 ping/pong 探针。Jian 的业务帧里只有发震时刻、没有发出时刻，
    // 推不出链路 RTT，此前完全没有延迟（latencyMs 恒为 -1）。
    attachLatencyProbe(socket, current, [this](qint64 latencyMs) {
        if (info_.latencyMs == latencyMs) return;
        info_.latencyMs = latencyMs;
        emit infoChanged();
    });
}

void JianSource::scheduleReconnect(const QString& note) {
    if (!running_) return;
    if (!note.isEmpty()) setStatus(ConnectionStatus::Error, note);
    const int delayMs = std::min(15, std::max(3, 3 + retryCount_)) * 1000;
    ++retryCount_;
    reconnectTimer_.start(delayMs);
}

void JianSource::refreshDirectory() {
    if (!running_ || !socket_ || socket_->state() != QAbstractSocket::ConnectedState) return;
    socket_->sendTextMessage(JianProtocol::listCommand());
}

void JianSource::handleMessage(const QString& text) {
    if (!running_) return;
    heartbeatWatch_.start(JianProtocol::kHeartbeatTimeoutMs);
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
    if (!doc.isObject()) return;
    const QJsonObject obj = doc.object();
    const QString type = obj.value(QStringLiteral("type")).toString();

    if (type == QLatin1String("heartbeat") || type == QLatin1String("pong")) return;

    if (type == QLatin1String("error")) {
        const int code = obj.value(QStringLiteral("code")).toInt(0);
        const QString message = obj.value(QStringLiteral("message")).toString(
            obj.value(QStringLiteral("error")).toString());
        // 4004 = 访问令牌过期：丢弃并重取。
        if (code == 4004) {
            accessToken_.clear();
            accessExpiryMonoMs_ = 0;
        }
        setStatus(ConnectionStatus::Error, message.isEmpty() ? QStringLiteral("服务端错误") : message);
        ++attempt_;
        if (socket_) socket_->abort();
        scheduleReconnect();
        return;
    }

    // /all 首帧快照：{type:"all", "source：xxx": {Data, md5}, …}
    //
    // **冒号是全角 U+FF1A，不是 ASCII ':'**。上游文档与真实快照都用全角（`source：cea`），
    // 参考实现也是用 `^source[:：]` 同时兜住两种写法。此前这里只匹配半角 `"source:"`，
    // 结果整帧快照被静默丢弃 —— 表现为「连上了、也有历史列表，但最新一报永远不出现」，
    // 而且不报任何错。两端都必须同时接受两种冒号。
    if (type == QLatin1String("all")) {
        static const QString kHalfPrefix = QStringLiteral("source:");
        // 用 QChar(0xFF1A) 显式构造：写成 QStringLiteral("source\xEF\xBC\x9A") 会把 UTF-8
        // 的三个字节当成三个 UTF-16 码元（U+00EF U+00BC U+009A），看起来像全角冒号、
        // 实际永远匹配不上 —— 这个坑本身也会让「修好的代码」继续静默丢帧。
        static const QString kFullPrefix =
            QStringLiteral("source") + QChar(0xFF1A);   // '：'
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            const QString& key = it.key();
            const QString prefix = key.startsWith(kHalfPrefix) ? kHalfPrefix
                                  : key.startsWith(kFullPrefix) ? kFullPrefix
                                                                : QString();
            if (prefix.isEmpty()) continue;
            handleRecord(key.mid(prefix.size()),
                         it.value().toObject().value(QStringLiteral("Data")).toObject());
        }
        return;
    }

    // 历史列表：{type:"<chan>list_response", Data:[…]}
    static const QString kListSuffix = QStringLiteral("list_response");
    if (type.endsWith(kListSuffix)) {
        const QString base = type.left(type.size() - kListSuffix.size());
        const QJsonValue data = obj.value(QStringLiteral("Data"));
        if (data.isArray()) {
            for (const QJsonValue& v : data.toArray()) handleRecord(base, v.toObject());
        } else if (data.isObject()) {
            handleRecord(base, data.toObject());
        }
        return;
    }

    handleRecord(type, obj.value(QStringLiteral("Data")).toObject());
}

void JianSource::handleRecord(const QString& type, const QJsonObject& data) {
    if (data.isEmpty()) return;
    const JianChannel* channel = jianChannelFor(type);
    if (!channel) return;
    if (!isSubSourceEnabled(channel->type)) return;
    auto event = JianParser::parseRecord(*channel, data, userLocation(), standard_);
    if (!event) return;
    if (channel->kind == SourceEventKind::Live) {
        const long long now = nowMs();
        if (event->timestamp > 0 && now - event->timestamp > JianProtocol::kEewLiveWindowMs) return;
    }
    attachRawPayload(*event, data);
    emit eventReceived(*event, channel->kind);
}

void JianSource::login(const QString& loginKey) {
    const QString key = loginKey.trimmed();
    if (key.isEmpty()) return;
    QNetworkRequest request{QUrl(JianProtocol::refreshUrl())};
    request.setTransferTimeout(15000);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("komiraquake/2.0"));
    request.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    QNetworkReply* reply = network_.post(request, QByteArray());
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QByteArray body = reply->readAll();
        const bool httpOk = reply->error() == QNetworkReply::NoError;
        const QString errorText = reply->errorString();
        reply->deleteLater();
        const auto token = JianParser::parseAuthToken(body);
        if (!httpOk || !token) {
            setStatus(ConnectionStatus::Error,
                      QStringLiteral("登录失败：")
                          + (httpOk ? QStringLiteral("登录密钥无效或已过期") : errorText));
            return;
        }
        refreshToken_ = *token;
        accessToken_.clear();
        accessExpiryMonoMs_ = 0;
        emit refreshTokenObtained(*token);
        if (running_) ensureAccessAndConnect();
    });
}

} // namespace komira
