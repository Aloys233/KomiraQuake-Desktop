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
    /// 仅在图标真的变化时才下发到托盘。
    void applyIcon(const QIcon& icon);

    AppController* app_ = nullptr;
    QSystemTrayIcon tray_;
    /// 缓存的图标（正常 / 预警两色 / 闪烁用的暗色）。旧实现每次 refresh() 都新建
    /// 一张 64×64 QPixmap 并走一遍 QPainter —— 预警期间每 500 ms 一次，纯重复劳动。
    QIcon normalIcon_;
    QIcon criticalIcon_;
    QIcon warningIcon_;
    QIcon blinkDarkIcon_;
    /// 当前已应用的图标；相同则不再 setIcon（避免托盘后端反复重绘）。
    QIcon currentIcon_;
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
