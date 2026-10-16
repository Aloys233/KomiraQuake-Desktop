#include "source/wolfx_seis_source.h"

#include <QDateTime>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <algorithm>

namespace komira {

WolfxSeisSource::WolfxSeisSource(QObject* parent) : QObject(parent) {
    reconnectTimer_.setSingleShot(true);
    connect(&reconnectTimer_, &QTimer::timeout, this, &WolfxSeisSource::connectSocket);

    heartbeatWatch_.setSingleShot(true);
    connect(&heartbeatWatch_, &QTimer::timeout, this, [this]() {
        if (!running_) return;
        // 退休本次尝试，避免随后到达的 disconnected 再次调度重连。
        ++attempt_;
        if (socket_) socket_->abort();
        scheduleReconnect();
    });

    listTimer_.setInterval(WolfxSeisProtocol::kListRefreshMs);
    connect(&listTimer_, &QTimer::timeout, this, &WolfxSeisSource::fetchStationList);

    expiryTimer_.setInterval(WolfxSeisProtocol::kExpiryCheckMs);
    connect(&expiryTimer_, &QTimer::timeout, this, &WolfxSeisSource::expireStale);
}

void WolfxSeisSource::start() {
    if (running_) return;
    running_ = true;
    ++generation_;
    retryCount_ = 0;
    connectSocket();
    // 名录先行：没有它也能靠实时帧自描述展示，但列表给出完整测站网与正式展示名。
    fetchStationList();
    listTimer_.start();
    expiryTimer_.start();
}

void WolfxSeisSource::stop() {
    running_ = false;
    ++generation_;
    ++attempt_;
    reconnectTimer_.stop();
    heartbeatWatch_.stop();
    listTimer_.stop();
    expiryTimer_.stop();
    if (socket_) {
        socket_->disconnect(this);
        socket_->abort();
        socket_->deleteLater();
        socket_ = nullptr;
    }
    if (connected_) {
        connected_ = false;
        emit statusChanged();
    }
}

void WolfxSeisSource::connectSocket() {
    if (!running_) return;
    const quint64 generation = generation_;
    if (socket_) {
        socket_->disconnect(this);
        socket_->abort();
        socket_->deleteLater();
        socket_ = nullptr;
    }
    auto* socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    socket_ = socket;
    attachHandlers(socket, generation, ++attempt_);
    // 该通道免鉴权：直接开连，不附加 token、不换票。
    socket->open(QUrl(QString::fromUtf8(WolfxSeisProtocol::kWsUrl)));
}

void WolfxSeisSource::attachHandlers(QWebSocket* socket, quint64 generation, quint64 attempt) {
    const auto current = [this, socket, generation, attempt]() {
        return running_ && generation == generation_ && attempt == attempt_ && socket == socket_;
    };

    connect(socket, &QWebSocket::connected, this, [this, current]() {
        if (!current()) return;
        retryCount_ = 0;
        reconnectTimer_.stop();
        heartbeatWatch_.start(WolfxSeisProtocol::kHeartbeatTimeoutMs);
        if (!connected_) {
            connected_ = true;
            qInfo() << "[wolfx-seis] station stream connected";
            emit statusChanged();
        }
    });

    connect(socket, &QWebSocket::disconnected, this, [this, current]() {
        if (!current()) return;
        ++attempt_;
        heartbeatWatch_.stop();
        if (connected_) {
            connected_ = false;
            emit statusChanged();
        }
        scheduleReconnect();
    });

    connect(socket, &QWebSocket::textMessageReceived, this, [this, current](const QString& text) {
        if (current()) handleMessage(text);
    });

    connect(socket, &QWebSocket::errorOccurred, this, [this, current]() {
        if (!current()) return;
        ++attempt_;
        heartbeatWatch_.stop();
        if (connected_) {
            connected_ = false;
            emit statusChanged();
        }
        scheduleReconnect();
    });
}

void WolfxSeisSource::scheduleReconnect() {
    if (!running_) return;
    // 与 Kmoni / Jian 同款退避：3s 起，逐步升到 15s。
    const int delayMs = std::min(15, std::max(3, 3 + retryCount_)) * 1000;
    ++retryCount_;
    reconnectTimer_.start(delayMs);
}

void WolfxSeisSource::handleMessage(const QString& text) {
    if (!running_) return;
    heartbeatWatch_.start(WolfxSeisProtocol::kHeartbeatTimeoutMs);

    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
    if (!doc.isObject()) return;
    const QJsonObject obj = doc.object();

    // 心跳：回 ping 维持连接（文档推荐），本身不含业务数据。
    if (WolfxSeisParser::isHeartbeat(obj)) {
        if (socket_) socket_->sendTextMessage(QString::fromUtf8(WolfxSeisProtocol::kPingPayload));
        return;
    }
    if (WolfxSeisParser::isPong(obj)) return;

    const auto parsed = WolfxSeisParser::parseStation(obj);
    if (!parsed) return;
    const WolfxSeisLiveFrame& live = *parsed;
    if (!live.updatedAt.isEmpty()) frameTime_ = live.updatedAt;
    seen_.insert(live.id, QDateTime::currentMSecsSinceEpoch());

    const auto found = index_.constFind(live.id);
    if (found == index_.constEnd()) {
        // 列表还没拉到（或这是新增测站）：先用帧自描述的位置/坐标建立。
        WolfxSeisStation station;
        station.id = live.id;
        station.location = live.region;
        station.latitude = live.latitude;
        station.longitude = live.longitude;
        station.calcShindo = live.calcShindo;
        station.csis = live.csis;
        station.updatedAt = live.updatedAt;
        index_.insert(live.id, stations_.size());
        stations_.push_back(station);
        emit stationsChanged();
        return;
    }
    WolfxSeisStation& station = stations_[*found];
    station.calcShindo = live.calcShindo;
    station.csis = live.csis;
    station.updatedAt = live.updatedAt;
    if (station.location.isEmpty()) station.location = live.region;
    emit dataChanged();
}

void WolfxSeisSource::fetchStationList() {
    if (!running_) return;
    QNetworkRequest request{QUrl(QString::fromUtf8(WolfxSeisProtocol::kListUrl))};
    request.setTransferTimeout(15000);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("komiraquake/2.0"));
    QNetworkReply* reply = network_.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (!running_) return;
        if (reply->error() != QNetworkReply::NoError) {
            qInfo() << "[wolfx-seis] station list failed:" << reply->errorString();
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject()) return;
        applyCatalog(WolfxSeisParser::parseCatalog(doc.object()));
    });
}

