#include "service/update_service.h"

#include <QDebug>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QtGlobal>

#include "app/version.h"
#include "core/version_compare.h"

namespace komira {

namespace {

const QUrl kLatestReleaseUrl =
    QUrl(QStringLiteral("https://api.github.com/repos/Aloys233/KomiraQuake-Desktop/releases/latest"));

} // namespace

UpdateService::UpdateService(QObject* parent) : QObject(parent) {}

QString UpdateService::currentVersion() const {
    return QStringLiteral(KOMIRA_VERSION);
}

void UpdateService::setState(const QString& state, const QString& message) {
    if (state_ == state && message_ == message) return;
    state_ = state;
    message_ = message;
    emit changed();
}

void UpdateService::check(bool silent) {
    if (inflight_) return; // 已有请求在途，忽略重复点击
    silent_ = silent;
    if (!silent) setState(QStringLiteral("checking"), QStringLiteral("正在检查更新…"));

    QNetworkRequest request(kLatestReleaseUrl);
    request.setRawHeader("Accept", QByteArrayLiteral("application/vnd.github+json"));
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("KomiraQuake/") + currentVersion());
    request.setTransferTimeout(10000);

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

    if (reply->error() != QNetworkReply::NoError || tag.isEmpty()) {
        qInfo() << "[update] check failed:" << reply->error() << reply->errorString();
        if (silent_) {
            setState(QStringLiteral("idle"), QString());
        } else {
            setState(QStringLiteral("error"), QStringLiteral("检查更新失败，请稍后重试"));
        }
        return;
    }

    latest_ = tag.startsWith(QLatin1Char('v')) ? tag.mid(1) : tag;
    releaseUrl_ = object.value(QStringLiteral("html_url")).toString();
    qInfo() << "[update] current" << currentVersion() << "latest" << latest_
            << "silent" << silent_;

    if (isNewerVersion(currentVersion(), latest_)) {
        setState(QStringLiteral("updateAvailable"),
                 QStringLiteral("发现新版本 v%1").arg(latest_));
    } else if (silent_) {
        setState(QStringLiteral("idle"), QString());
    } else {
        setState(QStringLiteral("upToDate"),
                 QStringLiteral("已是最新版本 v%1").arg(currentVersion()));
    }
}

void UpdateService::openReleasePage() const {
    if (!releaseUrl_.isEmpty()) QDesktopServices::openUrl(QUrl(releaseUrl_));
}

} // namespace komira
