#pragma once

#include <QColor>
#include <QIcon>
#include <QObject>
#include <QSet>
#include <QString>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QVariantMap>

class QMenu;

namespace komira {

class AppController;

/// 系统托盘：预警时红闪，点击恢复主窗口。M6。
class TrayController : public QObject {
    Q_OBJECT
public:
    explicit TrayController(AppController* app, QObject* parent = nullptr);

signals:
    /// 用户点击托盘图标/菜单：恢复窗口，保留用户离开时的页面。
    void showWindowRequested();
    /// 预警置顶：除显示窗口外，还要切回主页面（用户可能停在设置页）。
    void alertWindowRaiseRequested();

private:
    void refresh();
    /// 分级提醒：按 tier 读取对应的「通知 / 置顶」设置，同一事件只提醒一次。
    void announceTier(const QVariantMap& event, const QString& tier);
    static QIcon makeIcon(const QColor& color);

    AppController* app_ = nullptr;
    QSystemTrayIcon tray_;
    QMenu* menu_ = nullptr;
    QTimer blinkTimer_;
    bool blinkOn_ = false;
    /// 已提醒过的 `tier|identity`：同一事件只提醒一次，修正报不重复打扰。
    QSet<QString> announced_;
    /// 通知气泡驻留时长。与 Qt 默认一致，但显式传入以免被枚举隐式转换覆盖
    /// （见 announceTier 注释：MessageIcon 的值会被当成 msecs）。
    static constexpr int kNotifyTimeoutMs = 10'000;
};

} // namespace komira
