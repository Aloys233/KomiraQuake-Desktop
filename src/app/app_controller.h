#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointF>
#include <QStringList>
#include <functional>
#include <vector>
#include "core/warning_session.h"
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include "core/event_gate.h"
#include "model/data_source_info.h"
#include "model/event_list_model.h"
#include "model/earthquake_event.h"
#include "prefs/settings_store.h"
#include "service/autostart.h"
#include "service/update_service.h"

namespace komira {
class LocationService;
class EarthquakeSource;
class JianSource;
class WhewsSource;
class SimulatedSource;
class HistoryStore;
class AlertSoundService;
class AlertAnnouncer;
class NtpClock;

/// 面向 QML 的应用控制器：汇聚数据源、去重、历史、告警与 1Hz 倒计时。
class AppController : public QObject {
    Q_OBJECT
    Q_PROPERTY(komira::SettingsStore* settings READ settings CONSTANT)
    /// 开机自启开关（读写操作系统状态）。
    Q_PROPERTY(komira::AutoStartService* autoStart READ autoStart CONSTANT)
    /// 版本与更新检查。
    Q_PROPERTY(komira::UpdateService* updater READ updater CONSTANT)
    /// 本次启动是否应隐藏主窗口（静默启动）。常量：只取启动时读到的设置。
    Q_PROPERTY(bool startHidden READ startHidden CONSTANT)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(QString statusLevelTag READ statusLevelTag NOTIFY statusChanged)
    Q_PROPERTY(bool hasLocation READ hasLocation NOTIFY locationChanged)
    Q_PROPERTY(QString locationName READ locationName NOTIFY locationChanged)
    Q_PROPERTY(double userLatitude READ userLatitude NOTIFY locationChanged)
    Q_PROPERTY(double userLongitude READ userLongitude NOTIFY locationChanged)
    Q_PROPERTY(QString locationStatusText READ locationStatusText NOTIFY locationChanged)
    Q_PROPERTY(QVariantMap sourceInfo READ sourceInfo NOTIFY statusChanged)
    /// 逐数据源的状态列表：`[{ id, name, enabled, status, statusTag, latency, directory, directoryLatency, description }]`。
    Q_PROPERTY(QVariantList sources READ sources NOTIFY statusChanged)
    /// 校时状态：`{ enabled, state, detail }`。设置页展示；地震时间语义走 NtpClock::now()。
    Q_PROPERTY(QVariantMap clockInfo READ clockInfo NOTIFY clockChanged)
    Q_PROPERTY(QVariantList history READ history NOTIFY historyChanged)
    /// 右侧列表数据：目录条目用权威数据，并与实时预警按「发震时刻 + 震中」合并为一条。
    Q_PROPERTY(QVariantList eventList READ eventList NOTIFY eventListChanged)
    /// 稳定的侧栏模型；避免 QML 每次目录事件都替换 JS 数组并重建 delegate 上下文。
    Q_PROPERTY(QAbstractItemModel* eventModel READ eventModel CONSTANT)
    Q_PROPERTY(bool hasWarning READ hasWarning NOTIFY warningChanged)
    /// 当前活动预警是否达到提醒门槛（总开关 + 本地烈度过滤）；未达标只展示、不出现预警卡/倒计时。
    Q_PROPERTY(bool alertEligible READ alertEligible NOTIFY warningChanged)
    Q_PROPERTY(bool warningOverlayVisible READ warningOverlayVisible NOTIFY warningOverlayChanged)
    Q_PROPERTY(QVariant activeWarning READ activeWarning NOTIFY warningChanged)
    Q_PROPERTY(QVariantList activeWarnings READ activeWarnings NOTIFY warningChanged)
    Q_PROPERTY(int countdown READ countdown NOTIFY countdownChanged)
    /// 解析后的实际深色状态：themeMode 为 dark 时恒真；system 时跟随系统配色方案。
    /// 只读——切换主题请写 settings.themeMode。
    Q_PROPERTY(bool darkMode READ darkMode NOTIFY darkModeChanged)
    /// 地图上要画的那一个事件：有焦点用焦点，否则退化为最近一次（`history[0]`）。
    Q_PROPERTY(QVariant mapEvent READ mapEvent NOTIFY mapEventChanged)
    /// 当前是否有地图焦点：地图仅在有关注事件时自动取景，避免默认被「最近事件」抢镜。
    Q_PROPERTY(bool hasMapFocus READ hasMapFocus NOTIFY mapFocusChanged)
    /// 左下角 HUD 要显示的那一个事件：用户主动点击的焦点最优先，其次实时预警，
    /// 再次是仍在 10 分钟窗口内的自动焦点；否则无效（HUD 默认隐藏，点击后才显示）。
    Q_PROPERTY(QVariant hudEvent READ hudEvent NOTIFY hudEventChanged)
    /// HUD 在全部活动事件中的当前位置（0 基）与总数，供左右切换展示 `n/N`。
    Q_PROPERTY(int hudIndex READ hudIndex NOTIFY hudEventChanged)
    Q_PROPERTY(int hudCount READ hudCount NOTIFY hudEventChanged)
    /// P/S 波前圆半径（km），-1 表示不画。
    Q_PROPERTY(QVariantMap waveRadii READ waveRadii NOTIFY waveRadiiChanged)

public:
    explicit AppController(QObject* parent = nullptr, bool startServices = true);
    ~AppController() override;

