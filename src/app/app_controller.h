#pragma once

#include <QFutureWatcher>
#include <QHash>
#include <QList>
#include <QObject>
#include <map>
#include <QPointF>
#include <QStringList>
#include <functional>
#include <memory>
#include <vector>
#include "core/warning_session.h"
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include "core/event_gate.h"
#include "core/vector_map_data.h"
#include "model/data_source_info.h"
#include "model/event_list_model.h"
#include "model/earthquake_event.h"
#include "model/station_snapshot.h"
#include "prefs/settings_store.h"
#include "service/autostart.h"
#include "service/update_service.h"

namespace komira {
class LocationService;
class EarthquakeSource;
class JianSource;
class WhewsSource;
class SimulatedSource;
class KmoniStationSource;
class SnetStationSource;
class KmaStationSource;
class PancakesSeisSource;
class WolfxSeisSource;
class HistoryStore;
class AlertSoundService;
class AlertAnnouncer;
class NtpClock;
class SourceEstimationService;

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
    Q_PROPERTY(bool hasLocation READ hasLocation NOTIFY locationChanged)
    Q_PROPERTY(QString locationName READ locationName NOTIFY locationChanged)
    Q_PROPERTY(double userLatitude READ userLatitude NOTIFY locationChanged)
    Q_PROPERTY(double userLongitude READ userLongitude NOTIFY locationChanged)
    Q_PROPERTY(QString locationStatusText READ locationStatusText NOTIFY locationChanged)
    /// 逐数据源的状态列表：`[{ id, name, enabled, configured, status, statusTag, latency, directory, directoryLatency, description }]`。
    /// 左下角徽章据此逐源着色（只取 enabled && configured）。
    Q_PROPERTY(QVariantList sources READ sources NOTIFY statusChanged)
    /// 校时状态：`{ enabled, state, detail }`。设置页展示；地震时间语义走 NtpClock::now()。
    Q_PROPERTY(QVariantMap clockInfo READ clockInfo NOTIFY clockChanged)
    Q_PROPERTY(QVariantList history READ history NOTIFY historyChanged)
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
    ///
    /// 5 个标量各自成 Q_PROPERTY 而不是一张 QVariantMap：旧实现每个 16 ms tick 都新建一张
    /// map，QML 里 7 处绑定各自读它 → 每帧 7 次 map 构造 + 全量重绑（值没变也重绑）。
    /// 标量属性只在**值真的变化**时才发通知。
    Q_PROPERTY(double wavePKm READ wavePKm NOTIFY waveRadiiChanged)
    Q_PROPERTY(double waveSKm READ waveSKm NOTIFY waveRadiiChanged)
    Q_PROPERTY(double wavePOpacity READ wavePOpacity NOTIFY waveRadiiChanged)
    Q_PROPERTY(double waveSOpacity READ waveSOpacity NOTIFY waveRadiiChanged)
    Q_PROPERTY(double waveSFillOpacity READ waveSFillOpacity NOTIFY waveRadiiChanged)
    /// 区县名 -> CSIS 烈度原始值，供矢量地图按烈度给区县面填色。
    Q_PROPERTY(QVariantMap districtIntensity READ districtIntensity NOTIFY districtIntensityChanged)
    /// 实时测站震度的**快照对象**（**仅显示，不参与预警**）：合并各测站源，地图项直接读
    /// C++ vector。指针恒定，内容更新走 StationSnapshot::changed()，不经 QVariantList/JS 转换。
    Q_PROPERTY(QObject* stationSnapshot READ stationSnapshot CONSTANT)
    /// 逐测站源的实时最大值卡片数据：`[{ id, name, caption, hasData, raw, text, stationCount, frameTime }]`。
    /// 逐测站源的实时最大值卡片数据。**缓存**在 `stationSourcesCache_`：只有内容真变了
    /// 才重建并通知（旧实现每次读属性都为 5 路源重算文本、时间格式与颜色，
    /// 而 QML 里每个 delegate 各读一次 = 每 100 ms 十几张 map）。
    Q_PROPERTY(QVariantList stationSources READ stationSources NOTIFY stationIntensityChanged)
    /// 本地震源推算（Beta）：只吃实时测站强度场，不产事件、不参与告警。
    /// `{ state, enabled, visible, latitude, longitude, depthKm, magnitude, family,
    ///    stationCount, quietStations, uncertaintyKm, confidence, grade, ... }`
    Q_PROPERTY(QVariantMap sourceEstimateInfo READ sourceEstimateInfo NOTIFY sourceEstimateChanged)
    /// 地图叠加层载荷；`{ visible, latitude, longitude, uncertaintyKm, family, magnitude, ... }`
    Q_PROPERTY(QVariantMap sourceEstimateMarker READ sourceEstimateMarker NOTIFY sourceEstimateChanged)

public:
    explicit AppController(QObject* parent = nullptr, bool startServices = true);
    ~AppController() override;

