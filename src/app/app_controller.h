#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointF>
#include <functional>
#include "core/warning_session.h"
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include "core/event_gate.h"
#include "model/earthquake_event.h"
#include "prefs/settings_store.h"
#include "service/autostart.h"
#include "service/update_service.h"

namespace komira {
class LocationService;
class WolfxSource;
class HistoryStore;
class AlertSoundService;
class SpeechService;
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
    /// 校时状态：`{ enabled, state, detail }`。设置页展示；地震时间语义走 NtpClock::now()。
    Q_PROPERTY(QVariantMap clockInfo READ clockInfo NOTIFY clockChanged)
    Q_PROPERTY(QVariantList history READ history NOTIFY historyChanged)
    /// 右侧列表数据：目录条目用权威数据，并与实时预警按「发震时刻 + 震中」合并为一条。
    Q_PROPERTY(QVariantList eventList READ eventList NOTIFY eventListChanged)
    Q_PROPERTY(bool hasWarning READ hasWarning NOTIFY warningChanged)
    Q_PROPERTY(bool warningOverlayVisible READ warningOverlayVisible NOTIFY warningOverlayChanged)
    Q_PROPERTY(QVariant activeWarning READ activeWarning NOTIFY warningChanged)
    Q_PROPERTY(QVariantList activeWarnings READ activeWarnings NOTIFY warningChanged)
    Q_PROPERTY(int countdown READ countdown NOTIFY countdownChanged)
    Q_PROPERTY(bool speechAvailable READ speechAvailable CONSTANT)
    Q_PROPERTY(bool darkMode READ darkMode WRITE setDarkMode NOTIFY darkModeChanged)
    /// 地图上要画的那一个事件：有焦点用焦点，否则退化为最近一次（`history[0]`）。
    Q_PROPERTY(QVariant mapEvent READ mapEvent NOTIFY mapEventChanged)
    /// 当前是否有地图焦点：地图仅在有关注事件时自动取景，避免默认被「最近事件」抢镜。
    Q_PROPERTY(bool hasMapFocus READ hasMapFocus NOTIFY mapFocusChanged)
    /// 左下角 HUD 要显示的那一个事件：用户主动点击的焦点最优先，其次实时预警，
    /// 再次是仍在 10 分钟窗口内的自动焦点；否则无效（HUD 默认隐藏，点击后才显示）。
    Q_PROPERTY(QVariant hudEvent READ hudEvent NOTIFY hudEventChanged)
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
    QVariantMap clockInfo() const;
    /// 校时后的当前时刻，固定渲染为 UTC+8 的 `yyyy-MM-dd HH:mm:ss`（与设备时区无关）。
    /// 《NATIVE_PORT_SPEC》 §12。
    Q_INVOKABLE QString clockTextUtc8() const;
    QVariantList history() const;
    QVariantList eventList() const;
    bool hasWarning() const { return hasActiveWarning_; }
    bool warningOverlayVisible() const { return overlayVisible_; }
    /// 无活动预警时返回无效 QVariant（QML 视为 undefined/falsy）。
    QVariant activeWarning() const;
    QVariantList activeWarnings() const;
    int countdown() const { return countdown_; }
    bool speechAvailable() const;
    bool darkMode() const { return darkMode_; }
    void setDarkMode(bool dark);
    QVariant mapEvent() const;
    bool hasMapFocus() const { return hasMapFocus_; }
    QVariant hudEvent() const;
    QVariantMap waveRadii() const;

    /// 列表卡片按钮：命中当前焦点则取消，否则把这次地震设为焦点。
    Q_INVOKABLE void toggleMapFocus(const QString& id);
    Q_INVOKABLE bool isMapFocused(const QString& id) const;