    SettingsStore* settings() const { return settings_; }
    AutoStartService* autoStart() const { return autoStart_; }
    UpdateService* updater() const { return updater_; }
    bool startHidden() const;

    QString statusText() const;
    QString statusLevelTag() const;
    bool hasLocation() const;
    QString locationName() const;
    double userLatitude() const;
    double userLongitude() const;
    QString locationStatusText() const;
    QVariantMap sourceInfo() const;
    QVariantList sources() const;
    QVariantMap clockInfo() const;
    /// 校时后的当前时刻，固定渲染为 UTC+8 的 `yyyy-MM-dd HH:mm:ss`（与设备时区无关）。
    /// 《NATIVE_PORT_SPEC》 §12。
    Q_INVOKABLE QString clockTextUtc8() const;
    QVariantList history() const;
    QVariantList eventList() const;
    QAbstractItemModel* eventModel() const { return eventModel_; }
    bool hasWarning() const { return hasActiveWarning_; }
    bool alertEligible() const;
    bool warningOverlayVisible() const { return overlayVisible_; }
    /// 无活动预警时返回无效 QVariant（QML 视为 undefined/falsy）。
    QVariant activeWarning() const;
    QVariantList activeWarnings() const;
    int countdown() const { return countdown_; }
    bool darkMode() const { return darkMode_; }
    /// themeMode 变化或系统配色方案变化时重算 darkMode_。
    /// themeMode 为 system 时跟随系统配色方案，其余取字面值。
    bool resolveDarkMode() const {
        const QString mode = settings_ ? settings_->themeMode() : QStringLiteral("light");
        if (mode == QLatin1String("dark")) return true;
        if (mode == QLatin1String("system")) return systemDark_;
        return false;
    }
    QVariant mapEvent() const;
    bool hasMapFocus() const { return hasMapFocus_; }
    QVariant hudEvent() const;
    int hudIndex() const;
    int hudCount() const;
    QVariantMap waveRadii() const;

    /// HUD 左右切换：在全部活动事件间循环（按发震时刻倒序，0 为最新）。
    Q_INVOKABLE void hudPrev();
    Q_INVOKABLE void hudNext();

    /// 列表卡片按钮：命中当前焦点则取消，否则把这次地震设为焦点。
    Q_INVOKABLE void toggleMapFocus(const QString& id);
    Q_INVOKABLE bool isMapFocused(const QString& id) const;

