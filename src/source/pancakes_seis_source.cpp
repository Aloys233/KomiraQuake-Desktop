#include "source/pancakes_seis_source.h"

#include <QDateTime>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

#include <algorithm>
#include <utility>

#include "source/pancakes_seis_protocol.h"

namespace komira {

PancakesSeisSource::PancakesSeisSource(QObject* parent) : QObject(parent) {
    reconnectTimer_.setSingleShot(true);
    connect(&reconnectTimer_, &QTimer::timeout, this, &PancakesSeisSource::connectSocket);

    heartbeatWatch_.setSingleShot(true);
    connect(&heartbeatWatch_, &QTimer::timeout, this, [this]() {
        if (!running_) return;
        // 退休本次尝试，避免随后到达的 disconnected 再次调度重连。
        ++attempt_;
        if (socket_) socket_->abort();
        scheduleReconnect();
    });

    expiryTimer_.setInterval(PancakesSeisProtocol::kExpiryCheckMs);
    connect(&expiryTimer_, &QTimer::timeout, this, &PancakesSeisSource::expireStale);
}

void PancakesSeisSource::start() {
    if (running_) return;
    running_ = true;
    ++generation_;
    retryCount_ = 0;
    connectSocket();
    expiryTimer_.start();
}

void PancakesSeisSource::stop() {
    running_ = false;
    ++generation_;
    ++attempt_;
    reconnectTimer_.stop();
    heartbeatWatch_.stop();
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

void PancakesSeisSource::connectSocket() {
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
    socket->open(QUrl(QString::fromUtf8(PancakesSeisProtocol::kWsUrl)));
}

void PancakesSeisSource::attachHandlers(QWebSocket* socket, quint64 generation, quint64 attempt) {
    const auto current = [this, socket, generation, attempt]() {
        return running_ && generation == generation_ && attempt == attempt_ && socket == socket_;
    };

    connect(socket, &QWebSocket::connected, this, [this, current]() {
        if (!current()) return;
        retryCount_ = 0;
        reconnectTimer_.stop();
        heartbeatWatch_.start(PancakesSeisProtocol::kHeartbeatTimeoutMs);
        if (!connected_) {
            connected_ = true;
            qInfo() << "[seis] station stream connected";
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

void PancakesSeisSource::scheduleReconnect() {
    if (!running_) return;
    // 与 Kmoni / Jian 同款退避：3s 起，逐步升到 15s。
    const int delayMs = std::min(15, std::max(3, 3 + retryCount_)) * 1000;
    ++retryCount_;
    reconnectTimer_.start(delayMs);
}

void PancakesSeisSource::handleMessage(const QString& text) {
    if (!running_) return;
    heartbeatWatch_.start(PancakesSeisProtocol::kHeartbeatTimeoutMs);

    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
    if (!doc.isObject()) return;
    const QJsonObject obj = doc.object();

    // 测站档案：连接建立时一次，此后测站上下线/改动会重发全量列表。
    if (PancakesSeisParser::isStationFrame(obj)) {
        if (auto parsed = PancakesSeisParser::parseStations(obj)) applyStations(std::move(*parsed));
        return;
    }

    // 实时帧：`s` 为档案里的 sid（不是数组下标）。
    const auto sample = PancakesSeisParser::parseSample(obj);
    if (!sample || !hasData()) return;
    // 健康码非 0 的读数是坏数据：必须整站整秒剔除，不能拿来做着色/统计。
    if (sample->errorCode != 0) {
        const auto stale = indexBySid_.constFind(sample->sid);
        const bool hadValue =
            stale != indexBySid_.constEnd()
            && intensity_[*stale] > PancakesSeisProtocol::kMissingIntensity;
        if (hadValue) {
            intensity_[*stale] = PancakesSeisProtocol::kMissingIntensity;
            seenAt_.remove(sample->sid);
            emit dataChanged();
        }
        return;
    }
    const auto found = indexBySid_.constFind(sample->sid);
    if (found == indexBySid_.constEnd()) return;   // 档案里没有的站（竞态/已下线）
    intensity_[*found] = sample->intensity;
    seenAt_.insert(sample->sid, QDateTime::currentMSecsSinceEpoch());
    if (sample->timestamp > 0) lastTimestamp_ = sample->timestamp;
    emit dataChanged();
}

void PancakesSeisSource::applyStations(QVector<PancakesSeisStation> stations) {
    // 档案是全量权威来源：按新表的 sid 重建对齐的烈度数组。仍在线的站保留旧读数，
    // 避免这一刻整网瞬时回落缺测。用「sid+坐标+名称」判断表是否真有变化，没有就
    // 不重建、不发信号（上游每次上下线都会重发全量，内容常常完全一样）。
    std::vector<double> keep(stations.size(), PancakesSeisProtocol::kMissingIntensity);
    for (int i = 0; i < stations.size(); ++i) {
        const auto old = indexBySid_.constFind(stations[i].sid);
        if (old == indexBySid_.constEnd()) continue;
        const double value = intensity_[*old];
        if (value > PancakesSeisProtocol::kMissingIntensity) keep[i] = value;
    }

    const bool same = stations.size() == stations_.size()
                      && std::equal(stations.cbegin(), stations.cend(), stations_.cbegin(),
                                    [](const PancakesSeisStation& a, const PancakesSeisStation& b) {
                                        return a.sid == b.sid && a.name == b.name
                                               && a.latitude == b.latitude
                                               && a.longitude == b.longitude
                                               && a.altitude == b.altitude
                                               && a.region == b.region;
                                    });

    indexBySid_.clear();
    stations_ = std::move(stations);
    intensity_.clear();
    intensity_.reserve(stations_.size());
    for (int i = 0; i < stations_.size(); ++i) {
        indexBySid_.insert(stations_[i].sid, i);
        intensity_.push_back(keep[static_cast<std::size_t>(i)]);
    }
    // 下线测站的记录一并清掉，不让 seenAt_ 无限增长。
    for (auto it = seenAt_.begin(); it != seenAt_.end();) {
        if (!indexBySid_.contains(it.key())) it = seenAt_.erase(it);
        else ++it;
    }

    if (same) return;
    qInfo() << "[seis] stations" << stations_.size();
    emit stationsChanged();
}

void PancakesSeisSource::expireStale() {
    if (!running_) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool changed = false;
    for (int i = 0; i < stations_.size(); ++i) {
        if (intensity_[i] <= PancakesSeisProtocol::kMissingIntensity) continue;
        const qint64 seen = seenAt_.value(stations_[i].sid, 0);
        // 有读数却没有记录时刻（理论上不该发生）也按静默处理，避免旧值永久残留。
        if (seen <= 0 || now - seen > PancakesSeisProtocol::kLiveTtlMs) {
            intensity_[i] = PancakesSeisProtocol::kMissingIntensity;
            seenAt_.remove(stations_[i].sid);
            changed = true;
        }
    }
    if (changed) emit dataChanged();
}

double PancakesSeisSource::maxIntensity() const {
    double best = PancakesSeisProtocol::kMissingIntensity;
    // std::as_const：这些访问器是只读的，非 const 迭代会让 QVector 触发 detach 检查。
    for (double value : std::as_const(intensity_)) {
        if (value > best) best = value;
    }
    return best;
}

} // namespace komira
