#pragma once

#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantList>

#include <vector>

#include "core/update_channel.h"

class QJsonObject;
class QNetworkReply;

namespace komira {

class SettingsStore;

/// 检查 GitHub Releases 上的最新版本。`state` ∈ {idle, checking, upToDate,
/// updateAvailable, error}，供设置页按状态展示文案与操作。
///
/// **多通道回退**：一次检查会按 `settings.updateChannel` 生成一串有序候选（直连 /
/// 内置加速代理 / 自定义代理，见 `core/update_channel.h`），逐个尝试、第一个成功即止。
/// 之所以不做成「一个固定 URL」：GitHub 与 api.github.com 在部分网络下不可达，
/// 而同一份 API 经由 gh-proxy 家族转出来是逐字相同的 JSON —— 换地址比换协议简单得多。
/// 每个候选都失败才算 `error`，且成功后记下**走的哪条通道**（`routeLabel`），
/// 供界面展示，也供「下载安装包」复用同一条通道（加速代理只放行资源下载，不放行网页）。
class UpdateService : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT)
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QString releaseUrl READ releaseUrl NOTIFY changed)
    /// 最近一次检查**尝试/成功**的通道名（「直连」「gh-proxy.org」…）；检查前为空。
    Q_PROPERTY(QString routeLabel READ routeLabel NOTIFY changed)
    /// 最近一次成功的检查是否经加速代理：决定安装包下载链接要不要套前缀。
    Q_PROPERTY(bool usedProxy READ usedProxy NOTIFY changed)
    /// 最新版本的安装包列表：`[{ name, sizeText, url, downloadUrl, recommended }]`。
    /// `downloadUrl` 已按本次检查走的通道拼好（直连或 `<代理前缀>/<原地址>`），
    /// QML 直接把它交给 `openUrl()`，不必自己拼。
    Q_PROPERTY(QVariantList assets READ assets NOTIFY changed)
    /// GitHub 仓库主页：设置页左下角的入口。放在更新服务里，因为它与检查目标是同一份常量。
    Q_PROPERTY(QString repositoryUrl READ repositoryUrl CONSTANT)
    /// 可选通道：`[{ id, label, accelerator }]`，供设置页下拉直接铺开。
    /// 由 `core/update_channel.h` 的单一份表生成 —— QML 里再抄一遍代理列表，
    /// 早晚会出现「加了一个代理、下拉里没有」。
    Q_PROPERTY(QVariantList channelOptions READ channelOptions CONSTANT)
public:
    explicit UpdateService(SettingsStore* settings, QObject* parent = nullptr);

    QString currentVersion() const;
    QString state() const { return state_; }
    QString latestVersion() const { return latest_; }
    QString message() const { return message_; }
    QString releaseUrl() const { return releaseUrl_; }
    QString routeLabel() const { return routeLabel_; }
    bool usedProxy() const { return usedProxy_; }
    QVariantList assets() const { return assets_; }
    QString repositoryUrl() const;
    QVariantList channelOptions() const;

    /// silent=true 用于自动检查：不进入 "checking" 态，失败/无更新时保持静默。
    Q_INVOKABLE void check(bool silent = false);
    Q_INVOKABLE void openReleasePage();
    /// 在系统浏览器里打开一个地址（安装包下载链接、仓库主页）。统一从这里出去，
    /// 浏览器调用只有一处，测试也只需替换一个出口。
    Q_INVOKABLE void openUrl(const QString& url);

    /// **测试接缝**：覆盖 Releases API 的目标地址（默认 api.github.com）。
    ///
    /// Qt 的 HTTP 栈没有「透明拦截」的官方口子，而这条逻辑的价值恰恰全在**回退链路**上
    /// （直连失败 → 代理成功），只测 `core/update_channel.h` 的字符串拼接等于没测网络层。
    /// 所以留一个显式的地址注入点：回归用本地 QTcpServer 扮演「直连不通、代理通」。
    void setReleaseEndpointForTest(const QString& url) { endpointOverride_ = url; }
    /// **测试接缝**：直接指定这一轮的候选地址表（绕过 `settings` 的通道解析）。
    /// 用途是让「第 1 个失败、第 2 个成功」这种链路在**不碰真实 gh-proxy** 的前提下可复现
    /// —— 通道表本身的取舍由 `core_engine_test` 逐条钉住，这里只关心重试循环与结果回填。
    void setAttemptsForTest(const QStringList& urls) { attemptsOverride_ = urls; }

signals:
    void changed();
    /// 请求用系统浏览器打开地址。AppController 接到后转给自己的 `openExternalUrl()`，
    /// 这样「打开网页」在应用里仍然只有一条出口。
    void openUrlRequested(const QUrl& url);

private:
    void setState(const QString& state, const QString& message);
    void startNextAttempt();
    void handleReply(QNetworkReply* reply);
    void finishFailure();
    void applyRelease(const QJsonObject& object, const QString& routeLabel, bool accelerator);
    /// 解析候选通道；`settings_` 为空时退化为「直连优先 + 内置代理」。
    std::vector<update_channel::Candidate> buildCandidates() const;

    QNetworkAccessManager nam_;
    QNetworkReply* inflight_ = nullptr;
    std::vector<update_channel::Candidate> attempts_;
    std::size_t attemptIndex_ = 0;
    /// 整轮检查的总时限：单个候选 6 s 超时，六个候选串起来最坏 36 s，
    /// 用户点「检查更新」等这么久会以为卡死；超过总时限直接报失败。
    QElapsedTimer attemptClock_;
    SettingsStore* settings_ = nullptr;
    /// 测试注入的 Releases API 地址；为空则用内置的 api.github.com。
    QString endpointOverride_;
    /// 测试注入的候选表（见 setAttemptsForTest）；非空时完全不看 settings 的通道。
    QStringList attemptsOverride_;
    QString state_ = QStringLiteral("idle");
    QString latest_;
    QString message_;
    QString releaseUrl_;
    QString routeLabel_;
    bool usedProxy_ = false;
    QVariantList assets_;
    bool silent_ = false;
};

} // namespace komira
