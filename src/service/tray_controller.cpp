#include "service/tray_controller.h"

#include <QCoreApplication>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QVariantMap>

#include "app/app_controller.h"
#include "prefs/settings_store.h"

namespace komira {

TrayController::TrayController(AppController* app, QObject* parent)
    : QObject(parent), app_(app) {
    menu_ = new QMenu();
    menu_->addAction(QStringLiteral("显示主界面"), this, [this]() { emit showWindowRequested(); });
    menu_->addSeparator();

    auto* muteAction = menu_->addAction(QStringLiteral("全局静音"), this, [this](bool checked) {
        if (app_->settings()) {
            app_->settings()->setIsMuted(checked);
        }
    });
    muteAction->setCheckable(true);
    if (app_->settings()) {
        muteAction->setChecked(app_->settings()->isMuted());
    }


    menu_->addSeparator();
    menu_->addAction(QStringLiteral("退出 KomiraQuake"), this, []() { QCoreApplication::quit(); });

    tray_.setContextMenu(menu_);
    tray_.setToolTip(QStringLiteral("KomiraQuake 地震预警"));
    tray_.setIcon(makeIcon(QColor(0x00, 0x68, 0x74)));

    connect(&tray_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
                    emit showWindowRequested();
                }
            });
    tray_.show();

    blinkTimer_.setInterval(500);
    connect(&blinkTimer_, &QTimer::timeout, this, [this]() {
        blinkOn_ = !blinkOn_;
        refresh();
    });

    connect(app_, &AppController::warningChanged, this, &TrayController::refresh);
    refresh();
}

QIcon TrayController::makeIcon(const QColor& color) {
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawRoundedRect(QRectF(4, 4, 56, 56), 16, 16);
    painter.setPen(QPen(Qt::white, 4));
    painter.setBrush(Qt::NoBrush);
    painter.drawArc(QRectF(16, 20, 32, 26), 0, 180 * 16);
    painter.end();
    return QIcon(pixmap);
}

void TrayController::announceAlert(const QVariantMap& warning) {
    // 门槛与音效 / HUD 一致：只有通过烈度过滤的预警级事件才提醒。
    if (!app_->alertEligible()) return;
    // toMap 的 id 已是 identity()，可直接作为去重键。
    const QString identity = warning.value("id").toString();
    if (identity.isEmpty() || identity == announced_) return;
    announced_ = identity;

    SettingsStore* settings = app_->settings();
    const QString location = warning.value("location").toString();
    if (settings && settings->enableDesktopNotification() && QSystemTrayIcon::supportsMessages()) {
        // 注意重载选择：QSystemTrayIcon 有两个 showMessage 重载 ——
        //   (title, msg, const QIcon&, int msecs) 与
        //   (title, msg, MessageIcon, int msecs)
        // 直接传Critical 会被解析成第一个（QIcon 版），枚举值 2 被当成
        // msecs，通知只显示 2 毫秒 —— 表现为「一闪而过」。显式转型锁定正确重载。
        tray_.showMessage(
            QStringLiteral("地震预警"),
            location.isEmpty() ? QStringLiteral("检测到地震，请注意避险。")
                               : QStringLiteral("%1，请注意避险。").arg(location),
            QSystemTrayIcon::MessageIcon(QSystemTrayIcon::Critical), int{kNotifyTimeoutMs});
    }
    // 抢焦点是打扰性最强的手段，默认关闭，由用户在设置里显式开启。
    if (settings && settings->enableWindowRaise()) emit alertWindowRaiseRequested();
}

void TrayController::refresh() {
    const QVariantMap warning = app_->activeWarning().toMap();
    const QString tag = warning.value("levelTag").toString();
    // 闪烁门槛与通知 / 音效 / HUD 一致：必须是通过烈度过滤的预警级事件。
    // 只看 levelTag 会让全球任意M4.5+ 事件都把托盘闪红，与用户的过滤设置矛盾。
    const bool alert = app_->alertEligible()
        && (tag == QLatin1String("WARNING") || tag == QLatin1String("CRITICAL"));

    if (alert) {
        announceAlert(warning);
        if (!blinkTimer_.isActive()) blinkTimer_.start();
        const QColor base = tag == QLatin1String("CRITICAL") ? QColor(0xBA, 0x1A, 0x1A)
                                                             : QColor(0xBC, 0x28, 0x00);
        tray_.setIcon(makeIcon(blinkOn_ ? base : QColor(0x24, 0x2C, 0x2E)));
        tray_.setToolTip(QStringLiteral("⚠ 地震预警：%1").arg(warning.value("location").toString()));
    } else {
        blinkTimer_.stop();
        blinkOn_ = false;
        // 预警结束后允许下一个事件重新提醒。
        announced_.clear();
        tray_.setIcon(makeIcon(QColor(0x00, 0x68, 0x74)));
        tray_.setToolTip(QStringLiteral("KomiraQuake 地震预警 · 正常守候中"));
    }
}

} // namespace komira
