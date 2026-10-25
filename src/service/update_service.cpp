#include "service/update_service.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QtGlobal>

#include "app/version.h"
#include "core/version_compare.h"
#include "prefs/settings_store.h"

namespace komira {

namespace {

const QString kRepositoryUrl = QStringLiteral("https://github.com/Aloys233/KomiraQuake-Desktop");
const char* kLatestReleaseApi =
    "https://api.github.com/repos/Aloys233/KomiraQuake-Desktop/releases/latest";

/// 单个候选的超时。直连在无出口的网络里通常是「连不上」快速失败，但被墙的表现
/// 往往是**静默丢包**（连接一直挂着直到超时），所以这个值不能设成 30 s —— 六个
/// 候选串起来就是三分钟。6 s 与移动网络下的一次正常 TLS 握手 + 首个字节同量级。
constexpr int kPerAttemptTimeoutMs = 6000;
/// 整轮检查的总预算：超过就报失败（设置页的「检查更新」不能被挂成无响应）。
constexpr qint64 kCheckBudgetMs = 24000;

QString humanSize(qint64 bytes) {
    if (bytes <= 0) return QString();
    if (bytes < 1024) return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024) return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
}

/// 当前平台最该点的那个安装包（决定 `recommended`）。纯展示用：识别不出来就
/// 一个都不推荐，让用户自己挑，而不是瞎猜一个错平台的文件。
bool isRecommendedAsset(const QString& name) {
    const QString lower = name.toLower();
#if defined(Q_OS_WIN)
    return lower.endsWith(QStringLiteral(".exe"));
#elif defined(Q_OS_LINUX)
    return lower.endsWith(QStringLiteral(".appimage")) || lower.endsWith(QStringLiteral(".deb"));
#elif defined(Q_OS_MACOS)
    return lower.endsWith(QStringLiteral(".dmg"));
#else
    Q_UNUSED(lower)
    return false;
#endif
}

} // namespace

UpdateService::UpdateService(SettingsStore* settings, QObject* parent)
    : QObject(parent), settings_(settings) {}

QString UpdateService::currentVersion() const {
    return QStringLiteral(KOMIRA_VERSION);
}

QString UpdateService::repositoryUrl() const { return kRepositoryUrl; }

QVariantList UpdateService::channelOptions() const {
    const auto row = [](const char* id, const QString& label, bool accelerator) {
        QVariantMap map;
        map["id"] = QString::fromLatin1(id);
        map["label"] = label;
        map["accelerator"] = accelerator;
        return map;
    };
    QVariantList out;
    out << row(update_channel::kAuto, QStringLiteral("自动（直连优先，失败走加速）"), true);
    out << row(update_channel::kDirect, QStringLiteral("仅直连"), false);
    out << row(update_channel::kCustom, QStringLiteral("自定义加速代理"), true);
    for (const update_channel::Proxy& proxy : update_channel::builtinProxies()) {
        out << row(proxy.id, QString::fromUtf8(proxy.label), true);
    }
    return out;
}

void UpdateService::setState(const QString& state, const QString& message) {
    if (state_ == state && message_ == message) return;
    state_ = state;
    message_ = message;
    emit changed();
}

std::vector<update_channel::Candidate> UpdateService::buildCandidates() const {
    if (!attemptsOverride_.isEmpty()) {
        std::vector<update_channel::Candidate> out;
        for (int i = 0; i < attemptsOverride_.size(); ++i) {
            update_channel::Candidate candidate;
            candidate.url = attemptsOverride_.at(i).toStdString();
            candidate.label = QStringLiteral("测试通道 %1").arg(i + 1).toStdString();
            // 只有第一个算「直连」样式的候选；其余按加速代理处理，好让
            // `usedProxy` 与安装包前缀拼接这条支路也被覆盖到。
            candidate.accelerator = i > 0;
            out.push_back(candidate);
        }
        return out;
    }
    const std::string channel =
        settings_ ? settings_->updateChannel().toStdString() : std::string(update_channel::kAuto);
    const std::string custom =
        settings_ ? settings_->updateCustomProxy().toStdString() : std::string();
    const std::string target =
        endpointOverride_.isEmpty() ? std::string(kLatestReleaseApi)
                                    : endpointOverride_.toStdString();
    return update_channel::candidates(channel, custom, target);
}

void UpdateService::check(bool silent) {
    if (inflight_) return; // 已有请求在途，忽略重复点击
    silent_ = silent;
    attempts_ = buildCandidates();
    attemptIndex_ = 0;
    if (!silent) setState(QStringLiteral("checking"), QStringLiteral("正在检查更新…"));
    attemptClock_.invalidate();
    startNextAttempt();
}

