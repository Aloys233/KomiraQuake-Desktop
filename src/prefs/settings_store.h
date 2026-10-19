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
    /// 在栅格底图之上叠加矢量层（省界 / 区县）；开启时预警按区县烈度着色。
    Q_PROPERTY(bool vectorOverlay READ vectorOverlay WRITE setVectorOverlay NOTIFY changed)
    /// 矢量层中的省界描边；关闭后只保留区县填色。
    Q_PROPERTY(bool showProvinceBorders READ showProvinceBorders WRITE setShowProvinceBorders NOTIFY changed)
    /// 地图叠加层：测站强度点与右上角测站卡片、P/S 波前圆。
    Q_PROPERTY(bool showStationLayer READ showStationLayer WRITE setShowStationLayer NOTIFY changed)
    Q_PROPERTY(bool showWavefront READ showWavefront WRITE setShowWavefront NOTIFY changed)
    Q_PROPERTY(int intensityStandard READ intensityStandard WRITE setIntensityStandard NOTIFY changed)
    Q_PROPERTY(double localIntensityFilter READ localIntensityFilter WRITE setLocalIntensityFilter NOTIFY changed)
    Q_PROPERTY(bool enableWarnings READ enableWarnings WRITE setEnableWarnings NOTIFY changed)
    Q_PROPERTY(bool enableSoundAlert READ enableSoundAlert WRITE setEnableSoundAlert NOTIFY changed)
    /// 提醒矩阵（视觉）：分级事件各自决定是否弹系统通知 / 置顶主窗口。
    /// alert = 通过本地烈度过滤的预警；eew = 其余实时预警；info = 目录地震信息。
    Q_PROPERTY(bool notifyAlert READ notifyAlert WRITE setNotifyAlert NOTIFY changed)
    Q_PROPERTY(bool raiseAlert READ raiseAlert WRITE setRaiseAlert NOTIFY changed)
    Q_PROPERTY(bool notifyEew READ notifyEew WRITE setNotifyEew NOTIFY changed)
    Q_PROPERTY(bool raiseEew READ raiseEew WRITE setRaiseEew NOTIFY changed)
    Q_PROPERTY(bool notifyInfo READ notifyInfo WRITE setNotifyInfo NOTIFY changed)
    Q_PROPERTY(bool raiseInfo READ raiseInfo WRITE setRaiseInfo NOTIFY changed)
    /// 播报内容分轨：首发/终报、更新报、倒计时与抵达、警报音可分别关闭。
    Q_PROPERTY(bool announceEvent READ announceEvent WRITE setAnnounceEvent NOTIFY changed)
    Q_PROPERTY(bool announceUpdate READ announceUpdate WRITE setAnnounceUpdate NOTIFY changed)
    Q_PROPERTY(bool announceCountdown READ announceCountdown WRITE setAnnounceCountdown NOTIFY changed)
    Q_PROPERTY(bool enableAlarmTone READ enableAlarmTone WRITE setEnableAlarmTone NOTIFY changed)
    Q_PROPERTY(bool reduceMotion READ reduceMotion WRITE setReduceMotion NOTIFY changed)
    Q_PROPERTY(bool backgroundBlur READ backgroundBlur WRITE setBackgroundBlur NOTIFY changed)
    Q_PROPERTY(bool isMuted READ isMuted WRITE setIsMuted NOTIFY changed)
    /// 已禁用的数据源 id 列表。采用「禁用集合」而非「启用集合」：未列出的源默认启用，
    /// 新增数据源无需迁移即默认开启（两端一致）。
    Q_PROPERTY(QStringList disabledSources READ disabledSources WRITE setDisabledSources NOTIFY changed)
    /// 已禁用的测站显示源 id 列表（仅桌面端）。空的禁用集合 = 三路测站都开启。
    Q_PROPERTY(QStringList disabledStationSources READ disabledStationSources WRITE setDisabledStationSources NOTIFY changed)
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
    /// 关闭主窗口时收进托盘（默认）；关闭时直接退出应用。
    Q_PROPERTY(bool closeToTray READ closeToTray WRITE setCloseToTray NOTIFY changed)
    Q_PROPERTY(bool autoCheckUpdates READ autoCheckUpdates WRITE setAutoCheckUpdates NOTIFY changed)
    /// 本地震源推算（Beta）：只吃实时测站强度场，不产事件、不参与告警。
    Q_PROPERTY(bool sourceEstimationEnabled READ sourceEstimationEnabled WRITE setSourceEstimationEnabled NOTIFY changed)
    /// 触发阈值（同制式强度）：达到它的测站进入正证据，低于它的邻近站作为删失观测。
    /// 兼容旧键：只有中国区沿用旧 `sourceEstimationTrigger`，其余区域各有独立键。
    Q_PROPERTY(double sourceEstimationTrigger READ sourceEstimationTrigger WRITE setSourceEstimationTrigger NOTIFY changed)
    /// 发布门槛：触发测站数低于它时只显示「收集中」，不给震源。最小可设 1 站
    /// （1–2 站靠未触发站的删失约束定位，见 SourceEstimator::estimate 的说明）。
    Q_PROPERTY(int sourceEstimationMinStations READ sourceEstimationMinStations WRITE setSourceEstimationMinStations NOTIFY changed)
    /// 是否用未触发站做负证据（删失观测）。
    Q_PROPERTY(bool sourceEstimationQuietEvidence READ sourceEstimationQuietEvidence WRITE setSourceEstimationQuietEvidence NOTIFY changed)
    /// 证据消散后结果在地图上保留的秒数（全局：与区域无关）。
    Q_PROPERTY(int sourceEstimationHoldSeconds READ sourceEstimationHoldSeconds WRITE setSourceEstimationHoldSeconds NOTIFY changed)
    /// 触发区合并半径（km）：相距超过它的两片触发区**不会**被拟合成同一个震源。
    Q_PROPERTY(int sourceEstimationClusterRadius READ sourceEstimationClusterRadius WRITE setSourceEstimationClusterRadius NOTIFY changed)
    /// 在地图上画出推定震中（十字 + 不确定度圈）。
    Q_PROPERTY(bool showEstimatedEpicenter READ showEstimatedEpicenter WRITE setShowEstimatedEpicenter NOTIFY changed)

