#include "service/ntp_clock.h"

#include <QDateTime>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <cstdlib>

namespace komira {

namespace {

constexpr long long kTimeoutMs = 5000;
constexpr long long kInitialDelayMs = 2000;
constexpr long long kNormalIntervalMs = 10LL * 60LL * 1000LL;
constexpr long long kFastRetryMs = 60LL * 1000LL;
constexpr long long kOffsetFastThresholdMs = 3000;
constexpr int kSamplesPerServer = 2;
constexpr int kMaxHosts = 3;
constexpr int kTargetSamples = 3;

/// 内置授时源顺序。《NATIVE_PORT_SPEC》 §13.2。
const QStringList& builtinSntpHosts() {
    static const QStringList hosts = {
        QStringLiteral("ntp.aliyun.com"),
        QStringLiteral("ntp1.aliyun.com"),
        QStringLiteral("ntp.tencent.com"),
        QStringLiteral("pool.ntp.org"),
        QStringLiteral("time.apple.com"),
    };
    return hosts;
}

/// 规范化用户输入的自定义主机名：去空白与 `ntp://` 前缀 / 结尾斜杠。
QString normalizeHost(const QString& raw) {
    QString host = raw.trimmed();
    if (host.startsWith(QStringLiteral("ntp://"), Qt::CaseInsensitive))
        host = host.mid(6);
    while (host.endsWith(QLatin1Char('/'))) host.chop(1);
    return host.trimmed();
}

struct HttpSource {
    const char* url;
    const char* label;
    const char* field;
};

const std::vector<HttpSource>& httpSources() {
    static const std::vector<HttpSource> sources = {
        {"https://api.wolfx.jp/ntp.json", "api.wolfx.jp", "timestamp"},
    };
    return sources;
}

QString stateLabel(ClockState state) {
    switch (state) {
    case ClockState::Synced: return QStringLiteral("已校准");
    case ClockState::Stale: return QStringLiteral("校准过期");
    case ClockState::Local: break;
    }
    return QStringLiteral("本地时钟");
}

} // namespace

NtpClock::NtpClock(QObject* parent) : QObject(parent) {
    scheduleTimer_.setSingleShot(true);
    queryTimer_.setSingleShot(true);
    httpTimer_.setSingleShot(true);

    connect(&scheduleTimer_, &QTimer::timeout, this, [this]() {
        if (!running_) return;
        if (!enabled_) {
            scheduleNext(kNormalIntervalMs);
            return;
        }
        beginCalibration();
    });
    connect(&udp_, &QUdpSocket::readyRead, this, &NtpClock::onUdpReadyRead);
    connect(&queryTimer_, &QTimer::timeout, this, &NtpClock::onQueryTimeout);
    connect(&httpTimer_, &QTimer::timeout, this, &NtpClock::onHttpTimeout);
}

void NtpClock::setEnabled(bool enabled) {
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    if (enabled) {
        // 重新启用：尽快校准一次，而不是等周期到点。
        if (running_) scheduleNext(kInitialDelayMs);
    } else {
        clock_.reset();
        queryTimer_.stop();
        httpTimer_.stop();
        if (httpReply_) {
            QNetworkReply* reply = httpReply_;
            httpReply_ = nullptr;
            reply->abort();
            reply->deleteLater();
        }
        // 保持调度存活（禁用期间每次到点只跳过）：否则校准中途被关闭会让循环彻底停摆，
        // 之后再启用就永远不校准了。
        if (running_) scheduleNext(kNormalIntervalMs);
    }
    emit changed();
}

void NtpClock::setCustomServer(const QString& host) {
    const QString normalized = normalizeHost(host);
    if (customServer_ == normalized) return;
    customServer_ = normalized;
    // 启用且正在运行时，立即按新的主机顺序重校一次。
    if (running_ && enabled_) refresh();
}

QStringList NtpClock::sntpHosts() const {
    QStringList hosts;
    if (!customServer_.isEmpty()) hosts << customServer_;
    for (const QString& host : builtinSntpHosts())
        if (!hosts.contains(host)) hosts << host;
    return hosts;
}

QVariantMap NtpClock::info() const {
    QVariantMap map;
    map["enabled"] = enabled_;
    // 供 QML 直接判断字体色：只有真正在校准有效期内才算"已同步"。《NATIVE_PORT_SPEC》 §9.2。
    map["synced"] = enabled_ && clock_.state() == ClockState::Synced;
    map["state"] = enabled_ ? stateLabel(clock_.state()) : QStringLiteral("已关闭");
    if (!enabled_) {
        map["detail"] = QStringLiteral("使用系统本地时钟");
    } else if (clock_.sourceLabel().empty()) {
        map["detail"] = QStringLiteral("等待 SNTP / HTTP 授时");
    } else {
        map["detail"] = QStringLiteral("%1 · 偏差 %2 ms · 往返 %3 ms")
                            .arg(QString::fromStdString(clock_.sourceLabel()))
                            .arg(static_cast<qlonglong>(clock_.currentOffsetMs()))
                            .arg(static_cast<qlonglong>(clock_.lastDelayMs()));
    }
    return map;
}

void NtpClock::start() {
    if (running_) return;
    running_ = true;
    scheduleNext(kInitialDelayMs);
}

void NtpClock::stop() {
    running_ = false;
    scheduleTimer_.stop();
    queryTimer_.stop();
    httpTimer_.stop();
    udp_.close();
}

void NtpClock::refresh() {
    if (!enabled_) return;
    // 正在校准时不必插队，避免并发查询互相干扰。
    if (queryTimer_.isActive() || httpTimer_.isActive() || httpReply_) return;
    scheduleTimer_.stop();
    beginCalibration();
}

void NtpClock::scheduleNext(long long delayMs) {
    scheduleTimer_.start(static_cast<int>(delayMs));
}

void NtpClock::beginCalibration() {
    samples_.clear();
    hostIndex_ = 0;
    attemptIndex_ = 0;
    querySntp();
}

void NtpClock::querySntp() {
    if (samples_.size() >= static_cast<size_t>(kTargetSamples) || hostIndex_ >= kMaxHosts) {
        const auto best = ntp::chooseBest(samples_);
        if (best) {
            finishWith(*best, QStringLiteral("SNTP %1").arg(lastHost_));
            return;
        }
        beginHttp();
        return;
    }
    if (attemptIndex_ >= kSamplesPerServer) {
        ++hostIndex_;
        attemptIndex_ = 0;
        querySntp();
        return;
    }
    pendingHost_ = sntpHosts().at(hostIndex_);
    QHostInfo::lookupHost(pendingHost_, this, &NtpClock::onHostResolved);
}

void NtpClock::onHostResolved(const QHostInfo& info) {
    if (!running_ || !enabled_) return;
    if (info.error() != QHostInfo::NoError || info.addresses().isEmpty()) {
        advanceSntp();
        return;
    }
    const QHostAddress address = info.addresses().first();
    // T1/T4 一律用**本地原始墙钟**，故 offset 恒为 serverTime − localWall。
    pendingT1_ = QDateTime::currentMSecsSinceEpoch();
    const auto packet = ntp::buildRequest(pendingT1_);
    udp_.writeDatagram(reinterpret_cast<const char*>(packet.data()), ntp::kPacketSize, address,
                       static_cast<quint16>(ntp::kPort));
    lastHost_ = pendingHost_;
    queryTimer_.start(static_cast<int>(kTimeoutMs));
}

void NtpClock::onUdpReadyRead() {
    if (!queryTimer_.isActive()) return;
    while (udp_.hasPendingDatagrams()) {
        QByteArray buffer;
        buffer.resize(static_cast<int>(udp_.pendingDatagramSize()));
        udp_.readDatagram(buffer.data(), buffer.size());
        const long long t4 = QDateTime::currentMSecsSinceEpoch();
        const auto response = ntp::parseResponse(
            reinterpret_cast<const uint8_t*>(buffer.constData()), buffer.size());
        if (response) samples_.push_back(ntp::computeSample(pendingT1_, *response, t4));
    }
    queryTimer_.stop();
    advanceSntp();
}

void NtpClock::onQueryTimeout() { advanceSntp(); }

void NtpClock::advanceSntp() {
    queryTimer_.stop();
    ++attemptIndex_;
    querySntp();
}

void NtpClock::beginHttp() {
    httpIndex_ = 0;
    httpGet();
}

void NtpClock::httpGet() {
    if (httpIndex_ >= static_cast<int>(httpSources().size())) {
        finishFailed();
        return;
    }
    const HttpSource& source = httpSources().at(static_cast<size_t>(httpIndex_));
    httpMonoStart_ = clock_.elapsedMs();
    httpWallStart_ = QDateTime::currentMSecsSinceEpoch();

    QNetworkRequest request{QUrl(QString::fromLatin1(source.url))};
    request.setRawHeader("Cache-Control", "no-cache");
    httpReply_ = network_.get(request);
    connect(httpReply_, &QNetworkReply::finished, this, &NtpClock::onHttpFinished);
    httpTimer_.start(static_cast<int>(kTimeoutMs));
}

void NtpClock::onHttpTimeout() {
    if (!httpReply_) return;
    QNetworkReply* reply = httpReply_;
    httpReply_ = nullptr;
    reply->abort();
    reply->deleteLater();
    ++httpIndex_;
    httpGet();
}

void NtpClock::onHttpFinished() {
    httpTimer_.stop();
    QNetworkReply* reply = httpReply_;
    httpReply_ = nullptr;
    if (!reply) return;
    reply->deleteLater();

    const size_t index = static_cast<size_t>(httpIndex_);
    const long long wallEnd = QDateTime::currentMSecsSinceEpoch();
    // RTT 用单调时钟量测，不受系统时间影响。
    const long long roundTrip = clock_.elapsedMs() - httpMonoStart_;

    if (reply->error() != QNetworkReply::NoError) {
        ++httpIndex_;
        httpGet();
        return;
    }
    const QJsonObject object = QJsonDocument::fromJson(reply->readAll()).object();
    const QJsonValue value = object.value(QString::fromLatin1(httpSources().at(index).field));
    if (!value.isDouble()) {
        ++httpIndex_;
        httpGet();
        return;
    }
    const ntp::Sample sample = ntp::httpSample(
        static_cast<long long>(value.toDouble()), httpWallStart_, wallEnd, roundTrip);
    if (!sample.valid() || sample.delayMs > ntp::kMaxDelayMs) {
        ++httpIndex_;
        httpGet();
        return;
    }
    finishWith(sample,
               QStringLiteral("HTTP %1").arg(QString::fromLatin1(httpSources().at(index).label)));
}

void NtpClock::finishWith(const ntp::Sample& sample, const QString& label) {
    clock_.applyOffset(sample.offsetMs, sample.delayMs, label.toStdString());
    qInfo() << "[ntp] synced via" << label << "offset(ms)" << sample.offsetMs
            << "rtt(ms)" << sample.delayMs;
    emit changed();
    const long long next =
        std::llabs(sample.offsetMs) > kOffsetFastThresholdMs ? kFastRetryMs : kNormalIntervalMs;
    scheduleNext(next);
}

void NtpClock::finishFailed() {
    // 全部失败：保留上次锚定值（通常仍优于错误的系统钟），仅刷新状态供 UI 显示。
    qWarning() << "[ntp] calibration failed on all sources; keeping last anchor if any";
    emit changed();
    scheduleNext(kFastRetryMs);
}

} // namespace komira
