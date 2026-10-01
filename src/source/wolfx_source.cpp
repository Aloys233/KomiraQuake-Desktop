#include "source/wolfx_source.h"

#include <QDateTime>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <algorithm>
#include <chrono>

#include "source/eew_parser.h"
#include "source/wolfx_protocol.h"

namespace komira {

namespace {

/// 标明来源：列表卡片左下角显示 `<provider>·<agency>`；新增数据源时同样在此打标。
void stampSource(EarthquakeEvent& event, const QString& provider, const QString& agency) {
    event.sourceProvider = provider.toStdString();
    event.sourceAgency = agency.toStdString();
}

} // namespace

WolfxSource::WolfxSource(QObject* parent) : QObject(parent) {
    info_.id = SourceIds::kWolfx;
    info_.name = QStringLiteral("Wolfx");
    info_.region = QStringLiteral("全球");
    info_.description = QStringLiteral("Wolfx all_eew 聚合（CENC/SC/JMA/CWA/FJ/CQ）");


    connect(&queryTimer_, &QTimer::timeout, this, &WolfxSource::sendQueries);
    connect(&pollTimer_, &QTimer::timeout, this, &WolfxSource::pollDirectory);
    reconnectTimer_.setSingleShot(true);
    connect(&reconnectTimer_, &QTimer::timeout, this, &WolfxSource::connectSocket);
}

void WolfxSource::setUserLocation(double lat, double lon) {
    userLat_ = lat;
    userLon_ = lon;
    hasLocation_ = true;
}

void WolfxSource::clearUserLocation() {
    hasLocation_ = false;
    userLat_ = 0.0;
    userLon_ = 0.0;
}

EewParser::UserLocation WolfxSource::userLocation() const {
    if (!hasLocation_) return std::nullopt;
    return std::make_pair(userLat_, userLon_);
}

long long WolfxSource::nowMs() const {
    return nowProvider_ ? nowProvider_() : QDateTime::currentMSecsSinceEpoch();
}

long long WolfxSource::monoMs() const {
    return monoProvider_ ? monoProvider_()
        : std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now().time_since_epoch()).count();
}

void WolfxSource::start() {
    if (running_) return;
    // 定位可选：数据源连接与事件接收不依赖定位。
    running_ = true;
    const quint64 generation = ++generation_;
    connectSocket();
    if (!running_ || generation != generation_) return;
    pollTimer_.start(WolfxProtocol::kPollIntervalMs);
    pollDirectory();
}

void WolfxSource::stop() {
    running_ = false;
    ++generation_;
    ++attempt_;
    queryTimer_.stop();
    pollTimer_.stop();
    reconnectTimer_.stop();
    if (socket_) {
        socket_->disconnect(this);
        socket_->abort();
        socket_->deleteLater();
        socket_ = nullptr;
    }
    info_.directoryStatus = ConnectionStatus::Disconnected;
    info_.directoryError.clear();
    setStatus(ConnectionStatus::Disconnected);
}

void WolfxSource::refreshDirectory() {
    if (!running_) return;
    pollDirectory();
}

