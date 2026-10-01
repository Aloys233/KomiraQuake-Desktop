#pragma once

#include <QColor>
#include <QIcon>
#include <QObject>
#include <QSystemTrayIcon>
#include <QTimer>

class QMenu;

namespace komira {

class AppController;

/// 系统托盘：预警时红闪，点击恢复主窗口。M6。
class TrayController : public QObject {
    Q_OBJECT
public:
    explicit TrayController(AppController* app, QObject* parent = nullptr);

signals:
    void showWindowRequested();

private:
    void refresh();
    static QIcon makeIcon(const QColor& color);

    AppController* app_ = nullptr;
    QSystemTrayIcon tray_;
    QMenu* menu_ = nullptr;
    QTimer blinkTimer_;
    bool blinkOn_ = false;
};

} // namespace komira
