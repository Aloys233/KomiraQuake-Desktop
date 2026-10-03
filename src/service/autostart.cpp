#include "service/autostart.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

namespace komira {

namespace {

/// 桌面项 Exec / 注册表命令行里带空格的路径需要引号包裹，并转义引号与反斜杠。
[[maybe_unused]] QString quoted(const QString& path) {
    QString escaped = path;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
const QString kAutostartFileName = QStringLiteral("komiraquake.desktop");

QString autostartFilePath() {
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
           + QLatin1Char('/') + kAutostartFileName;
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
        run.setValue(kEntryName,
                     quoted(QDir::toNativeSeparators(QCoreApplication::applicationFilePath())));
    } else {
        run.remove(kEntryName);
    }
    emit enabledChanged();
#else
    Q_UNUSED(enabled);
#endif
}

} // namespace komira