void UpdateService::startNextAttempt() {
    if (attemptIndex_ >= attempts_.size()) {
        finishFailure();
        return;
    }
    if (!attemptClock_.isValid()) attemptClock_.start();
    if (attemptClock_.elapsed() > kCheckBudgetMs) {
        qInfo() << "[update] budget exhausted after" << attemptIndex_ << "attempts";
        finishFailure();
        return;
    }

    const update_channel::Candidate& candidate = attempts_.at(attemptIndex_);
    // 尝试过程中把「正在走哪条」回显到 message：加速代理的首字节可能要好几秒，
    // 界面上只是个转圈的话用户会以为点了没反应。
    if (!silent_) {
        setState(QStringLiteral("checking"),
                 candidate.accelerator
                     ? QStringLiteral("正在检查更新…（经 %1）").arg(QString::fromStdString(candidate.label))
                     : QStringLiteral("正在检查更新…"));
    }
    qInfo() << "[update] attempt" << int(attemptIndex_ + 1) << "/" << int(attempts_.size())
            << QString::fromStdString(candidate.url);

    QNetworkRequest request(QUrl(QString::fromStdString(candidate.url)));
    request.setRawHeader("Accept", QByteArrayLiteral("application/vnd.github+json"));
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("KomiraQuake/") + currentVersion());
    request.setTransferTimeout(kPerAttemptTimeoutMs);

    inflight_ = nam_.get(request);
    connect(inflight_, &QNetworkReply::finished, this, [this]() {
        QNetworkReply* reply = inflight_;
        inflight_ = nullptr;
        handleReply(reply);
        reply->deleteLater();
    });
}

void UpdateService::handleReply(QNetworkReply* reply) {
    const QByteArray body = reply->readAll();
    const QJsonObject object = QJsonDocument::fromJson(body).object();
    const QString tag = object.value(QStringLiteral("tag_name")).toString();
    const update_channel::Candidate candidate = attempts_.at(attemptIndex_);

    if (reply->error() != QNetworkReply::NoError || tag.isEmpty()) {
        qInfo() << "[update] attempt failed:" << QString::fromStdString(candidate.label)
                << reply->error() << reply->errorString();
        ++attemptIndex_;
        startNextAttempt();
        return;
    }

    applyRelease(object, QString::fromStdString(candidate.label), candidate.accelerator);
}

void UpdateService::applyRelease(const QJsonObject& object, const QString& routeLabel,
                                 bool accelerator) {
    const QString tag = object.value(QStringLiteral("tag_name")).toString();
    latest_ = tag.startsWith(QLatin1Char('v')) ? tag.mid(1) : tag;
    releaseUrl_ = object.value(QStringLiteral("html_url")).toString();
    routeLabel_ = routeLabel;
    usedProxy_ = accelerator;

    // 安装包同样复用本次成功的通道：检查能通、点下载却打开 github.com 是自相矛盾的
    // —— 用户选加速代理，正是因为那个域名在他这儿打不开。
    assets_.clear();
    const auto prefixForAssets = [&](const QString& url) {
        if (!accelerator) return url;
        // 从命中的候选 URL 反推前缀：候选本身就是 `<前缀>/<目标>`。
        const QString candidateUrl =
            QString::fromStdString(attempts_.at(attemptIndex_).url);
        const QString target = endpointOverride_.isEmpty() ? QString::fromLatin1(kLatestReleaseApi)
                                                          : endpointOverride_;
        if (!candidateUrl.endsWith(target)) return url;
        const QString prefix = candidateUrl.left(candidateUrl.size() - target.size());
        return prefix + url;
    };
    const QJsonArray assets = object.value(QStringLiteral("assets")).toArray();
    for (const QJsonValue& value : assets) {
        const QJsonObject asset = value.toObject();
        const QString name = asset.value(QStringLiteral("name")).toString();
        const QString url = asset.value(QStringLiteral("browser_download_url")).toString();
        if (name.isEmpty() || url.isEmpty()) continue;
        QVariantMap row;
        row["name"] = name;
        row["url"] = url;
        row["downloadUrl"] = prefixForAssets(url);
        row["sizeText"] = humanSize(static_cast<qint64>(asset.value(QStringLiteral("size")).toDouble()));
        row["recommended"] = isRecommendedAsset(name);
        assets_ << row;
    }

    qInfo() << "[update] current" << currentVersion() << "latest" << latest_ << "route"
            << routeLabel_ << "silent" << silent_;

    if (isNewerVersion(currentVersion(), latest_)) {
        setState(QStringLiteral("updateAvailable"),
                 QStringLiteral("发现新版本 v%1").arg(latest_));
    } else if (silent_) {
        setState(QStringLiteral("idle"), QString());
    } else {
        setState(QStringLiteral("upToDate"),
                 QStringLiteral("已是最新版本 v%1").arg(currentVersion()));
    }
    // releaseUrl / assets / routeLabel 与 state 不同，可能只变其中之一（同一版本
    // 换了通道），统一再发一次变更。
    emit changed();
}

void UpdateService::finishFailure() {
    const std::size_t tried = attempts_.size();
    qInfo() << "[update] all" << int(tried) << "routes failed";
    attempts_.clear();
    assets_.clear();
    // 上一轮成功的通道不能留到这一轮：状态已经是 error，界面上若还挂着
    // 「更新通道 · gh-proxy.org」会让人以为这次是走代理失败的（其实可能一条都没通）。
    routeLabel_.clear();
    usedProxy_ = false;
    if (silent_) {
        setState(QStringLiteral("idle"), QString());
        return;
    }
    setState(QStringLiteral("error"),
             tried == 0 ? QStringLiteral("检查更新失败：没有可用的更新通道")
                        : QStringLiteral("检查更新失败，请检查网络或更换更新通道"));
    emit changed();
}

void UpdateService::openReleasePage() {
    if (!releaseUrl_.isEmpty()) openUrl(releaseUrl_);
}

void UpdateService::openUrl(const QString& url) {
    if (url.isEmpty()) return;
    emit openUrlRequested(QUrl(url));
}

} // namespace komira