public:
    explicit SettingsStore(QObject* parent = nullptr);

    /// 区域标签（`china` / `japan` / `korea` / `taiwan`）；未知 id 返回 false。
    Q_INVOKABLE bool hasSourceEstimationRegion(const QString& region) const;
    /// 区域展示名；未知 id 回退为空串。
    Q_INVOKABLE QString sourceEstimationRegionLabel(const QString& region) const;

    // 区域参数读写。`region` 为上面的 id；未知 id 时读取返回该字段的全局默认值、
    // 写入被忽略（避免拼错键悄悄落一条谁都不读的记录）。
    //
    // 读取接口在 QML 里必须与同名 Q_PROPERTY 区分开：Qt 不允许一个属性 getter 同时
    // 作为可调用方法暴露，所以这里用 `...For(region)` 后缀；写入接口没有这个限制。

    /// 该区域的震源推算是否启用。
    ///
    /// **默认全部关闭**：未在集合里 = 不参与推算。这与全局总开关的取向一致
    /// （Beta 能力默认关闭，由用户显式打开），也保证「只开某一片台网」是显式选择：
    /// 想只看日本 NIED，就只打开 `japan`，其余三区既不记账也不算。
    ///
    /// 用「启用集合」而不是「禁用集合」正是因为默认必须是关闭 ——
    /// 禁用集合会隐含「未列出的默认开启」，与这里要的语义相反。
    Q_INVOKABLE bool isSourceEstimationRegionEnabled(const QString& region) const;
    Q_INVOKABLE void setSourceEstimationRegionEnabled(const QString& region, bool enabled);
    Q_PROPERTY(QStringList enabledSourceEstimationRegions READ enabledSourceEstimationRegions
                   WRITE setEnabledSourceEstimationRegions NOTIFY changed)
    /// 与 `enabledSourceEstimationRegions` 同源，但拼成逗号串并带 `sourceEstimationRegions`
    /// 前缀：QML 里列表属性变化不一定触发所有绑定重算，字符串拼接是可靠的依赖来源。
    Q_PROPERTY(QString sourceEstimationRegionSignature READ sourceEstimationRegionSignature
                   NOTIFY changed)

    Q_INVOKABLE double sourceEstimationTriggerFor(const QString& region) const;
    Q_INVOKABLE void setSourceEstimationTrigger(const QString& region, double v);
    Q_INVOKABLE int sourceEstimationMinStationsFor(const QString& region) const;
    Q_INVOKABLE void setSourceEstimationMinStations(const QString& region, int v);
    Q_INVOKABLE bool sourceEstimationQuietEvidenceFor(const QString& region) const;
    Q_INVOKABLE void setSourceEstimationQuietEvidence(const QString& region, bool v);
    Q_INVOKABLE int sourceEstimationClusterRadiusFor(const QString& region) const;
    Q_INVOKABLE void setSourceEstimationClusterRadius(const QString& region, int v);
    /// 某区恢复出厂参数（只动该区开关外的参数，不影响其余区域与全局开关）。
    Q_INVOKABLE void resetSourceEstimationRegion(const QString& region);

    QStringList enabledSourceEstimationRegions() const {
        return s_.value("enabledSourceEstimationRegions").toStringList();
    }
    void setEnabledSourceEstimationRegions(const QStringList& v) {
        set("enabledSourceEstimationRegions", v);
    }
    /// 启用集合的稳定签名（排序后逗号拼接）。QML 用它做依赖，避免直接依赖列表属性。
    QString sourceEstimationRegionSignature() const {
        QStringList list = enabledSourceEstimationRegions();
        list.sort();
        return list.join(QLatin1Char(','));
    }
    /// 是否有任何区域被启用（UI 用它决定是否提示「总开关开了但没选区域」）。
    Q_INVOKABLE bool hasAnySourceEstimationRegion() const {
        return !enabledSourceEstimationRegions().isEmpty();
    }

    QString basemapId() const { return s_.value("basemapId", "petal").toString(); }
    void setBasemapId(const QString& v) { set("basemapId", v); }

    QString customBasemapUrl() const { return s_.value("customBasemapUrl", "").toString(); }
    void setCustomBasemapUrl(const QString& v) { set("customBasemapUrl", v); }

    int customBasemapDatum() const { return s_.value("customBasemapDatum", 1).toInt(); } // 0 wgs84, 1 gcj02
    void setCustomBasemapDatum(int v) { set("customBasemapDatum", v); }

    /// 在栅格底图之上叠加矢量层（省界 / 区县着色）。默认关闭：叠加是 opt-in。
    bool vectorOverlay() const { return s_.value("vectorOverlay", false).toBool(); }
    void setVectorOverlay(bool v) { set("vectorOverlay", v); }

    bool showProvinceBorders() const { return s_.value("showProvinceBorders", true).toBool(); }
    void setShowProvinceBorders(bool v) { set("showProvinceBorders", v); }
    bool showStationLayer() const { return s_.value("showStationLayer", true).toBool(); }
    void setShowStationLayer(bool v) { set("showStationLayer", v); }
    bool showWavefront() const { return s_.value("showWavefront", true).toBool(); }
    void setShowWavefront(bool v) { set("showWavefront", v); }

    int intensityStandard() const { return s_.value("intensityStandard", 0).toInt(); } // 0 csis, 1 jma
    void setIntensityStandard(int v) { set("intensityStandard", v); }

    double localIntensityFilter() const { return s_.value("localIntensityFilter", 0.0).toDouble(); }
    void setLocalIntensityFilter(double v) { set("localIntensityFilter", v); }

    /// 地震预警总开关（设置页「地震预警」）：关闭时只展示，不产生任何提醒。
    bool enableWarnings() const { return s_.value("enableWarnings", false).toBool(); }
    void setEnableWarnings(bool v) { set("enableWarnings", v); }

    bool enableSoundAlert() const { return s_.value("enableSoundAlert", true).toBool(); }
    void setEnableSoundAlert(bool v) { set("enableSoundAlert", v); }

    /// 提醒矩阵（视觉）。旧键 enableDesktopNotification / enableWindowRaise 只在
    /// 新键缺失时作为 alert 分级的迁移来源，之后一律以新键为准。
    bool notifyAlert() const {
        if (s_.contains("notifyAlert")) return s_.value("notifyAlert").toBool();
        return s_.value("enableDesktopNotification", true).toBool();
    }
    void setNotifyAlert(bool v) { set("notifyAlert", v); }
    bool raiseAlert() const {
        if (s_.contains("raiseAlert")) return s_.value("raiseAlert").toBool();
        return s_.value("enableWindowRaise", false).toBool();
    }
    void setRaiseAlert(bool v) { set("raiseAlert", v); }
    bool notifyEew() const { return s_.value("notifyEew", false).toBool(); }
    void setNotifyEew(bool v) { set("notifyEew", v); }
    bool raiseEew() const { return s_.value("raiseEew", false).toBool(); }
    void setRaiseEew(bool v) { set("raiseEew", v); }
    bool notifyInfo() const { return s_.value("notifyInfo", false).toBool(); }
    void setNotifyInfo(bool v) { set("notifyInfo", v); }
    bool raiseInfo() const { return s_.value("raiseInfo", false).toBool(); }
    void setRaiseInfo(bool v) { set("raiseInfo", v); }

    /// 播报内容分轨：默认全开；关闭只影响对应片段，不影响其余。
    bool announceEvent() const { return s_.value("announceEvent", true).toBool(); }
    void setAnnounceEvent(bool v) { set("announceEvent", v); }
    bool announceUpdate() const { return s_.value("announceUpdate", true).toBool(); }
    void setAnnounceUpdate(bool v) { set("announceUpdate", v); }
    bool announceCountdown() const { return s_.value("announceCountdown", true).toBool(); }
    void setAnnounceCountdown(bool v) { set("announceCountdown", v); }
    bool enableAlarmTone() const { return s_.value("enableAlarmTone", true).toBool(); }
    void setEnableAlarmTone(bool v) { set("enableAlarmTone", v); }

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

    /// 测站显示源（仅桌面端）：与事件源分开管理，默认全开。
    QStringList disabledStationSources() const {
        return s_.value("disabledStationSources").toStringList();
    }
    void setDisabledStationSources(const QStringList& v) { set("disabledStationSources", v); }
    Q_INVOKABLE bool isStationSourceEnabled(const QString& id) const {
        return !disabledStationSources().contains(id);
    }
    Q_INVOKABLE void setStationSourceEnabled(const QString& id, bool enabled) {
        QStringList disabled = disabledStationSources();
        if ((!disabled.contains(id)) == enabled) return;
        if (enabled) disabled.removeAll(id); else disabled << id;
        set("disabledStationSources", disabled);
    }

    /// 逐源「信息事件震级过滤」：仅作用于目录事件，不影响实时预警链路。
    /// -1 = 不接收；0 = 不过滤；>0 = 低于该震级的信息事件不显示。
    Q_INVOKABLE double infoMagnitudeFilter(const QString& sourceId) const {
        return s_.value(QStringLiteral("minMagnitude.") + sourceId, 0.0).toDouble();
    }
    Q_INVOKABLE void setInfoMagnitudeFilter(const QString& sourceId, double value) {
        set(QStringLiteral("minMagnitude.") + sourceId, value);
    }
    /// 目录事件是否通过逐源震级过滤。未知震级（<= 0）不因阈值被丢弃。
    bool passesInfoMagnitudeFilter(const QString& sourceId, double magnitude) const {
        const double filter = infoMagnitudeFilter(sourceId);
        if (filter < 0.0) return false;
        if (filter <= 0.0) return true;
        if (magnitude <= 0.0) return true;
        return magnitude + 1e-9 >= filter;
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

    /// 关闭主窗口时收进托盘（默认）；关闭时直接退出应用。
    bool closeToTray() const { return s_.value("closeToTray", true).toBool(); }
    void setCloseToTray(bool v) { set("closeToTray", v); }

    /// 启动时后台自动检查一次新版本。
    bool autoCheckUpdates() const { return s_.value("autoCheckUpdates", true).toBool(); }
    void setAutoCheckUpdates(bool v) { set("autoCheckUpdates", v); }

    /// 本地震源推算（Beta）。**默认关闭**：它是 Beta 能力，解尚未在真实地震上校验过，
    /// 默认打开会让一张可能不准的推定震中出现在所有人的地图上；由用户显式开启。
    bool sourceEstimationEnabled() const {
        return s_.value("sourceEstimationEnabled", false).toBool();
    }
    void setSourceEstimationEnabled(bool v) { set("sourceEstimationEnabled", v); }

    /// 触发阈值。默认 3.0：与「有感」量级一致，既不会被常态噪声触发，也能在地震波
    /// 扫过几站后迅速起算。范围由设置页滑块限制在 1–6。
    ///
    /// **按区域取值**：无参版本返回中国区（兼容旧调用），设置页与服务层都走带区域的重载。
    /// 旧版本只有一组全局键 `sourceEstimation{Trigger,MinStations,QuietEvidence,ClusterRadius}`；
    /// 迁移规则见 `.cpp` 里的 `migrateLegacyGlobal`（一次性把旧值抄进中国区，然后删旧键）。
    double sourceEstimationTrigger() const { return sourceEstimationTriggerFor(QStringLiteral("china")); }
    void setSourceEstimationTrigger(double v) { setSourceEstimationTrigger(QStringLiteral("china"), v); }

    /// 发布门槛（触发站数）。默认 6（中国区）：少于这个数时解的空间自由度太大，宁可等待。
    int sourceEstimationMinStations() const {
        return sourceEstimationMinStationsFor(QStringLiteral("china"));
    }
    void setSourceEstimationMinStations(int v) {
        setSourceEstimationMinStations(QStringLiteral("china"), v);
    }

    /// 未触发站负证据。默认开启：没有它，触发的测站只覆盖震中一侧时解会系统性偏移。
    bool sourceEstimationQuietEvidence() const {
        return sourceEstimationQuietEvidenceFor(QStringLiteral("china"));
    }
    void setSourceEstimationQuietEvidence(bool v) {
        setSourceEstimationQuietEvidence(QStringLiteral("china"), v);
    }

    /// 结果保持时长（秒）。默认 60：证据消散后留一段时间供用户查看。**全局**。
    int sourceEstimationHoldSeconds() const {
        return s_.value("sourceEstimationHoldSeconds", 60).toInt();
    }
    void setSourceEstimationHoldSeconds(int v) { set("sourceEstimationHoldSeconds", v); }

    /// 触发区合并半径（km）。默认随区域：中国 250 / 日本 200 / 韩国 200 / 台湾 250。
    int sourceEstimationClusterRadius() const {
        return sourceEstimationClusterRadiusFor(QStringLiteral("china"));
    }
    void setSourceEstimationClusterRadius(int v) {
        setSourceEstimationClusterRadius(QStringLiteral("china"), v);
    }

    /// 地图上画推定震中。默认开启（与推算总开关联动，总开关关闭时一并隐藏）。
    bool showEstimatedEpicenter() const {
        return s_.value("showEstimatedEpicenter", true).toBool();
    }
    void setShowEstimatedEpicenter(bool v) { set("showEstimatedEpicenter", v); }

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