    SettingsStore* settings() const { return settings_; }
    AutoStartService* autoStart() const { return autoStart_; }
    UpdateService* updater() const { return updater_; }
    bool startHidden() const;

    bool hasLocation() const;
    QString locationName() const;
    double userLatitude() const;
    double userLongitude() const;
    QString locationStatusText() const;
    /// 逐源状态列表的快照（**缓存**，见 rebuildSources()）。
    QVariantList sources() const { return sourcesCache_; }
    QVariantMap clockInfo() const;
    /// 校时后的当前时刻，固定渲染为 UTC+8 的 `yyyy-MM-dd HH:mm:ss`（与设备时区无关）。
    /// 《NATIVE_PORT_SPEC》 §12。
    Q_INVOKABLE QString clockTextUtc8() const;
    /// 历史事件数组（`toMap` 全字段）。**懒构造 + 缓存**：QML 已不再读它，
    /// 旧实现却每次目录刷新都重建 200 张 QVariantMap 并 emit。
    QVariantList history() const;
    /// 兼容入口：完整事件数组（含全部字段）。QML 已改用 `eventModel` 的逐列 role，
    /// 这里仅供 C++ 测试/调试调用，且**不再**随每次刷新发 `eventListChanged`
    /// —— 那曾让每次目录更新都重建一份约 200×40 项的 QVariant 树，而 QML 根本没人读。
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
    double wavePKm() const { return wavePKm_; }
    double waveSKm() const { return waveSKm_; }
    double wavePOpacity() const { return wavePOpacity_; }
    double waveSOpacity() const { return waveSOpacity_; }
    double waveSFillOpacity() const { return waveSFillOpacity_; }
    QVariantMap districtIntensity() const { return districtIntensity_; }
    QObject* stationSnapshot() const { return stationSnapshot_; }
    QVariantList stationSources() const;
    QVariantMap sourceEstimateInfo() const;
    QVariantMap sourceEstimateMarker() const;

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
    /// 分级提醒：tier ∈ {alert, eew, info}。alert = 通过本地烈度过滤的预警，
    /// eew = 其余实时预警，info = 新入库的目录地震信息。托盘按设置决定通知/置顶。
    void notifiableEvent(const QVariantMap& event, const QString& tier);
    void statusChanged();
    void clockChanged();
    void locationChanged();
    void historyChanged();
    void warningChanged();
    void warningOverlayChanged();
    void countdownChanged();
    void darkModeChanged();
    void mapEventChanged();
    void mapFocusChanged();
    void hudEventChanged();
    void waveRadiiChanged();
    void districtIntensityChanged();
    void stationIntensityChanged();
    void sourceEstimateChanged();
    /// 实时 EEW 到达：请求把地图移到该震中。
    void centerMapRequested(double latitude, double longitude);
    /// 取消焦点（再次点击同一事件）：请求把镜头还原到默认视野。
    void resetMapRequested();

private:
    void wire();
    /// 逐项接线（设置变化 → 只跑真正相关的那一段）。
    void wireSettings();
    /// 数据源/凭据/开发者模式变化：重推凭据并按门控重启或停止各源。
    void applySourceSettings();
    /// inDirectory：事件来自 HTTP 目录（进入列表/历史）；否则为 WS 实时预警（只告警不入列表）。
    /// source：发出该帧的源，用于把仓库层的处置结果回传给需要它的数据源（见 EarthquakeSource::onAdmission）。
    void handleEvent(const EarthquakeEvent& event, bool replay, bool inDirectory,
                     EarthquakeSource* source = nullptr);
    /// 同 id 就地更新或追加；避免重启后「DB 载入 + 首次目录轮询」在列表留下重复条目。
    /// 写入内存历史；返回持久化字段是否真的变化（未变则无需写库）。
    bool upsertHistory(const EarthquakeEvent& event);
    /// 把事件排进下一批写库；同 id 去重。
    void queueHistoryWrite(const EarthquakeEvent& event);
    /// 一次事务写完全部待写事件，并 prune 一次；随后合并落盘墓碑。
    void flushHistoryWrites();
    /// 排入墓碑写回（同 identity 覆盖）。预警结束/取消是低频事件，但每次都要 3 条 SQL，
    /// 合并进同一条 80 ms 写回可避免 GUI 线程在倒计时期间做同步事务。
    void queueTombstoneWrite(const QString& identity, long long endedAt);
    /// 把待写墓碑落盘（合并调用）。
    void flushTombstoneWrites();
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
    /// 重建 `stationSourcesCache_`；内容未变则返回 false（调用方据此决定是否发信号）。
    bool rebuildStationCards();
    /// 重建 `sourcesCache_`；内容未变则返回 false。
    bool rebuildSources();
    /// 数据源状态变化：合并到一次 250 ms 的单次定时器里再通知 UI。
    ///
    /// Wolfx 的每一条 WS 报文都会 `emit infoChanged()`（其中多数只更新 lastHeartbeat/
    /// latency）。旧实现把 infoChanged 直接接到 `statusChanged`，于是每帧报文都让 QML
    /// 侧的 `app.sources`（一个 QVariantList 属性）整体重建 + 全部绑定重算 —— 推流
    /// 频繁时这条路径比数据解析本身还贵。
    void scheduleStatusRefresh();
    /// 将高频测站帧合并为固定频率的快照刷新，避免每条站点帧都重建整张表。
    void scheduleStationIntensityRefresh();
    /// 按各测站源的最新读数重建 StationSnapshot（纯 POD 拷贝，不发 QML 信号）。
    void refreshStationSnapshot();
    /// 台站表（首包/名录/启用切换）变化：标脏元数据并安排一次刷新。
    /// 震度帧只更新数值，不触发元数据重建 —— 否则每 100 ms 要重建约 2500 条
    /// QString（五路台站表）。
    void noteStationTablesChanged();
    /// 全部活动事件按发震时刻倒序（最新在前）的 identity 列表。
    std::vector<std::string> orderedActiveKeys() const;
    /// HUD 左右切换的公共实现：delta 为 ±1 时循环切换所选预警。
    void shiftHud(int delta);
    /// 焦点事件"还年轻"（发震时刻在窗口内）时跑 100ms 波前定时器，否则停。
    void updateWaveTimer();
    void updateWaveRadii();
    /// 安排一次区县烈度重算：合并 80 ms 内的多次事件变更，只算最后一��。
    void scheduleDistrictRefresh();
    /// 在当前焦点事件下派发一次区县烈度重算。计算本体跑在 worker 线程；已有任务在跑时
    /// 只记待办，等它结束后再按届时最新状态算一次，保证任一时刻只有一个任务读矢量数据。
    void recomputeDistrictIntensity();
    /// worker 线程算完回到主线程应用结果；期间有新请求则按最新状态再算一次。
    void onDistrictComputed();
    /// 应用一次结果：与当前值相同则不重复发信号。
    void applyDistrictIntensity(QVariantMap next);
    /// 在给定事件参数下算出「区县名 -> CSIS 烈度」，纯函数、不触碰任何 QObject，
    /// 故可安全地在 worker 线程调用（矢量数据以不可变快照传入）。
    static QVariantMap computeDistrictIntensity(const VectorMapData& data, double magnitude,
                                                double depthKm, double lat, double lon);
    void refreshEventModel();
    /// 侧栏当前应显示的事件（目录 + 实时预警合并、按发震时刻倒序）及 isActive 标记。
    /// 供 `eventList()`（兼容调试/测试）与 `refreshEventModel()`（行模型）共用一次合并。
    std::vector<std::pair<EarthquakeEvent, bool>> mergedEvents() const;
    /// 把一条事件装进侧栏行（只填 delegate 真正会读的字段）。
    EventListModel::Row makeRow(const EarthquakeEvent& event, bool active) const;
    /// 焦点变化后只刷新受影响的两行（旧的与新的），不重建整表。
    void syncEventModelFocus();
    void scheduleDirectoryUiRefresh();
    /// 开启自动检查时，本次会话安排一次静默检查更新（只做一次）。
    void maybeAutoCheckUpdates();
    /// 同一地震的跨链路/跨报次标识（用于合并 WS 预警与 HTTP 目录）。
    static QString identityOf(const EarthquakeEvent& event);
    /// 两条报次是否描述同一次地震（发震时刻接近且震中邻近）。
    static bool samePhysicalEvent(const EarthquakeEvent& a, const EarthquakeEvent& b);
    /// 是否为触发本地烈度限制且有倒计时的受保护关键预警
    bool isProtectedAlert(const EarthquakeEvent& e, long long clockNow) const;
    bool isProtectedAlert(const EarthquakeEvent& e) const { return isProtectedAlert(e, nowMs()); }