    Q_INVOKABLE void requestLocation();
    Q_INVOKABLE void setManualLocation(double latitude, double longitude, const QString& label = QString());
    /// 主窗口隐藏时暂停目录 HTTP 轮询；实时 WebSocket 连接保持运行。
    Q_INVOKABLE void setWindowVisible(bool visible);
    Q_INVOKABLE void refreshCatalog();
    /// Jian 数据源登录：用登录密钥 `lk_…` 换取刷新令牌并持久化。
    Q_INVOKABLE void loginJian(const QString& loginKey);
    /// 写入 Whews 数据源令牌 `wat_…`（设置页粘贴框），空串表示清除。
    Q_INVOKABLE void setWhewsToken(const QString& token);
    /// 写入模拟数据源地址（设置页），并立即推入活动源使其重连。
    Q_INVOKABLE void setSimulatedUrl(const QString& url);
    /// 立即重新校时一次（不打断周期调度）。
    Q_INVOKABLE void refreshClock();
    Q_INVOKABLE QPointF wgs84ToGcj02(double lat, double lng) const;
    Q_INVOKABLE QPointF gcj02ToWgs84(double lat, double lng) const;
    Q_INVOKABLE void dismissWarningOverlay();
    Q_INVOKABLE void muteWarning();
    Q_INVOKABLE void stopWarning();
    Q_INVOKABLE QString colorForIntensity(double raw) const;
    Q_INVOKABLE QString colorForMagnitude(double magnitude) const;
    Q_INVOKABLE QString severityColor(const QString& levelTag) const;

signals:
    void statusChanged();
    void clockChanged();
    void locationChanged();
    void historyChanged();
    void eventListChanged();
    void warningChanged();
    void warningOverlayChanged();
    void countdownChanged();
    void darkModeChanged();
    void mapEventChanged();
    void mapFocusChanged();
    void hudEventChanged();
    void waveRadiiChanged();
    /// 实时 EEW 到达：请求把地图移到该震中。
    void centerMapRequested(double latitude, double longitude);
    /// 取消焦点（再次点击同一事件）：请求把镜头还原到默认视野。
    void resetMapRequested();

private:
    void wire();
    /// 当前启用的数据源 id 列表（用于聚合状态展示）。
    QStringList enabledSourceIds() const;
    /// 注册表内全部源的链路状态（顺序即展示顺序）。
    QList<DataSourceInfo> allSourceInfos() const;
    /// inDirectory：事件来自 HTTP 目录（进入列表/历史）；否则为 WS 实时预警（只告警不入列表）。
    /// source：发出该帧的源，用于把仓库层的处置结果回传给需要它的数据源（见 EarthquakeSource::onAdmission）。
    void handleEvent(const EarthquakeEvent& event, bool replay, bool inDirectory,
                     EarthquakeSource* source = nullptr);
    /// 同 id 就地更新或追加；避免重启后「DB 载入 + 首次目录轮询」在列表留下重复条目。
    void upsertHistory(const EarthquakeEvent& event);
    void onTick();
    void recomputeEvents();
    void syncWarning();
    WarningSession sessions_;
    std::string selectedWarning_;
    void rebuildHistory();
    /// 地图当前显示的那个事件（焦点优先，否则最近一次）；无事件返回 nullptr。
    const EarthquakeEvent* displayedEvent() const;
    /// manual=true 表示用户主动点击选中；false 表示实时预警自动接管。
    void setMapFocus(const EarthquakeEvent& event, bool manual);
    void clearMapFocus();
    /// 自动焦点事件在最后一报推送后 10 分钟内仍显示 HUD，超出即视为历史事件。
    bool hudWindowOpen(const EarthquakeEvent& event, long long now) const;
    /// 安排一次 HUD 可见性重算（跨过 10 分钟窗口时隐藏自动焦点的 HUD）。
    void scheduleHudRefresh();
    /// 全部活动事件按发震时刻倒序（最新在前）的 identity 列表。
    std::vector<std::string> orderedActiveKeys() const;
    /// HUD 左右切换的公共实现：delta 为 ±1 时循环切换所选预警。
    void shiftHud(int delta);
    /// 焦点事件"还年轻"（发震时刻在窗口内）时跑 100ms 波前定时器，否则停。
    void updateWaveTimer();
    void updateWaveRadii();
    void refreshEventModel();
    void scheduleDirectoryUiRefresh();
    /// 开启自动检查时，本次会话安排一次静默检查更新（只做一次）。
    void maybeAutoCheckUpdates();
    /// 同一地震的跨链路/跨报次标识（用于合并 WS 预警与 HTTP 目录）。
    static QString identityOf(const EarthquakeEvent& event);
    /// 两条报次是否描述同一次地震（发震时刻接近且震中邻近）。
    static bool samePhysicalEvent(const EarthquakeEvent& a, const EarthquakeEvent& b);