    Q_INVOKABLE void requestLocation();
    Q_INVOKABLE void setManualLocation(double latitude, double longitude, const QString& label = QString());
    Q_INVOKABLE void refreshCatalog();
    Q_INVOKABLE QPointF wgs84ToGcj02(double lat, double lng) const;
    Q_INVOKABLE QPointF gcj02ToWgs84(double lat, double lng) const;
    Q_INVOKABLE void dismissWarningOverlay();
    Q_INVOKABLE void muteWarning();
    Q_INVOKABLE void stopWarning();
    Q_INVOKABLE void sampleSpeech();
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
    /// inDirectory：事件来自 HTTP 目录（进入列表/历史）；否则为 WS 实时预警（只告警不入列表）。
    void handleEvent(const EarthquakeEvent& event, bool replay, bool inDirectory);
    /// 同 id 就地更新或追加；避免重启后「DB 载入 + 首次目录轮询」在列表留下重复条目。
    void upsertHistory(const EarthquakeEvent& event);
    void onTick();
    void recomputeEvents();
    void syncWarning();
    WarningSession sessions_;
    std::string selectedWarning_;
    void rebuildHistory();
    bool passesMagnitudeFilter(const EarthquakeEvent& event) const;
    /// 地图当前显示的那个事件（焦点优先，否则最近一次）；无事件返回 nullptr。
    const EarthquakeEvent* displayedEvent() const;
    /// manual=true 表示用户主动点击选中；false 表示实时预警自动接管。
    void setMapFocus(const EarthquakeEvent& event, bool manual);
    void clearMapFocus();
    /// 自动焦点事件在最后一报推送后 10 分钟内仍显示 HUD，超出即视为历史事件。
    bool hudWindowOpen(const EarthquakeEvent& event, long long now) const;
    /// 安排一次 HUD 可见性重算（跨过 10 分钟窗口时隐藏自动焦点的 HUD）。
    void scheduleHudRefresh();
    /// 焦点事件"还年轻"（发震时刻在窗口内）时跑 100ms 波前定时器，否则停。
    void updateWaveTimer();
    void updateWaveRadii();
    /// 开启自动检查时，本次会话安排一次静默检查更新（只做一次）。
    void maybeAutoCheckUpdates();
    /// 同一地震的跨链路/跨报次标识（用于合并 WS 预警与 HTTP 目录）。
    static QString identityOf(const EarthquakeEvent& event);
    /// 两条报次是否描述同一次地震（发震时刻接近且震中邻近）。
    static bool samePhysicalEvent(const EarthquakeEvent& a, const EarthquakeEvent& b);

    bool startServices_ = true;
    std::function<long long()> nowProvider_;
    long long nowMs() const;
    SettingsStore* settings_ = nullptr;
    LocationService* location_ = nullptr;
    HistoryStore* historyStore_ = nullptr;
    WolfxSource* source_ = nullptr;
    AlertSoundService* sound_ = nullptr;
    SpeechService* speech_ = nullptr;
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
    QTimer countdownTimer_;

    /// 地图焦点：用户从列表选中，或被实时 EEW 自动接管。
    EarthquakeEvent mapFocus_;
    bool hasMapFocus_ = false;
    /// 焦点是否来自用户主动点击（true 时 HUD 不受 10 分钟窗口限制）。
    bool mapFocusManual_ = false;
    QTimer waveTimer_;
    QTimer hudRefreshTimer_;
    double wavePKm_ = -1.0;
    double waveSKm_ = -1.0;

    /// 波前圆参数。《NATIVE_PORT_SPEC》 §12：>2000 km 切 `jb` 表，>10000 km 不再画。
    static constexpr double kWaveTableSwitchKm = 2000.0;
    static constexpr double kWaveMaxRadiusKm = 10000.0;
    static constexpr long long kWaveWindowMs = 60LL * 60LL * 1000LL;
    /// 自动焦点转为历史事件后仍展示 HUD 的宽限期（以最后一报推送时间为准）。
    static constexpr long long kHudWindowMs = 10LL * 60LL * 1000LL;
};

} // namespace komira