void WolfxSeisSource::applyCatalog(const QVector<WolfxSeisCatalogStation>& catalog) {
    bool changed = false;
    for (const WolfxSeisCatalogStation& entry : catalog) {
        if (!entry.enabled) continue;
        const auto found = index_.constFind(entry.id);
        if (found == index_.constEnd()) {
            WolfxSeisStation station;
            station.id = entry.id;
            station.location = entry.location;
            station.latitude = entry.latitude;
            station.longitude = entry.longitude;
            index_.insert(entry.id, stations_.size());
            stations_.push_back(station);
            changed = true;
            continue;
        }
        // 列表是名录/坐标的权威来源；实时帧可能已先建立该站，这里补全展示名与坐标。
        WolfxSeisStation& station = stations_[*found];
        if (!entry.location.isEmpty() && station.location != entry.location) {
            station.location = entry.location;
            changed = true;
        }
        if (station.latitude != entry.latitude || station.longitude != entry.longitude) {
            station.latitude = entry.latitude;
            station.longitude = entry.longitude;
            changed = true;
        }
    }
    if (changed) emit stationsChanged();
}

void WolfxSeisSource::expireStale() {
    if (!running_) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool changed = false;
    for (WolfxSeisStation& station : stations_) {
        // 两项读数都已缺测的站无需再回落。
        if (station.csis < WolfxSeisProtocol::kMissingIntensity
            && station.calcShindo <= WolfxSeisProtocol::kMissingShindo) {
            continue;
        }
        const qint64 seen = seen_.value(station.id, 0);
        if (seen > 0 && now - seen > WolfxSeisProtocol::kLiveTtlMs) {
            station.csis = WolfxSeisProtocol::kMissingIntensity;
            station.calcShindo = WolfxSeisProtocol::kMissingShindo;
            changed = true;
        }
    }
    if (changed) emit dataChanged();
}

double WolfxSeisSource::maxShindo() const {
    double best = WolfxSeisProtocol::kMissingShindo;
    for (const WolfxSeisStation& station : stations_) {
        if (station.calcShindo > best) best = station.calcShindo;
    }
    return best;
}

int WolfxSeisSource::maxCsis() const {
    int best = WolfxSeisProtocol::kMissingIntensity;
    for (const WolfxSeisStation& station : stations_) {
        if (station.csis > best) best = station.csis;
    }
    return best;
}

} // namespace komira
