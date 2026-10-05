#include "source/pancakes_source.h"

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

#include "source/pancakes_protocol.h"

namespace komira {

PancakesSource::PancakesSource(QObject* parent) : QObject(parent) {
    info_.id = SourceIds::kPancakes;
    info_.name = QStringLiteral("Pancakes");
    info_.region = QStringLiteral("全球");
    info_.description = QStringLiteral("Pancakes 聚合（GQ/USGS/JMA）");

    connect(&pollTimer_, &QTimer::timeout, this, &PancakesSource::pollDirectory);
    reconnectTimer_.setSingleShot(true);
    connect(&reconnectTimer_, &QTimer::timeout, this, &PancakesSource::connectSocket);
}

void PancakesSource::setUserLocation(double lat, double lon) {
    userLat_ = lat;
    userLon_ = lon;
    hasLocation_ = true;
}

void PancakesSource::clearUserLocation() {
    hasLocation_ = false;
    userLat_ = 0.0;
    userLon_ = 0.0;
}

EewParser::UserLocation PancakesSource::userLocation() const {
    if (!hasLocation_) return std::nullopt;
    return std::make_pair(userLat_, userLon_);
}

long long PancakesSource::nowMs() const {
    return nowProvider_ ? nowProvider_() : QDateTime::currentMSecsSinceEpoch();
}

long long PancakesSource::monoMs() const {
    return monoProvider_ ? monoProvider_()
        : std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now().time_since_epoch()).count();
}

void PancakesSource::start() {
    if (running_) return;
    running_ = true;
    const quint64 generation = ++generation_;
    connectSocket();
    if (!running_ || generation != generation_) return;
    pollTimer_.start(PancakesProtocol::kPollIntervalMs);
    pollDirectory();
}

void PancakesSource::stop() {
    running_ = false;
    ++generation_;
    ++attempt_;
    pollTimer_.stop();
    reconnectTimer_.stop();
    pendingDirectory_ = 0;
    directoryError_.clear();
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

void PancakesSource::refreshDirectory() {
    if (!running_) return;
    pollDirectory();
}

void PancakesSource::connectSocket() {
    if (!running_) return;
    const quint64 connectingGeneration = generation_;
    setStatus(ConnectionStatus::Connecting);
    if (!running_ || connectingGeneration != generation_) return;
    qInfo() << "[pancakes] ws connecting:" << PancakesProtocol::wsUrl();
    if (socket_) {
        socket_->disconnect(this);
        socket_->abort();
        socket_->deleteLater();
    }
    auto* socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    socket_ = socket;
    attachSocketHandlers(socket, generation_, ++attempt_);
    socket->open(QUrl(PancakesProtocol::wsUrl()));
}

void PancakesSource::attachSocketHandlers(QWebSocket* socket, quint64 generation,
                                          quint64 socketAttempt) {
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
    });
    // 传输失败时 QWebSocket 会同时发 errorOccurred 与 disconnected；两者都通过守卫后
    // 先 ++attempt_ 退休本次尝试，后到的那个便会被 current() 挡掉，保证只调度一次重连。
    connect(socket, &QWebSocket::disconnected, this, [this, current]() {
        if (!current()) return;
        ++attempt_;
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
        setStatus(ConnectionStatus::Error,
                  QStringLiteral("WS 连接失败：%1").arg(socket->errorString()));
        scheduleReconnect();
    });
}

void PancakesSource::scheduleReconnect() {
    if (!running_) return;
    const int delayMs = std::min(15, std::max(3, 3 + retryCount_)) * 1000;
    ++retryCount_;
    qInfo() << "[pancakes] reconnect in" << delayMs << "ms (retry" << retryCount_ << ")";
    reconnectTimer_.start(delayMs);
}

void PancakesSource::handleMessage(const QString& text) {
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

void PancakesSource::handleJsonObject(const QJsonObject& obj) {
    if (!running_) return;
    info_.lastHeartbeat = nowMs();
    emit infoChanged();
    const QString type = obj.value("type").toString();
    if (type == QLatin1String("pong") || type == QLatin1String("heartbeat")) return;
    auto parsed = PancakesParser::parseRealtime(obj, userLocation(), standard_, nowMs());
    if (!parsed) return;

    const EarthquakeEvent& event = parsed->event;
    const long long now = nowMs();
    if (event.timestamp > 0 && now - event.timestamp > PancakesProtocol::kLiveWindowMs) return;
    emit eventReceived(event, parsed->kind);
}

void PancakesSource::pollDirectory() {
    if (!running_ || info_.directoryStatus == ConnectionStatus::Connecting) return;
    const quint64 generation = generation_;
    info_.directoryStatus = ConnectionStatus::Connecting;
    info_.directoryError.clear();
    directoryError_.clear();
    pendingDirectory_ = 0;
    emit infoChanged();
    if (!running_ || generation != generation_) return;
    for (const QString& source : PancakesProtocol::quakeSources()) {
        fetchList(source, generation);
    }
    if (pendingDirectory_ == 0) finishDirectoryRequest();
}

void PancakesSource::fetchList(const QString& source, quint64 generation) {
    QNetworkRequest request{QUrl(PancakesProtocol::listUrl(source))};
    request.setTransferTimeout(15000);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("komiraquake/2.0"));
    QNetworkReply* reply = network_.get(request);
    ++pendingDirectory_;
    const qint64 started = monoMs();
    connect(reply, &QNetworkReply::finished, this, [this, reply, started, generation]() {
        reply->deleteLater();
        if (!running_ || generation != generation_) return;
        if (reply->error() != QNetworkReply::NoError) {
            if (directoryError_.isEmpty()) {
                directoryError_ = QStringLiteral("目录请求失败：%1").arg(reply->errorString());
            }
            finishDirectoryRequest();
            return;
        }
        const QByteArray body = reply->readAll();
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (!doc.isArray()) {
            if (directoryError_.isEmpty()) directoryError_ = QStringLiteral("目录响应格式无效");
            finishDirectoryRequest();
            return;
        }
        const long long now = nowMs();
        for (const auto& v : doc.array()) {
            if (!running_ || generation != generation_) return;
            auto event = PancakesParser::parseListItem(v.toObject(), userLocation(), standard_, now);
            if (event) emit eventReceived(*event, PancakesKind::Directory);
        }
        info_.directoryLatencyMs = monoMs() - started;
        finishDirectoryRequest();
    });
}

void PancakesSource::finishDirectoryRequest() {
    if (pendingDirectory_ > 0) --pendingDirectory_;
    if (pendingDirectory_ > 0) return;
    if (directoryError_.isEmpty()) {
        info_.directoryStatus = ConnectionStatus::Connected;
        info_.directoryLastSuccess = nowMs();
        info_.directoryError.clear();
    } else {
        info_.directoryStatus = ConnectionStatus::Error;
        info_.directoryError = directoryError_;
    }
    emit infoChanged();
}

void PancakesSource::setStatus(ConnectionStatus status, const QString& note) {
    info_.status = status;
    info_.description = note.isEmpty()
        ? QStringLiteral("Pancakes 聚合（GQ/USGS/JMA）") : note;
    if (status != ConnectionStatus::Connected) info_.latencyMs = -1;
    emit infoChanged();
}

} // namespace komira
