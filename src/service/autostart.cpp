#include "service/autostart.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

namespace komira {

namespace {

#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
const QString kAutostartFileName = QStringLiteral("komiraquake.desktop");

/// .desktop 的 Exec 是转义字段：带空格的路径用引号包裹，并按规范转义反斜杠与引号。
QString quoted(const QString& path) {
    QString escaped = path;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

/// 自启条目必须位于 `$XDG_CONFIG_HOME/autostart/`（通常 `~/.config/autostart/`）；
/// QStandardPaths::ConfigLocation 只给到 `~/.config`，需自行补 `autostart` 段，
/// 否则桌面环境不会读取该条目，自启不生效。
QString autostartFilePath() {
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
           + QStringLiteral("/autostart/") + kAutostartFileName;
}

/// AppImage 运行时 applicationFilePath() 指向临时挂载点，重启后失效；
/// 优先使用 APPIMAGE 环境变量里的原始文件。
QString launchPath() {
    const QByteArray appImage = qgetenv("APPIMAGE");
    return appImage.isEmpty() ? QCoreApplication::applicationFilePath()
                              : QString::fromLocal8Bit(appImage);
}
#elif defined(Q_OS_WIN)
const QString kWindowsRunKey =
    QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");
const QString kEntryName = QStringLiteral("KomiraQuake");

/// Run 值是 REG_SZ，原样交给 CreateProcess，**不转义反斜杠**（否则路径会被写成
/// `C:\\Program Files\\...` 这种双反斜杠）。只需为含空格的路径加引号。
QString runValue(const QString& path) {
    return QLatin1Char('"') + QDir::toNativeSeparators(path) + QLatin1Char('"');
}
#endif

} // namespace

AutoStartService::AutoStartService(QObject* parent) : QObject(parent) {}

bool AutoStartService::isEnabled() const {
#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
    return QFileInfo::exists(autostartFilePath());
#elif defined(Q_OS_WIN)
    QSettings run(kWindowsRunKey, QSettings::NativeFormat);
    return run.contains(kEntryName);
#else
    return false;
#endif
}

void AutoStartService::setEnabled(bool enabled) {
#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
    const QString path = autostartFilePath();
    if (!enabled) {
        QFile::remove(path);
        emit enabledChanged();
        return;
    }
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return;
    const QString content =
        QStringLiteral("[Desktop Entry]\n"
                       "Type=Application\n"
                       "Name=KomiraQuake\n"
                       "Name[zh_CN]=KomiraQuake 地震预警\n"
                       "Comment=Earthquake early warning and monitoring client\n"
                       "Exec=%1\n"
                       "Icon=komiraquake\n"
                       "Terminal=false\n"
                       "X-GNOME-Autostart-enabled=true\n")
            .arg(quoted(launchPath()));
    file.write(content.toUtf8());
    file.close();
    emit enabledChanged();
#elif defined(Q_OS_WIN)
    QSettings run(kWindowsRunKey, QSettings::NativeFormat);
    if (enabled) {
        run.setValue(kEntryName, runValue(QCoreApplication::applicationFilePath()));
    } else {
        run.remove(kEntryName);
    }
    emit enabledChanged();
#else
    Q_UNUSED(enabled);
#endif
}

} // namespace komira
