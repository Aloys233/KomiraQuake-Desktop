#pragma once

#include <QColor>
#include <QIcon>
#include <QObject>
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
    /// 预警首次成为活动事件时弹一次通知，并按设置请求把主窗口拉到前台。
    void announceAlert(const QVariantMap& warning);
    static QIcon makeIcon(const QColor& color);

    AppController* app_ = nullptr;
    QSystemTrayIcon tray_;
    QMenu* menu_ = nullptr;
    QTimer blinkTimer_;
    bool blinkOn_ = false;
    /// 已弹过通知的事件 identity：同一事件只提醒一次，修正报不重复打扰。
    QString announced_;
    /// 通知气泡驻留时长。与 Qt 默认一致，但显式传入以免被枚举隐式转换覆盖
    /// （见 announceAlert 注释：Critical 的值 2 会被当成 msecs）。
    static constexpr int kNotifyTimeoutMs = 10'000;
};

} // namespace komira
