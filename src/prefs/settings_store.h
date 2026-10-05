#pragma once

#include <QObject>
#include <QSettings>
#include <QString>

namespace komira {

/// 设置项与默认值。《NATIVE_PORT_SPEC》 §7。统一由 changed() 通知，QML 绑定整体刷新。
class SettingsStore : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString basemapId READ basemapId WRITE setBasemapId NOTIFY changed)
    Q_PROPERTY(QString customBasemapUrl READ customBasemapUrl WRITE setCustomBasemapUrl NOTIFY changed)
    Q_PROPERTY(int customBasemapDatum READ customBasemapDatum WRITE setCustomBasemapDatum NOTIFY changed)
    Q_PROPERTY(int intensityStandard READ intensityStandard WRITE setIntensityStandard NOTIFY changed)
    Q_PROPERTY(double localIntensityFilter READ localIntensityFilter WRITE setLocalIntensityFilter NOTIFY changed)
    Q_PROPERTY(bool enableWarnings READ enableWarnings WRITE setEnableWarnings NOTIFY changed)
    Q_PROPERTY(bool enableSoundAlert READ enableSoundAlert WRITE setEnableSoundAlert NOTIFY changed)
    Q_PROPERTY(bool reduceMotion READ reduceMotion WRITE setReduceMotion NOTIFY changed)
    Q_PROPERTY(bool backgroundBlur READ backgroundBlur WRITE setBackgroundBlur NOTIFY changed)
    Q_PROPERTY(bool isMuted READ isMuted WRITE setIsMuted NOTIFY changed)
    Q_PROPERTY(bool enabledWolfx READ enabledWolfx WRITE setEnabledWolfx NOTIFY changed)
    Q_PROPERTY(bool enabledPancakes READ enabledPancakes WRITE setEnabledPancakes NOTIFY changed)
    Q_PROPERTY(double alertVolume READ alertVolume WRITE setAlertVolume NOTIFY changed)
    Q_PROPERTY(bool enableSpeech READ enableSpeech WRITE setEnableSpeech NOTIFY changed)
    Q_PROPERTY(bool speakUpdates READ speakUpdates WRITE setSpeakUpdates NOTIFY changed)
    Q_PROPERTY(bool speakCountdown READ speakCountdown WRITE setSpeakCountdown NOTIFY changed)
    Q_PROPERTY(double speechRate READ speechRate WRITE setSpeechRate NOTIFY changed)
    Q_PROPERTY(bool enableNtpSync READ enableNtpSync WRITE setEnableNtpSync NOTIFY changed)
    Q_PROPERTY(QString customNtpServer READ customNtpServer WRITE setCustomNtpServer NOTIFY changed)
    Q_PROPERTY(bool darkMode READ darkMode WRITE setDarkMode NOTIFY changed)
    Q_PROPERTY(bool silentStart READ silentStart WRITE setSilentStart NOTIFY changed)
    Q_PROPERTY(bool autoCheckUpdates READ autoCheckUpdates WRITE setAutoCheckUpdates NOTIFY changed)

public:
    explicit SettingsStore(QObject* parent = nullptr);

    QString basemapId() const { return s_.value("basemapId", "amap_vector").toString(); }
    void setBasemapId(const QString& v) { set("basemapId", v); }

    QString customBasemapUrl() const { return s_.value("customBasemapUrl", "").toString(); }
    void setCustomBasemapUrl(const QString& v) { set("customBasemapUrl", v); }

    int customBasemapDatum() const { return s_.value("customBasemapDatum", 1).toInt(); } // 0 wgs84, 1 gcj02
    void setCustomBasemapDatum(int v) { set("customBasemapDatum", v); }

    int intensityStandard() const { return s_.value("intensityStandard", 0).toInt(); } // 0 csis, 1 jma
    void setIntensityStandard(int v) { set("intensityStandard", v); }

    double localIntensityFilter() const { return s_.value("localIntensityFilter", 0.0).toDouble(); }
    void setLocalIntensityFilter(double v) { set("localIntensityFilter", v); }

    /// 地震预警总开关（设置页「地震预警」）：关闭时只展示，不产生任何提醒。
    bool enableWarnings() const { return s_.value("enableWarnings", false).toBool(); }
    void setEnableWarnings(bool v) { set("enableWarnings", v); }

    bool enableSoundAlert() const { return s_.value("enableSoundAlert", true).toBool(); }
    void setEnableSoundAlert(bool v) { set("enableSoundAlert", v); }

    bool backgroundBlur() const { return s_.value("backgroundBlur", true).toBool(); }
    void setBackgroundBlur(bool v) { set("backgroundBlur", v); }

    bool reduceMotion() const { return s_.value("reduceMotion", false).toBool(); }
    void setReduceMotion(bool v) { set("reduceMotion", v); }

    bool isMuted() const { return s_.value("isMuted", false).toBool(); }
    void setIsMuted(bool v) { set("isMuted", v); }

    bool enabledWolfx() const { return s_.value("enabledWolfx", true).toBool(); }
    void setEnabledWolfx(bool v) { set("enabledWolfx", v); }

    bool enabledPancakes() const { return s_.value("enabledPancakes", true).toBool(); }
    void setEnabledPancakes(bool v) { set("enabledPancakes", v); }

    double alertVolume() const { return s_.value("alertVolume", 1.0).toDouble(); }
    void setAlertVolume(double v) { set("alertVolume", v); }

    bool enableSpeech() const { return s_.value("enableSpeech", false).toBool(); }
    void setEnableSpeech(bool v) { set("enableSpeech", v); }

    bool speakUpdates() const { return s_.value("speakUpdates", false).toBool(); }
    void setSpeakUpdates(bool v) { set("speakUpdates", v); }

    bool speakCountdown() const { return s_.value("speakCountdown", true).toBool(); }
    void setSpeakCountdown(bool v) { set("speakCountdown", v); }

    double speechRate() const { return s_.value("speechRate", 0.5).toDouble(); }
    void setSpeechRate(double v) { set("speechRate", v); }

    bool enableNtpSync() const { return s_.value("enableNtpSync", true).toBool(); }
    void setEnableNtpSync(bool v) { set("enableNtpSync", v); }

    /// 自定义 NTP 服务器主机名；留空则使用内置 SNTP 列表。
    QString customNtpServer() const { return s_.value("customNtpServer", "").toString(); }
    void setCustomNtpServer(const QString& v) { set("customNtpServer", v); }

    bool darkMode() const { return s_.value("darkMode", false).toBool(); }
    void setDarkMode(bool v) { set("darkMode", v); }

    /// 静默启动：启动时不显示主窗口，仅保留托盘（等同「关闭窗口」）。
    bool silentStart() const { return s_.value("silentStart", false).toBool(); }
    void setSilentStart(bool v) { set("silentStart", v); }

    /// 启动时后台自动检查一次新版本。
    bool autoCheckUpdates() const { return s_.value("autoCheckUpdates", true).toBool(); }
    void setAutoCheckUpdates(bool v) { set("autoCheckUpdates", v); }

    Q_INVOKABLE void resetToDefaults();

signals:
    void changed();

private:
    template <typename T>
    void set(const QString& key, const T& value) {
        if (s_.value(key) == QVariant::fromValue(value)) return;
        s_.setValue(key, value);
        emit changed();
    }

    QSettings s_;
};

} // namespace komira
