#pragma once

#include <QObject>

namespace komira {

/// 开机自启开关。基于操作系统状态（而非设置项）：
/// - Linux：`$XDG_CONFIG_HOME/autostart/komiraquake.desktop`
/// - Windows：`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`
/// 其他平台不支持，`enabled` 恒为 false。
class AutoStartService : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ isEnabled WRITE setEnabled NOTIFY enabledChanged)
public:
    explicit AutoStartService(QObject* parent = nullptr);

    bool isEnabled() const;
    void setEnabled(bool enabled);

signals:
    void enabledChanged();
};

} // namespace komira