void WolfxSource::connectSocket() {
    if (!running_) return;
    const QStringList& urls = WolfxProtocol::wsUrls();
    lastUrl_ = urls.at(urlIndex_ % urls.size());
    const quint64 connectingGeneration = generation_;
    setStatus(ConnectionStatus::Connecting);
    if (!running_ || connectingGeneration != generation_) return;
    qInfo() << "[wolfx] ws connecting:" << lastUrl_;
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

void WolfxSource::attachSocketHandlers(QWebSocket* socket, quint64 generation,
                                       quint64 socketAttempt) {
    // socketAttempt 令牌 + socket_ 指针双保险：过期尝试的迟到回调在此被丢弃。
    const auto current = [this, socket, generation, socketAttempt]() {
        return running_ && generation == generation_ && socketAttempt == attempt_
            && socket == socket_;
    };
    connect(socket, &QWebSocket::connected, this, [this, current]() {
        if (!current()) return;
        retryCount_ = 0;
        reconnectTimer_.stop();
        info_.lastHeartbeat = nowMs();
        setStatus(ConnectionStatus::Connected);
        if (!current()) return;
        sendQueries();
        queryTimer_.start(WolfxProtocol::kQueryIntervalMs);
    });
    // 传输失败时 QWebSocket 会同时发 errorOccurred 与 disconnected；两者都通过守卫后
    // 先 ++attempt_ 退休本次尝试，后到的那个便会被 current() 挡掉，保证只调度一次重连。
    connect(socket, &QWebSocket::disconnected, this, [this, current]() {
        if (!current()) return;
        ++attempt_;
        queryTimer_.stop();
        setStatus(ConnectionStatus::Disconnected);
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
        ++urlIndex_;
        queryTimer_.stop();
        setStatus(ConnectionStatus::Error,
                  QStringLiteral("WS 连接失败：%1").arg(socket->errorString()));
        scheduleReconnect();
    });
    connect(socket, &QWebSocket::pong, this,
            [this, current](quint64 elapsed, const QByteArray&) {
        if (!current()) return;
        info_.latencyMs = static_cast<qint64>(elapsed);
        info_.lastHeartbeat = nowMs();
        emit infoChanged();
    });
}

void WolfxSource::scheduleReconnect() {
    if (!running_) return;
    const int delayMs = std::min(15, std::max(3, 3 + retryCount_)) * 1000;
    ++retryCount_;
    qInfo() << "[wolfx] reconnect in" << delayMs << "ms (retry" << retryCount_ << ")";
    reconnectTimer_.start(delayMs);
}

void WolfxSource::sendQueries() {
    if (!running_ || !socket_ || socket_->state() != QAbstractSocket::ConnectedState) return;
    socket_->ping();
    for (const QString& q : WolfxProtocol::queries()) socket_->sendTextMessage(q);
}

void WolfxSource::handleMessage(const QString& text) {
    if (!running_) return;
    const quint64 generation = generation_;
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
    if (doc.isArray()) {
        for (const auto& v : doc.array()) {
            if (!running_ || generation != generation_) return;
            handleJsonObject(v.toObject());
        }
    } else if (doc.isObject()) {
        handleJsonObject(doc.object());
    }
}

void WolfxSource::handleJsonObject(const QJsonObject& obj) {
    if (!running_ || !socket_) return;
    const quint64 generation = generation_;
    info_.lastHeartbeat = nowMs();
    emit infoChanged();
    if (!running_ || generation != generation_ || !socket_) return;
    const QString type = obj.value("type").toString();
    if (type == QLatin1String("heartbeat")) {
        socket_->sendTextMessage(
            QStringLiteral("{\"type\":\"pong\",\"timestamp\":%1}").arg(nowMs()));
        return;
    }
    if (type == QLatin1String("pong")) return;

    const QString resolved = type.isEmpty() ? QStringLiteral("cwa_eew") : type;
    if (!WolfxProtocol::isEewType(resolved)) return;

    // 无定位时照常解析，只是距离/烈度/走时为未知。
    auto event = EewParser::parse(obj, userLocation(), standard_,
                                  WolfxProtocol::titleFor(resolved), QStringLiteral("wolfx_"),
                                  nowMs());
    if (event) {
        // Wolfx 连上后会把「最近一次」EEW 回放回来（可能是几小时前的），不能当作实时预警。
        const long long now = nowMs();
        if (event->timestamp > 0 && now - event->timestamp > WolfxProtocol::kEewLiveWindowMs) {
            qInfo() << "[wolfx] drop stale eew" << QString::fromStdString(event->id) << "age(s)"
                    << (now - event->timestamp) / 1000;
            return;
        }
        stampSource(*event, WolfxProtocol::providerName(), WolfxProtocol::agencyFor(resolved));
        emit eventReceived(*event, WolfxEventKind::Eew);
    }
}

void WolfxSource::pollDirectory() {
    // 地震列表完全由本 HTTP 轮询填充；WS 只负责实时预警。无定位也照常拉取（仅距离/烈度为未知）。
    if (!running_ || info_.directoryStatus == ConnectionStatus::Connecting) return;
    const quint64 generation = generation_;
    info_.directoryStatus = ConnectionStatus::Connecting;
    info_.directoryError.clear();
    emit infoChanged();
    if (!running_ || generation != generation_) return;
    QNetworkRequest request{QUrl(WolfxProtocol::eqListUrl())};
    request.setTransferTimeout(15000);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("komiraquake/2.0"));
    QNetworkReply* reply = network_.get(request);
    const qint64 started = monoMs();
    connect(reply, &QNetworkReply::finished, this, [this, reply, started, generation]() {
        reply->deleteLater();
        if (!running_ || generation != generation_) return;
        if (reply->error() != QNetworkReply::NoError) {
            const QString message = reply->errorString();
            qWarning() << "[wolfx] directory request failed:" << message;
            info_.directoryStatus = ConnectionStatus::Error;
            info_.directoryError = QStringLiteral("目录请求失败：%1").arg(message);
            emit infoChanged();
            return;
        }
        const QByteArray body = reply->readAll();
        const qint64 latency = monoMs() - started;

        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (!doc.isObject()) {
            info_.directoryStatus = ConnectionStatus::Error;
            info_.directoryError = QStringLiteral("目录响应格式无效");
            emit infoChanged();
            return;
        }
        const QJsonObject root = doc.object();
        for (auto it = root.begin(); it != root.end(); ++it) {
            if (!running_ || generation != generation_) return;
            if (!it.key().startsWith(QLatin1String("No"))) continue;
            auto event = EewParser::parseCencDirectory(
                it.value().toObject(), userLocation(), standard_, nowMs());
            if (event) {
                stampSource(*event, WolfxProtocol::providerName(), WolfxProtocol::directoryAgency());
                emit eventReceived(*event, WolfxEventKind::Directory);
            }
        }
        if (!running_ || generation != generation_) return;
        info_.directoryLatencyMs = latency;
        info_.directoryLastSuccess = nowMs();
        info_.directoryStatus = ConnectionStatus::Connected;
        info_.directoryError.clear();
        emit infoChanged();
    });
}

void WolfxSource::setStatus(ConnectionStatus status, const QString& note) {
    info_.status = status;
    info_.description = note.isEmpty()
        ? QStringLiteral("Wolfx all_eew 聚合（CENC/SC/JMA/CWA/FJ/CQ）") : note;
    if (status != ConnectionStatus::Connected) info_.latencyMs = -1;
    emit infoChanged();
}

} // namespace komira