    bool startServices_ = true;
    bool directoryRefreshEnabled_ = true;
    std::function<long long()> nowProvider_;
    long long nowMs() const;
    QString assetRoot_;
    SettingsStore* settings_ = nullptr;
    LocationService* location_ = nullptr;
    HistoryStore* historyStore_ = nullptr;
    /// 数据源注册表：所有源平级、互为备份。接线/启停/聚合都按本列表循环，不逐源硬编码。
    QList<EarthquakeSource*> sources_;
    EventListModel* eventListModel_ = nullptr;
    EventFilterModel* eventModel_ = nullptr;
    /// 上次写入行模型的焦点 id：焦点变化时只需刷这两行（旧的、新的）。
    QString focusedEventId_;
    /// Jian 源需要登录凭据交换，保留具体类型访问登录/令牌注入入口。
    JianSource* jian_ = nullptr;
    /// Whews 源需注入 `wat_…` 令牌，保留具体类型访问令牌注入入口。
    WhewsSource* whews_ = nullptr;
    /// 模拟源（仅开发自测）需注入开发者模式开关与地址，保留具体类型访问注入入口。
    SimulatedSource* simulated_ = nullptr;
    /// NIED Kmoni 实时测站显示源（非 EEW，不入 sources_ 事件注册表）。
    KmoniStationSource* stationSource_ = nullptr;
    /// S-Net 海底观测网实时测站显示源（非 EEW）。
    SnetStationSource* snetSource_ = nullptr;
    /// KMA PEWS 实时测站显示源（非 EEW）。
    KmaStationSource* kmaSource_ = nullptr;
    /// PancakesSeis 实时测站显示源（非 EEW，不入 sources_ 事件注册表）。
    PancakesSeisSource* seisSource_ = nullptr;
    /// Wolfx SeisJS 实时测站显示源（非 EEW，不入 sources_ 事件注册表）。
    WolfxSeisSource* wolfxSeisSource_ = nullptr;
    /// 本地震源推算（Beta）：消费上面五路测站强度，不产事件、不参与告警。
    SourceEstimationService* sourceEstimation_ = nullptr;
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
    QTimer stationIntensityRefreshTimer_;
    StationSnapshot* stationSnapshot_ = nullptr;
    /// 悬停浮层元数据（与 stationSnapshot_ 的 entries 下标对齐），按台站表变化重建。
    StationSnapshot::MetaList stationMetas_;
    /// `stationSources` 属性背后的缓存（见 Q_PROPERTY 注释）。
    QVariantList stationSourcesCache_;
    /// `sources` 属性背后的缓存（同上）。
    QVariantList sourcesCache_;
    /// `history` 属性背后的缓存：`historyDirty_` 为真时才重建。
    mutable QVariantList historyCache_;
    mutable bool historyDirty_ = true;
    /// 数据源状态合并定时器（见 scheduleStatusRefresh）。
    QTimer statusRefreshTimer_;
    /// 台站表变化后置脏：下一次 refreshStationSnapshot 连元数据一起替换。
    bool stationMetaDirty_ = true;
    /// 目录同步会把上百条事件一条条送进来；逐条写库 + 逐条 prune 会在启动/复显时
    /// 阻塞 GUI 线程一秒以上（实测 200 条约 400 ms，慢盘更久）。合并成一次事务。
    QTimer historyFlushTimer_;
    std::map<std::string, EarthquakeEvent> pendingHistoryWrites_;
    /// 待落盘的墓碑：identity -> endedAt。
    std::map<std::string, long long> pendingTombstones_;

