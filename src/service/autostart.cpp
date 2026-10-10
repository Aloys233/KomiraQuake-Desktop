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

/// NixOS 等不可变发行版每次升级都会更换 /nix/store 路径，安装版必须使用
/// PATH 中的稳定命令名；AppImage 则必须保留 APPIMAGE 指向的实际文件。
QString launchExec() {
    const QByteArray appImage = qgetenv("APPIMAGE");
    return appImage.isEmpty() ? QStringLiteral("komiraquake")
                              : quoted(QString::fromLocal8Bit(appImage));
}

QString autostartContent() {
    return QStringLiteral("[Desktop Entry]\n"
                          "Type=Application\n"
                          "Name=KomiraQuake\n"
                          "Name[zh_CN]=KomiraQuake 地震预警\n"
                          "Comment=Earthquake early warning and monitoring client\n"
                          "Exec=%1\n"
                          "Icon=komiraquake\n"
                          "Terminal=false\n"
                          "X-GNOME-Autostart-enabled=true\n")
        .arg(launchExec());
}

bool writeAutostartFile() {
    const QString path = autostartFilePath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return false;
    const QByteArray content = autostartContent().toUtf8();
    const bool written = file.write(content) == content.size();
    file.close();
    return written;
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

AutoStartService::AutoStartService(QObject* parent) : QObject(parent) {
#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
    // 旧版本把 /nix/store/<hash>-.../bin/.komiraquake-wrapped 写进了自启文件。
    // 新版本启动时迁移一次，之后升级只解析稳定的 komiraquake 命令名。
    const QString path = autostartFilePath();
    QFile file(path);
    if (file.exists() && file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString current = QString::fromUtf8(file.readAll());
        const QString expected = QStringLiteral("Exec=%1\n").arg(launchExec());
        if (!current.contains(expected)) writeAutostartFile();
    }
#endif
}

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
    if (!writeAutostartFile()) return;
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
