#include "service/tray_controller.h"

#include <QCoreApplication>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QVariantMap>

#include "app/app_controller.h"

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

void TrayController::refresh() {
    const QVariantMap warning = app_->activeWarning().toMap();
    const QString tag = warning.value("levelTag").toString();
    const bool alert = tag == QLatin1String("WARNING") || tag == QLatin1String("CRITICAL");

    if (alert) {
        if (!blinkTimer_.isActive()) blinkTimer_.start();
        const QColor base = tag == QLatin1String("CRITICAL") ? QColor(0xBA, 0x1A, 0x1A)
                                                             : QColor(0xBC, 0x28, 0x00);
        tray_.setIcon(makeIcon(blinkOn_ ? base : QColor(0x24, 0x2C, 0x2E)));
        tray_.setToolTip(QStringLiteral("⚠ 地震预警：%1").arg(warning.value("location").toString()));
    } else {
        blinkTimer_.stop();
        blinkOn_ = false;
        tray_.setIcon(makeIcon(QColor(0x00, 0x68, 0x74)));
        tray_.setToolTip(QStringLiteral("KomiraQuake 地震预警 · 正常守候中"));
    }
}

} // namespace komira
