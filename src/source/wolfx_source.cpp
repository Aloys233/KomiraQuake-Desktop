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
#include <vector>

#include "source/eew_parser.h"
#include "source/raw_payload.h"
#include "source/wolfx_protocol.h"
#include "source/ws_latency_probe.h"

namespace komira {

namespace {

/// 标明来源：列表卡片左下角显示 `<provider>·<agency>`；新增数据源时同样在此打标。
void stampSource(EarthquakeEvent& event, const QString& provider, const QString& agency) {
    event.sourceProvider = provider.toStdString();
    event.sourceAgency = agency.toStdString();
}

std::optional<EarthquakeEvent> parseCencEntry(const QJsonObject& obj, const EewParser::UserLocation& user,
                                              IntensityStandard standard, long long nowMs) {
    return EewParser::parseCencDirectory(obj, user, standard, nowMs);
}

std::optional<EarthquakeEvent> parseJmaEntry(const QJsonObject& obj, const EewParser::UserLocation& user,
                                             IntensityStandard standard, long long) {
    return EewParser::parseJmaDirectory(obj, user, standard);
}

std::optional<EarthquakeEvent> parseDirectoryEntry(bool jma, const QJsonObject& obj,
                                                   const EewParser::UserLocation& user,
                                                   IntensityStandard standard, long long nowMs) {
    return jma ? parseJmaEntry(obj, user, standard, nowMs)
               : parseCencEntry(obj, user, standard, nowMs);
}

/// 目录载荷的校验码。两个端点都把 `md5` 放在根对象上（与 `NoN` 平级）；
/// 内容没变时整帧跳过，可以省掉 50 条的重复解析与 50 次事件发射。
QString payloadMd5(const QJsonObject& root) {
    const QJsonValue value = root.value(QStringLiteral("md5"));
    return value.isString() ? value.toString() : QString();
}

const QString kDescription = QStringLiteral("Wolfx all_eew 聚合（CENC/SC/JMA/CWA/FJ/CQ + 目录推送）");

} // namespace

WolfxSource::WolfxSource(QObject* parent) : EarthquakeSource(parent) {
    info_.id = SourceIds::kWolfx;
    info_.name = QStringLiteral("Wolfx");
    info_.region = QStringLiteral("全球");
    info_.description = kDescription;

    connect(&queryTimer_, &QTimer::timeout, this, &WolfxSource::sendQueries);
    // 订阅队列的单发节拍：上游不接受并发查询，必须一条一条来。
    queryStaggerTimer_.setSingleShot(true);
    connect(&queryStaggerTimer_, &QTimer::timeout, this, &WolfxSource::flushQueryQueue);
    connect(&pollTimer_, &QTimer::timeout, this, &WolfxSource::pollDirectory);
    // 服务端不主动推目录变化（实测被动等 5 min 一帧未推），必须周期重发查询。
    connect(&directoryQueryTimer_, &QTimer::timeout, this, &WolfxSource::sendDirectoryQueries);
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
    // 目录查询的周期重问与「目录是否轮询」无关：即便 HTTP 兜底被关掉
    // （如线上用例），WS 主链路仍应继续刷新目录。
    directoryQueryTimer_.start(WolfxProtocol::kDirectoryQueryIntervalMs);
    if (directoryPollingEnabled_) {
        pollTimer_.start(WolfxProtocol::kPollIntervalMs);
        pollDirectory();
    }
}

void WolfxSource::stop() {
    running_ = false;
    ++generation_;
    ++directoryGeneration_;
    ++attempt_;
    queryTimer_.stop();
    directoryQueryTimer_.stop();
    queryStaggerTimer_.stop();
    pendingQueries_.clear();
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
    if (!running_ || !directoryPollingEnabled_) return;
    // 显式刷新：先借 WS 主链路要一帧（秒级），同时跑一次 HTTP 兜底，谁先回都能更新列表。
    // 清掉推送时刻是有意的：不清的话下面 pollDirectory 会因「刚推送过」把两路全跳过，
    // 而此刻推送其实还没到（查询才刚发出），显式刷新就变成了空操作。
    directoryPushAt_.clear();
    sendDirectoryQueries();
    pollDirectory();
}

void WolfxSource::setDirectoryPollingEnabled(bool enabled) {
    if (directoryPollingEnabled_ == enabled) return;
    directoryPollingEnabled_ = enabled;
    if (!enabled) {
        ++directoryGeneration_;
        pollTimer_.stop();
        if (info_.directoryStatus == ConnectionStatus::Connecting) {
            info_.directoryStatus = ConnectionStatus::Disconnected;
            info_.directoryError.clear();
            emit infoChanged();
        }
        return;
    }
    if (running_) pollTimer_.start(WolfxProtocol::kPollIntervalMs);
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
        // 连上后立刻要一轮全量：EEW 与目录一起订（上游会为每条查询回一帧「最近一次」）。
        sendQueries();
        sendDirectoryQueries();
        queryTimer_.start(WolfxProtocol::kQueryIntervalMs);
        directoryQueryTimer_.start(WolfxProtocol::kDirectoryQueryIntervalMs);
    });
    // 传输失败时 QWebSocket 会同时发 errorOccurred 与 disconnected；两者都通过守卫后
    // 先 ++attempt_ 退休本次尝试，后到的那个便会被 current() 挡掉，保证只调度一次重连。
    connect(socket, &QWebSocket::disconnected, this, [this, current]() {
        if (!current()) return;
        ++attempt_;
        queryTimer_.stop();
        directoryQueryTimer_.stop();
        queryStaggerTimer_.stop();
        pendingQueries_.clear();
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
        directoryQueryTimer_.stop();
        queryStaggerTimer_.stop();
        pendingQueries_.clear();
        setStatus(ConnectionStatus::Error,
                  QStringLiteral("WS 连接失败：%1").arg(socket->errorString()));
        scheduleReconnect();
    });
    // 链路延迟：协议级 ping/pong 探针（与其余四个事件源同一份实现）。不要再用
    // sendQueries 里的 ping —— 那一个只是为了触发上游心跳，量不到 RTT。
    attachLatencyProbe(socket, current, [this](qint64 latencyMs) {
        if (info_.latencyMs == latencyMs) return;
        info_.latencyMs = latencyMs;
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

void WolfxSource::enqueueQuery(const QString& query) {
    if (query.isEmpty()) return;
    if (pendingQueries_.contains(query)) return;
    pendingQueries_ << query;
    // 队列里已有待发项时不要插队：由 flushQueryQueue 自己按节拍继续。
    if (!queryStaggerTimer_.isActive()) queryStaggerTimer_.start(0);
}

void WolfxSource::flushQueryQueue() {
    if (!running_ || !socket_ || socket_->state() != QAbstractSocket::ConnectedState) {
        pendingQueries_.clear();
        queryStaggerTimer_.stop();
        return;
    }
    if (pendingQueries_.isEmpty()) {
        queryStaggerTimer_.stop();
        return;
    }
    const QString query = pendingQueries_.takeFirst();
    // 关闭的子源不再订阅：少发一条查询，上游也不会为该频道推流。
    const QString type = WolfxProtocol::typeForQuery(query);
    if (type.isEmpty() || isSubSourceEnabled(type)) socket_->sendTextMessage(query);
    if (!pendingQueries_.isEmpty()) queryStaggerTimer_.start(WolfxProtocol::kQueryStaggerMs);
    else queryStaggerTimer_.stop();
}

void WolfxSource::sendQueries() {
    if (!running_ || !socket_ || socket_->state() != QAbstractSocket::ConnectedState) return;
    // 不在这里 ping：延迟统一由 attachLatencyProbe 的独立探针测量。
    // 6 条 EEW 查询排进队列、按 1.2 s 串行发出（并发会被上游丢弃，只剩第一条回包）。
    for (const QString& q : WolfxProtocol::queries()) {
        if (WolfxProtocol::isDirectoryType(WolfxProtocol::typeForQuery(q))) continue;
        enqueueQuery(q);
    }
}

void WolfxSource::sendDirectoryQueries() {
    if (!running_ || !socket_ || socket_->state() != QAbstractSocket::ConnectedState) return;
    for (const auto& endpoint : WolfxProtocol::directoryEndpoints()) {
        if (!isSubSourceEnabled(endpoint.subId)) continue;
        enqueueQuery(endpoint.query);
    }
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

    // 目录帧：整份列表（每份 50 条），与 EEW 的单条报文是两种形态，必须分开处理。
    // 旧实现把它当成 EEW —— `isEewType` 返回 false 就 return，于是
    // `query_cenceqlist` / `query_jmaeqlist` 即使发了也全部被丢弃。
    if (WolfxProtocol::isDirectoryType(type)) {
        handleDirectoryFrame(type, obj, /*fromPush=*/true);
        return;
    }

    const QString resolved = type.isEmpty() ? QStringLiteral("cwa_eew") : type;
    if (!WolfxProtocol::isEewType(resolved)) return;
    if (!isSubSourceEnabled(resolved)) return;

    // 无定位时照常解析，只是距离/烈度/走时为未知。JMA 报文时刻为 JST，需按时区解析。
    // eventNamespace 用频道名（如 jma_eew），使 eventId 与其它聚合商的同名频道对齐合并。
    auto event = EewParser::parse(obj, userLocation(), standard_,
                                  WolfxProtocol::titleFor(resolved), QStringLiteral("wolfx_"),
                                  nowMs(), resolved == QLatin1String("jma_eew"), resolved);
    if (event) {
        // Wolfx 连上后会把「最近一次」EEW 回放回来（可能是几小时前的），不能当作实时预警。
        const long long now = nowMs();
        if (event->timestamp > 0 && now - event->timestamp > WolfxProtocol::kEewLiveWindowMs) {
            qInfo() << "[wolfx] drop stale eew" << QString::fromStdString(event->id) << "age(s)"
                    << (now - event->timestamp) / 1000;
            return;
        }
        stampSource(*event, WolfxProtocol::providerName(), WolfxProtocol::agencyFor(resolved));
        attachRawPayload(*event, obj);
        emit eventReceived(*event, SourceEventKind::Live);
    }
}

void WolfxSource::handleDirectoryFrame(const QString& type, const QJsonObject& root, bool fromPush) {
    if (!isSubSourceEnabled(type)) return;

    const auto& endpoints = WolfxProtocol::directoryEndpoints();
    auto spec = std::find_if(endpoints.cbegin(), endpoints.cend(),
                             [&type](const WolfxProtocol::DirectoryEndpointSpec& e) {
                                 return e.subId == type;
                             });
    if (spec == endpoints.cend()) return;

    // 只有 WS 帧才记「推送时刻」：它是 HTTP 兜底的跳过判据。HTTP 自己也记的话，
    // 下一次兜底就会因为「刚推送过」被跳过 —— 主链路已断时目录会彻底停更。
    if (fromPush) directoryPushAt_.insert(type, nowMs());

    // 校验码没变就是同一份列表：直接跳过，不必重新解析 50 条、发射 50 个事件。
    // 这一层对 WS 与 HTTP 都生效（两路载荷的 md5 是同一个算法、同一个值）。
    const QString md5 = payloadMd5(root);
    if (!md5.isEmpty() && directoryMd5_.value(type) == md5) return;
    if (!md5.isEmpty()) directoryMd5_.insert(type, md5);

    const long long now = nowMs();
    int emitted = 0;
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (!running_ || !isSubSourceEnabled(type)) return;
        if (!it.key().startsWith(QLatin1String("No"))) continue;
        const QJsonObject raw = it.value().toObject();
        if (raw.isEmpty()) continue;
        auto event = parseDirectoryEntry(spec->jma, raw, userLocation(), standard_, now);
        if (!event) continue;
        stampSource(*event, WolfxProtocol::providerName(), spec->agency);
        attachRawPayload(*event, raw);
        emit eventReceived(*event, SourceEventKind::Directory);
        ++emitted;
    }
    if (emitted > 0) {
        // WS 目录帧即目录主链路：状态按它置为健康，`directoryLastSuccess` 供设置页显示。
        info_.directoryLastSuccess = now;
        if (info_.directoryStatus != ConnectionStatus::Connected) {
            info_.directoryStatus = ConnectionStatus::Connected;
            info_.directoryError.clear();
            emit infoChanged();
        }
    }
    qInfo() << "[wolfx] directory frame" << type << "entries" << emitted << "md5" << md5;
}

bool WolfxSource::shouldPollDirectory(const QString& subId) const {
    // WS 目录帧在新鲜窗口内到过 → HTTP 兜底跳过。判据是「WS 主链路最近给过数据」，
    // 而不是「连接状态是 Connected」：WS 可能连着但目录查询从未成功（被上游限流等）。
    const long long last = directoryPushAt_.value(subId, 0);
    if (last <= 0) return true;
    return nowMs() - last > WolfxProtocol::kDirectoryPushFreshMs;
}

void WolfxSource::pollDirectory() {
    // HTTP 兜底：WS 没给出新鲜目录时才拉。无定位也照常拉取（仅距离/烈度为未知）。
    if (!running_ || !directoryPollingEnabled_
        || info_.directoryStatus == ConnectionStatus::Connecting) return;
    // 目录子源全部关闭时不再轮询：这属于用户的显式选择，不是链路异常。
    bool anyEnabled = false;
    for (const auto& endpoint : WolfxProtocol::directoryEndpoints()) {
        if (isSubSourceEnabled(endpoint.subId)) { anyEnabled = true; break; }
    }
    if (!anyEnabled) {
        ++directoryGeneration_;
        info_.directoryStatus = ConnectionStatus::Disconnected;
        info_.directoryError.clear();
        emit infoChanged();
        return;
    }
    const quint64 generation = ++directoryGeneration_;
    info_.directoryStatus = ConnectionStatus::Connecting;
    info_.directoryError.clear();
    emit infoChanged();
    if (!running_ || generation != directoryGeneration_) return;
    directoryIndex_ = 0;
    directoryFailed_ = false;
    directoryError_.clear();
    directoryCycleStart_ = monoMs();
    fetchDirectoryEndpoint(0);
}

void WolfxSource::fetchDirectoryEndpoint(int index) {
    if (!running_ || !directoryPollingEnabled_) return;
    const quint64 generation = directoryGeneration_;
    const auto& endpoints = WolfxProtocol::directoryEndpoints();
    if (index >= static_cast<int>(endpoints.size())) {
        // 周期结束：全部成功才算目录健康，否则记录首个失败端点。
        if (!running_ || !directoryPollingEnabled_ || generation != directoryGeneration_) return;
        if (directoryFailed_) {
            info_.directoryStatus = ConnectionStatus::Error;
            info_.directoryError = directoryError_;
        } else {
            info_.directoryLatencyMs = monoMs() - directoryCycleStart_;
            info_.directoryLastSuccess = nowMs();
            info_.directoryStatus = ConnectionStatus::Connected;
            info_.directoryError.clear();
        }
        emit infoChanged();
        return;
    }

    const WolfxProtocol::DirectoryEndpointSpec endpoint = endpoints.at(index);
    // 子源关闭、或 WS 主链路刚推过 → 这一路走 HTTP 没有意义，跳过（不能算失败）。
    if (!isSubSourceEnabled(endpoint.subId) || !shouldPollDirectory(endpoint.subId)) {
        fetchDirectoryEndpoint(index + 1);
        return;
    }
    QNetworkRequest request{QUrl(endpoint.url)};
    request.setTransferTimeout(15000);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("komiraquake/2.0"));
    QNetworkReply* reply = network_.get(request);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, endpoint, index, generation]() {
        reply->deleteLater();
        if (!running_ || !directoryPollingEnabled_ || generation != directoryGeneration_) return;

        const auto fail = [this](const QString& reason) {
            directoryFailed_ = true;
            if (directoryError_.isEmpty()) directoryError_ = reason;
        };

        if (reply->error() != QNetworkReply::NoError) {
            const QString message = reply->errorString();
            qWarning() << "[wolfx] directory request failed:" << endpoint.url << message;
            fail(QStringLiteral("目录请求失败：%1（%2）").arg(message, endpoint.url));
            fetchDirectoryEndpoint(index + 1);
            return;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject()) {
            fail(QStringLiteral("目录响应格式无效（%1）").arg(endpoint.url));
            fetchDirectoryEndpoint(index + 1);
            return;
        }
        // 与 WS 目录帧共用同一个入口：md5 短路、解析、来源标注、状态更新只有一份实现。
        handleDirectoryFrame(endpoint.subId, doc.object(), /*fromPush=*/false);
        fetchDirectoryEndpoint(index + 1);
    });
}

void WolfxSource::setStatus(ConnectionStatus status, const QString& note) {
    info_.status = status;
    info_.description = note.isEmpty() ? kDescription : note;
    if (status != ConnectionStatus::Connected) info_.latencyMs = -1;
    emit infoChanged();
}

} // namespace komira
