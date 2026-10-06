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
    Q_PROPERTY(bool enableDesktopNotification READ enableDesktopNotification WRITE setEnableDesktopNotification NOTIFY changed)
    Q_PROPERTY(bool enableWindowRaise READ enableWindowRaise WRITE setEnableWindowRaise NOTIFY changed)
    Q_PROPERTY(bool reduceMotion READ reduceMotion WRITE setReduceMotion NOTIFY changed)
    Q_PROPERTY(bool backgroundBlur READ backgroundBlur WRITE setBackgroundBlur NOTIFY changed)
    Q_PROPERTY(bool isMuted READ isMuted WRITE setIsMuted NOTIFY changed)
    /// 已禁用的数据源 id 列表。采用「禁用集合」而非「启用集合」：未列出的源默认启用，
    /// 新增数据源无需迁移即默认开启（两端一致）。
    Q_PROPERTY(QStringList disabledSources READ disabledSources WRITE setDisabledSources NOTIFY changed)
    /// Jian 数据源的刷新令牌（rt_…，登录后由应用写入并持久化；不向 UI 回显明文）。
    Q_PROPERTY(QString jianRefreshToken READ jianRefreshToken WRITE setJianRefreshToken NOTIFY changed)
    /// 是否已完成 Jian 登录（有刷新令牌）。未登录时设置页显示登录框，且该源不会被启动。
    Q_PROPERTY(bool jianConfigured READ jianConfigured NOTIFY changed)
    /// Whews 数据源的访问令牌（wat_…，在 auth.beecld.com 申请；设置页粘贴后写入并持久化；
    /// 不向 UI 回显明文）。留空即视为未配置，不会建立任何连接。
    Q_PROPERTY(QString whewsToken READ whewsToken WRITE setWhewsToken NOTIFY changed)
    /// 是否已配置 Whews 令牌。未配置时设置页显示令牌输入框，且该源不会被启动。
    Q_PROPERTY(bool whewsConfigured READ whewsConfigured NOTIFY changed)
    Q_PROPERTY(double alertVolume READ alertVolume WRITE setAlertVolume NOTIFY changed)
    Q_PROPERTY(bool enableNtpSync READ enableNtpSync WRITE setEnableNtpSync NOTIFY changed)
    Q_PROPERTY(QString customNtpServer READ customNtpServer WRITE setCustomNtpServer NOTIFY changed)
    /// 开发者模式：显出模拟数据源的卡片与地址输入框。关闭时该源不被启动、不建立任何连接。
    Q_PROPERTY(bool developerMode READ developerMode WRITE setDeveloperMode NOTIFY changed)
    /// 模拟数据源的 WebSocket 地址（如 ws://127.0.0.1:8080/ws）。留空即视为未配置。
    Q_PROPERTY(QString simulatedUrl READ simulatedUrl WRITE setSimulatedUrl NOTIFY changed)
    /// 主题模式：`system` / `light` / `dark`。替代旧版布尔 darkMode（迁移见 themeMode()）。
    Q_PROPERTY(QString themeMode READ themeMode WRITE setThemeMode NOTIFY changed)
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

    /// 桌面端预警时弹出系统托盘通知气泡。点击通知或托盘图标打开主窗口。
    bool enableDesktopNotification() const { return s_.value("enableDesktopNotification", true).toBool(); }
    void setEnableDesktopNotification(bool v) { set("enableDesktopNotification", v); }

    /// 桌面端预警时把主窗口拉到前台。会抢焦点，默认关闭。
    bool enableWindowRaise() const { return s_.value("enableWindowRaise", false).toBool(); }
    void setEnableWindowRaise(bool v) { set("enableWindowRaise", v); }

    bool backgroundBlur() const { return s_.value("backgroundBlur", true).toBool(); }
    void setBackgroundBlur(bool v) { set("backgroundBlur", v); }

    bool reduceMotion() const { return s_.value("reduceMotion", false).toBool(); }
    void setReduceMotion(bool v) { set("reduceMotion", v); }

    bool isMuted() const { return s_.value("isMuted", false).toBool(); }
    void setIsMuted(bool v) { set("isMuted", v); }

    /// 需要鉴权凭据的源默认关闭：填好凭据后由用户在设置里主动开启，避免无凭据时建立连接。
    static QStringList defaultDisabledSources() { return {QStringLiteral("jian"), QStringLiteral("whews")}; }

    QStringList disabledSources() const {
        if (s_.contains("disabledSources")) return s_.value("disabledSources").toStringList();
        QStringList disabled = defaultDisabledSources();
        // 迁移旧版的两个独立开关（默认均开启）。
        if (!s_.value("enabledWolfx", true).toBool()) disabled << QStringLiteral("wolfx");
        if (!s_.value("enabledPancakes", true).toBool()) disabled << QStringLiteral("pancakes");
        return disabled;
    }
    void setDisabledSources(const QStringList& v) { set("disabledSources", v); }

    /// 某数据源是否启用（未在禁用集合中即启用）。
    Q_INVOKABLE bool isSourceEnabled(const QString& id) const { return !disabledSources().contains(id); }
    Q_INVOKABLE void setSourceEnabled(const QString& id, bool enabled) {
        QStringList disabled = disabledSources();
        if ((!disabled.contains(id)) == enabled) return;
        if (enabled) disabled.removeAll(id); else disabled << id;
        set("disabledSources", disabled);
    }

    QString jianRefreshToken() const { return s_.value("jianRefreshToken", "").toString(); }
    void setJianRefreshToken(const QString& v) { set("jianRefreshToken", v); }
    bool jianConfigured() const { return !jianRefreshToken().isEmpty(); }

    QString whewsToken() const { return s_.value("whewsToken", "").toString(); }
    void setWhewsToken(const QString& v) { set("whewsToken", v.trimmed()); }
    bool whewsConfigured() const { return !whewsToken().isEmpty(); }

    double alertVolume() const { return s_.value("alertVolume", 1.0).toDouble(); }
    void setAlertVolume(double v) { set("alertVolume", v); }

    bool enableNtpSync() const { return s_.value("enableNtpSync", true).toBool(); }
    void setEnableNtpSync(bool v) { set("enableNtpSync", v); }

    /// 自定义 NTP 服务器主机名；留空则使用内置 SNTP 列表。
    QString customNtpServer() const { return s_.value("customNtpServer", "").toString(); }
    void setCustomNtpServer(const QString& v) { set("customNtpServer", v); }

    /// 开发者模式默认关闭。模拟源是唯一消费者，但它自己也在 isConfigured() 里再查一次，
    /// 故即便这里被绕过，没有地址也不会连接。
    bool developerMode() const { return s_.value("developerMode", false).toBool(); }
    void setDeveloperMode(bool v) { set("developerMode", v); }

    /// 留空即未配置：默认值只作为 UI 占位提示，不预填进持久状态，
    /// 这样「开发者模式开但从未配置」仍然是未配置。
    QString simulatedUrl() const { return s_.value("simulatedUrl", "").toString().trimmed(); }
    void setSimulatedUrl(const QString& v) { set("simulatedUrl", v.trimmed()); }

    /// 主题模式：`system`（跟随系统）/ `light` / `dark`。与安卓端同名键同语义。
    /// 优先级：新键 themeMode 一旦存在即为唯一来源（非法值回落 system，不退回旧键）；
    /// 仅有旧布尔 darkMode 时迁移为 dark / light；两者都无（全新安装或恢复默认）时为 system。
    QString themeMode() const {
        if (s_.contains("themeMode")) {
            const QString mode = s_.value("themeMode").toString();
            if (mode == QLatin1String("light") || mode == QLatin1String("dark")) return mode;
            return QStringLiteral("system");
        }
        if (s_.contains("darkMode"))
            return s_.value("darkMode").toBool() ? QStringLiteral("dark") : QStringLiteral("light");
        return QStringLiteral("system");
    }
    void setThemeMode(const QString& v) { set("themeMode", v); }

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