    bool startServices_ = true;
    bool directoryRefreshEnabled_ = true;
    std::function<long long()> nowProvider_;
    long long nowMs() const;
    SettingsStore* settings_ = nullptr;
    LocationService* location_ = nullptr;
    HistoryStore* historyStore_ = nullptr;
    /// 数据源注册表：所有源平级、互为备份。接线/启停/聚合都按本列表循环，不逐源硬编码。
    QList<EarthquakeSource*> sources_;
    EventListModel* eventListModel_ = nullptr;
    EventFilterModel* eventModel_ = nullptr;
    /// Jian 源需要登录凭据交换，保留具体类型访问登录/令牌注入入口。
    JianSource* jian_ = nullptr;
    /// Whews 源需注入 `wat_…` 令牌，保留具体类型访问令牌注入入口。
    WhewsSource* whews_ = nullptr;
    /// 模拟源（仅开发自测）需注入开发者模式开关与地址，保留具体类型访问注入入口。
    SimulatedSource* simulated_ = nullptr;
    AlertSoundService* sound_ = nullptr;
    AlertAnnouncer* announcer_ = nullptr;
    /// 网络授时：所有地震时间语义的唯一时间基准。《NATIVE_PORT_SPEC》 §13。
    NtpClock* clock_ = nullptr;
    AutoStartService* autoStart_ = nullptr;
    UpdateService* updater_ = nullptr;
    /// 自动检查更新每次会话只跑一次。
    bool autoUpdateChecked_ = false;

    EventGate gate_;
    QHash<QString, QString> identityToId_;
    QList<EarthquakeEvent> history_;
    EarthquakeEvent activeWarning_;
    bool hasActiveWarning_ = false;
    bool overlayVisible_ = false;
    bool arrivedAnnounced_ = false;
    int countdown_ = 0;
    bool darkMode_ = false;
    /// 系统配色方案是否为深色（themeMode == "system" 时的判定依据）。
    bool systemDark_ = false;
    QTimer countdownTimer_;

    /// 地图焦点：用户从列表选中，或被实时 EEW 自动接管。
    EarthquakeEvent mapFocus_;
    bool hasMapFocus_ = false;
    /// 焦点是否来自用户主动点击（true 时 HUD 不受 10 分钟窗口限制）。
    bool mapFocusManual_ = false;
    QTimer waveTimer_;
    QTimer hudRefreshTimer_;
    QTimer directoryUiTimer_;
    double wavePKm_ = -1.0;
    double waveSKm_ = -1.0;
    double wavePOpacity_ = 0.0;
    double waveSOpacity_ = 0.0;
    /// S 波径向渐变填充的不透明度（仅影响半径内非零）。
    double waveSFillOpacity_ = 0.0;

    /// 波前圆参数。《NATIVE_PORT_SPEC》 §12：>2000 km 切 `jb` 表，>10000 km 不再画。
    static constexpr double kWaveTableSwitchKm = 2000.0;
    static constexpr double kWaveMaxRadiusKm = 10000.0;
    /// 波前隐去的烈度阈值（CSIS I：可感下限）。
    static constexpr double kCsisFadeLevel = 1.0;
    static constexpr long long kWaveWindowMs = 60LL * 60LL * 1000LL;
    /// 自动焦点转为历史事件后仍展示 HUD 的宽限期（以最后一报推送时间为准）。
    static constexpr long long kHudWindowMs = 10LL * 60LL * 1000LL;
};

} // namespace komira