    /// 地图焦点：用户从列表选中，或被实时 EEW 自动接管。
    EarthquakeEvent mapFocus_;
    bool hasMapFocus_ = false;
    /// 焦点是否来自用户主动点击（true 时 HUD 不受 10 分钟窗口限制）。
    bool mapFocusManual_ = false;
    QTimer waveTimer_;
    QTimer hudRefreshTimer_;
    QTimer directoryUiTimer_;
    /// 区县烈度重算：80 ms 合并定时器 + 后台任务 + 最近一次结果。
    QTimer districtTimer_;
    QFutureWatcher<QVariantMap> districtWatcher_;
    /// 后台任务是否在跑；在跑时新请求只置 pending，保证任一时刻只有一个任务读矢量数据。
    bool districtRunning_ = false;
    bool districtPending_ = false;
    QVariantMap districtIntensity_;
    /// 区县矢量数据（首次需要着色时加载 3.7 MiB）。加载后不再变更，worker 线程按
    /// shared_ptr 读取 —— 即便 AppController 先析构，任务也不会读到悬垂数据。
    std::shared_ptr<const VectorMapData> districtData_;
    double wavePKm_ = -1.0;
    double waveSKm_ = -1.0;
    double wavePOpacity_ = 0.0;
    double waveSOpacity_ = 0.0;
    /// S 波径向渐变填充的不透明度（仅影响半径内非零）。
    double waveSFillOpacity_ = 0.0;
    /// 波前「影响半径」缓存：只随焦点的震级/深度变化而失效（见 updateWaveRadii）。
    double waveCacheMagnitude_ = -1.0;
    double waveCacheDepth_ = -1.0;
    double waveCacheFadeKm_ = 0.0;

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
