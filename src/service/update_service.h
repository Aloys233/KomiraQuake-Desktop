#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QString>

class QNetworkReply;

namespace komira {

/// 检查 GitHub Releases 上的最新版本。`state` ∈ {idle, checking, upToDate,
/// updateAvailable, error}，供设置页按状态展示文案与操作。
class UpdateService : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT)
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QString releaseUrl READ releaseUrl NOTIFY changed)
public:
    explicit UpdateService(QObject* parent = nullptr);

    QString currentVersion() const;
    QString state() const { return state_; }
    QString latestVersion() const { return latest_; }
    QString message() const { return message_; }
    QString releaseUrl() const { return releaseUrl_; }

    /// silent=true 用于自动检查：不进入 "checking" 态，失败/无更新时保持静默。
    Q_INVOKABLE void check(bool silent = false);
    Q_INVOKABLE void openReleasePage() const;

signals:
    void changed();

private:
    void handleReply(QNetworkReply* reply);
    void setState(const QString& state, const QString& message);

    QNetworkAccessManager nam_;
    QNetworkReply* inflight_ = nullptr;
    QString state_ = QStringLiteral("idle");
    QString latest_;
    QString message_;
    QString releaseUrl_;
    bool silent_ = false;
};

} // namespace komira
