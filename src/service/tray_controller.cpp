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

    // 图标只画一次：状态切换时复用（见 refresh）。
    normalIcon_ = makeIcon(QColor(0x00, 0x68, 0x74));
    criticalIcon_ = makeIcon(QColor(0xBA, 0x1A, 0x1A));
    warningIcon_ = makeIcon(QColor(0xBC, 0x28, 0x00));
    blinkDarkIcon_ = makeIcon(QColor(0x24, 0x2C, 0x2E));

    tray_.setContextMenu(menu_);
    tray_.setToolTip(QStringLiteral("KomiraQuake 地震预警"));
    currentIcon_ = normalIcon_;
    tray_.setIcon(currentIcon_);

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
    connect(app_, &AppController::notifiableEvent, this, &TrayController::announceTier);
    refresh();
}

void TrayController::applyIcon(const QIcon& icon) {
    // QSystemTrayIcon::setIcon 是一次 D-Bus / 平台调用：闪烁定时器 500 ms 一轮，
    // 值没变时不该重复下发（尤其「预警但未达闪烁档」时旧实现每轮都重建同一张图）。
    if (currentIcon_.cacheKey() == icon.cacheKey()) return;
    currentIcon_ = icon;
    tray_.setIcon(icon);
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

void TrayController::announceTier(const QVariantMap& event, const QString& tier) {
    // toMap 的 id 已是 identity()，可直接作为去重键。
    const QString identity = event.value("id").toString();
    if (identity.isEmpty()) return;
    const QString key = tier + QLatin1Char('|') + identity;
    if (announced_.contains(key)) return;
    announced_.insert(key);
    if (announced_.size() > 512) announced_.clear();

    SettingsStore* settings = app_->settings();
    if (!settings) return;

    bool notify = false;
    bool raise = false;
    QString title;
    QString body;
    QSystemTrayIcon::MessageIcon icon = QSystemTrayIcon::Information;
    const QString location = event.value("location").toString();
    if (tier == QLatin1String("alert")) {
        notify = settings->notifyAlert();
        raise = settings->raiseAlert();
        title = QStringLiteral("地震预警");
        body = location.isEmpty() ? QStringLiteral("检测到地震，请注意避险。")
                                  : QStringLiteral("%1，请注意避险。").arg(location);
        icon = QSystemTrayIcon::Critical;
    } else if (tier == QLatin1String("eew")) {
        notify = settings->notifyEew();
        raise = settings->raiseEew();
        title = QStringLiteral("地震预警更新");
        body = QStringLiteral("%1 · M %2")
                   .arg(location.isEmpty() ? QStringLiteral("未知地点") : location)
                   .arg(event.value("magnitude").toDouble(), 0, 'f', 1);
        icon = QSystemTrayIcon::Warning;
    } else if (tier == QLatin1String("info")) {
        notify = settings->notifyInfo();
        raise = settings->raiseInfo();
        title = QStringLiteral("地震信息");
        body = QStringLiteral("%1 · M %2")
                   .arg(location.isEmpty() ? QStringLiteral("未知地点") : location)
                   .arg(event.value("magnitude").toDouble(), 0, 'f', 1);
        icon = QSystemTrayIcon::Information;
    } else {
        return;
    }

    if (notify && QSystemTrayIcon::supportsMessages()) {
        // 注意重载选择：QSystemTrayIcon 有两个 showMessage 重载 ——
        //   (title, msg, const QIcon&, int msecs) 与
        //   (title, msg, MessageIcon, int msecs)
        // 直接传枚举会被解析成第一个（QIcon 版），枚举值被当成 msecs，
        // 通知只显示几毫秒 —— 表现为「一闪而过」。显式转型锁定正确重载。
        tray_.showMessage(title, body, QSystemTrayIcon::MessageIcon(icon), int{kNotifyTimeoutMs});
    }
    // 抢焦点是打扰性最强的手段，默认关闭，由用户在设置里显式开启。
    if (raise) emit alertWindowRaiseRequested();
}

void TrayController::refresh() {
    const QVariantMap warning = app_->activeWarning().toMap();
    const QString tag = warning.value("levelTag").toString();
    // 闪烁门槛与通知 / 音效 / HUD 一致：必须是通过烈度过滤的预警级事件。
    // 只看 levelTag 会让全球任意M4.5+ 事件都把托盘闪红，与用户的过滤设置矛盾。
    const bool alert = app_->alertEligible()
        && (tag == QLatin1String("WARNING") || tag == QLatin1String("CRITICAL"));

    if (alert) {
        if (!blinkTimer_.isActive()) blinkTimer_.start();
        const QIcon& base = tag == QLatin1String("CRITICAL") ? criticalIcon_ : warningIcon_;
        applyIcon(blinkOn_ ? base : blinkDarkIcon_);
        tray_.setToolTip(QStringLiteral("⚠ 地震预警：%1").arg(warning.value("location").toString()));
    } else {
        blinkTimer_.stop();
        blinkOn_ = false;
        // 预警结束后允许同一 identity 重新提醒；目录信息的去重键保留更久。
        for (auto it = announced_.begin(); it != announced_.end();) {
            if (it->startsWith(QLatin1String("alert|")) || it->startsWith(QLatin1String("eew|")))
                it = announced_.erase(it);
            else
                ++it;
        }
        applyIcon(normalIcon_);
        tray_.setToolTip(QStringLiteral("KomiraQuake 地震预警 · 正常守候中"));
    }
}

} // namespace komira
