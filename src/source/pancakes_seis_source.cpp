#include "source/pancakes_seis_source.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

#include <algorithm>

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
}

void PancakesSeisSource::start() {
    if (running_) return;
    running_ = true;
    ++generation_;
    retryCount_ = 0;
    connectSocket();
}

void PancakesSeisSource::stop() {
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

    // 首包（及重连后重发）的台站表：按 sid 对齐 PGA 数组，保留已知读数避免整网瞬时回落缺测。
    if (const auto parsed = PancakesSeisParser::parseStations(obj)) {
        stations_ = *parsed;
        QVector<double> next(stations_.size(), PancakesSeisProtocol::kMissingPga);
        for (int i = 0; i < stations_.size(); ++i) {
            const int sid = stations_[i].sid;
            if (sid >= 0 && sid < pga_.size()) next[i] = pga_[sid];
        }
        pga_ = next;
        qInfo() << "[seis] stations" << stations_.size();
        emit stationsChanged();
        return;
    }

    // 逐站加速度帧：`s` 为台站表下标。
    const auto sample = PancakesSeisParser::parseSample(obj);
    if (!sample || !hasData()) return;
    const int sid = sample->sid;
    if (sid < 0 || sid >= pga_.size()) return;
    pga_[sid] = sample->pga;
    if (sample->timestamp > 0) lastTimestamp_ = sample->timestamp;
    emit dataChanged();
}

double PancakesSeisSource::maxPga() const {
    double best = PancakesSeisProtocol::kMissingPga;
    for (double value : pga_) {
        if (value > best) best = value;
    }
    return best;
}

} // namespace komira
