#include "app/app_controller.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QList>
#include <QStandardPaths>
#include <QStringList>
#include <QStyleHints>
#include <QTimeZone>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>

#include "core/coordinate_transform.h"
#include "core/intensity_calculator.h"
#include "core/ip_geo_lookup.h"
#include "core/quake_calculator.h"
#include "core/region_names.h"
#include "core/travel_time_service.h"
#include "model/station_snapshot.h"
#include "prefs/settings_store.h"
#include "service/source_estimation_service.h"
#include "service/alert_announcer.h"
#include "service/alert_sound_service.h"
#include "service/autostart.h"
#include "service/location_service.h"
#include "service/ntp_clock.h"
#include "service/sentry_service.h"
#include "service/update_service.h"
#include "source/eew_parser.h"
#include "source/earthquake_source.h"
#include "source/jian_source.h"
#include "source/kma_station_protocol.h"
#include "source/kma_station_source.h"
#include "source/kmoni_station_protocol.h"
#include "source/kmoni_station_source.h"
#include "source/pancakes_seis_protocol.h"
#include "source/pancakes_seis_source.h"
#include "source/pancakes_source.h"
#include "source/simulated_source.h"
#include "source/snet_station_protocol.h"
#include "source/snet_station_source.h"
#include "source/whews_source.h"
#include "source/wolfx_seis_protocol.h"
#include "source/wolfx_seis_source.h"
#include "source/wolfx_source.h"
#include "store/history_store.h"
#include "theme/seismic_colors.h"

namespace komira {

namespace {

/// 校时时钟固定显示时区：UTC+8。《NATIVE_PORT_SPEC》 §12。
constexpr int kClockUtc8OffsetSeconds = 8 * 3600;

/// 只比较会落库的字段：目录每次整表重发，字段没变就不必再写一次库。
bool sameStoredEvent(const EarthquakeEvent& a, const EarthquakeEvent& b) {
    return a.eventId == b.eventId && a.magnitude == b.magnitude && a.latitude == b.latitude &&
           a.longitude == b.longitude && a.depth == b.depth && a.location == b.location &&
           a.timestamp == b.timestamp && a.reportTime == b.reportTime && a.source == b.source &&
           a.sourceProvider == b.sourceProvider && a.sourceAgency == b.sourceAgency &&
           a.distanceKm == b.distanceKm && a.estimatedIntensity == b.estimatedIntensity &&
           a.rawIntensity == b.rawIntensity && a.maxIntensityRaw == b.maxIntensityRaw &&
           a.maxIntensityText == b.maxIntensityText && a.pWaveArrival == b.pWaveArrival &&
           a.sWaveArrival == b.sWaveArrival && a.warningLevel == b.warningLevel &&
           a.reportNum == b.reportNum && a.isFinal == b.isFinal && a.isCanceled == b.isCanceled;
}

QVariantMap toMap(const EarthquakeEvent& e, IntensityStandard standard = IntensityStandard::Csis) {
    QVariantMap m;
    m["id"] = QString::fromStdString(e.identity());
    m["magnitude"] = e.magnitude;
    m["magnitudeText"] = QString::number(e.magnitude, 'f', 1);
    m["latitude"] = e.latitude;
    m["longitude"] = e.longitude;
    m["depth"] = e.depth;
    m["depthText"] = QString::number(e.depth, 'f', 0);
    m["location"] = QString::fromStdString(e.location);
    m["source"] = QString::fromStdString(e.source);
    m["timestamp"] = static_cast<qlonglong>(e.timestamp);
    // 发震时刻：与校时时钟一致，固定渲染为 UTC+8 的完整时刻。
    m["timeText"] = QDateTime::fromMSecsSinceEpoch(e.timestamp, QTimeZone(kClockUtc8OffsetSeconds))
                        .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    // 来源标注：`<数据源提供方>·<报数机构>`，如 Wolfx·CENC；新增数据源沿用同一格式。
    const QString provider = QString::fromStdString(e.sourceProvider);
    const QString agency = QString::fromStdString(e.sourceAgency);
    m["sourceTag"] = agency.isEmpty() ? provider : provider + QStringLiteral("·") + agency;
    const bool hasDistance = e.distanceKm >= 0.0;
    m["distance"] = hasDistance ? e.distanceKm : -1.0;
    m["distanceText"] = hasDistance ? QString::number(e.distanceKm, 'f', 0) : QStringLiteral("--");
    m["hasDistance"] = hasDistance;
    // 烈度：有定位用本地预估，否则退回数据源报的最大烈度（HUD / 全屏预警沿用本组字段）。
    const QString localText = QString::fromStdString(e.estimatedIntensity);
    const QString maxText = QString::fromStdString(e.maxIntensityText);
    const bool hasLocalIntensity =
        hasDistance && !localText.isEmpty() && localText != QLatin1String("--");
    const bool hasMaxIntensity = e.maxIntensityRaw > 0.0 || !maxText.isEmpty();
    QString intensityText = QStringLiteral("--");
    if (hasLocalIntensity) intensityText = localText;
    else if (hasMaxIntensity) intensityText = maxText;

    m["intensity"] = intensityText;
    m["intensityLabel"] = hasLocalIntensity ? QStringLiteral("预估烈度") : QStringLiteral("最大烈度");
    m["intensityIsLocal"] = hasLocalIntensity;
    m["estimatedIntensity"] = localText;
    m["maxIntensity"] = maxText;
    m["rawIntensity"] = e.rawIntensity;
    m["maxIntensityRaw"] = e.maxIntensityRaw;

    // 列表徽章：固定展示「震源最大烈度」（震中当地量），不随定位变化；
    // 源报缺失时在震中（距离 0）按衰减关系估算，保证列表总有可读烈度。
    double listRaw = e.maxIntensityRaw;
    QString listText = maxText;
    if (!hasMaxIntensity) {
        listRaw = IntensityCalculator::rawCsis(e.magnitude, 0.0, e.depth);
        listText = standard == IntensityStandard::Jma
                       ? QString::fromStdString(
                             IntensityCalculator::formatJma(e.magnitude, 0.0, e.depth))
                       : QString::fromStdString(IntensityCalculator::formatCsis(listRaw));
    }
    m["listIntensity"] = listText.isEmpty() ? QStringLiteral("--") : listText;
    m["listIntensityLabel"] = QStringLiteral("最大烈度");
    m["listIntensityColor"] = SeismicColors::intensityColor(listRaw).name();
    m["levelTag"] = QString::fromLatin1(warningTag(e.warningLevel));
    m["levelCode"] = warningCode(e.warningLevel);
    m["reportNum"] = e.reportNum;
    m["isFinal"] = e.isFinal;
    m["isCanceled"] = e.isCanceled;
    m["severityColor"] = SeismicColors::severity(e.warningLevel, false).name();
    m["intensityColor"] =
        SeismicColors::intensityColor(hasLocalIntensity ? e.rawIntensity : e.maxIntensityRaw).name();
    m["magnitudeColor"] = SeismicColors::magnitudeColor(e.magnitude).name();
    return m;
}

WarningLevel levelFromTag(const QString& tag) {
    if (tag == "CRITICAL") return WarningLevel::Critical;
    if (tag == "WARNING") return WarningLevel::Warning;
    if (tag == "WATCH") return WarningLevel::Watch;
    return WarningLevel::Normal;
}

/// 目录刷新状态 → severity 标签，供设置页给目录行上色。
QString directoryTag(ConnectionStatus s) {
    switch (s) {
    case ConnectionStatus::Connected: return QStringLiteral("NORMAL");
    case ConnectionStatus::Connecting: return QStringLiteral("WATCH");
    case ConnectionStatus::Error: return QStringLiteral("WARNING");
    case ConnectionStatus::Disconnected: return QStringLiteral("CRITICAL");
    }
    return QStringLiteral("CRITICAL");
}

/// CSIS 测站缺测哨兵：烈度域为整数 0–12，`< 0` 表示无观测。
constexpr double kMissingCsis = -1.0;

/// 中国地震烈度卡片数值文本：仪器烈度四舍五入取整（0–12）。
QString csisText(double raw) {
    return QString::number(static_cast<int>(std::lround(std::max(0.0, raw))));
}

/// 韩国 MMI 卡片数值文本：取整并限制在 0–11（与 kanameishi `getMmiFromKmaLevel`
/// 的 `holdLevel - 2` 语义一致）。
QString kmaMmiText(double raw) {
    return QString::number(std::max(0, std::min(11, static_cast<int>(std::lround(raw)))));
}

/// 一张测站源卡片的载荷。`standard` 决定语义与色阶：`jma` 用計測震度（缺测 `raw <= -3`），
/// `csis` 用中国地震烈度，`kma` 用韩国 MMI；三者缺测均显示 `--`。
QVariantMap stationCard(const QString& id, const QString& name, bool hasData, double raw,
                        const QString& standard, int stationCount, const QString& frameTime) {
    const bool jma = standard == QLatin1String("jma");
    QVariantMap m;
    m["id"] = id;
    m["name"] = name;
    m["standard"] = standard;
    m["caption"] = jma ? QStringLiteral("当前最大震度") : QStringLiteral("当前最大烈度");
    m["hasData"] = hasData;
    m["raw"] = raw;
    if (!hasData) {
        m["text"] = QStringLiteral("--");
    } else if (jma) {
        m["text"] = jmaShindoText(raw);
    } else if (standard == QLatin1String("kma")) {
        m["text"] = kmaMmiText(raw);
    } else {
        m["text"] = csisText(raw);
    }
    m["stationCount"] = stationCount;
    m["frameTime"] = frameTime;
    return m;
}

} // namespace

AppController::AppController(QObject* parent, bool startServices)
    : QObject(parent), startServices_(startServices) {
    assetRoot_ = QCoreApplication::applicationDirPath() + "/assets";
    if (!QFileInfo::exists(assetRoot_ + "/travel_times.json")
        || !QFileInfo::exists(assetRoot_ + "/map_bounds.kvb")) {
        assetRoot_ = QDir(QCoreApplication::applicationDirPath())
                         .absoluteFilePath("../share/komiraquake/assets");
    }

    TravelTimeService::instance().loadFromFile(assetRoot_ + "/travel_times.json");
    CityCoordTable::instance().loadFromFile(assetRoot_ + "/china_cities.json");
    // 震中地名表（参考实现 `getFEName` 的 1° 网格）。缺失不是致命错误：
    // 震源推算卡会退回坐标文本，其余功能不受影响。
    RegionNameTable::instance().loadFromFile((assetRoot_ + "/region_names.bin").toStdString());
    qInfo() << "[app] asset root:" << assetRoot_
            << "| travel times:" << (TravelTimeService::instance().isLoaded() ? "loaded" : "MISSING")
            << "| city coords:" << (CityCoordTable::instance().isLoaded() ? "loaded" : "MISSING")
            << "| region names:" << (RegionNameTable::instance().loaded() ? "loaded" : "MISSING");

    settings_ = new SettingsStore(this);
    directoryRefreshEnabled_ = !startHidden();
    eventListModel_ = new EventListModel(this);
    eventModel_ = new EventFilterModel(this);
    eventModel_->setSourceModel(eventListModel_);
    systemDark_ = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    darkMode_ = resolveDarkMode();
    // themeMode 是唯一的用户意图来源；系统配色只在「跟随系统」时参与判定。
    connect(settings_, &SettingsStore::changed, this, [this]() {
        if (darkMode_ != resolveDarkMode()) {
            darkMode_ = resolveDarkMode();
            emit darkModeChanged();
        }
        // 区县着色也由 SettingsStore::changed 驱动，确保关闭时清空旧结果、
        // 开启时按当前地图事件立即重算。
        scheduleDistrictRefresh();
    });
    if (QGuiApplication::styleHints()) {
        connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this](Qt::ColorScheme) {
            systemDark_ = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
            if (darkMode_ != resolveDarkMode()) {
                darkMode_ = resolveDarkMode();
                emit darkModeChanged();
            }
        });
    }
    location_ = new LocationService(this);
    sound_ = new AlertSoundService(this);
    sound_->setAssetRoot(assetRoot_);
    announcer_ = new AlertAnnouncer(settings_, sound_, this);

    // 数据源注册表：Wolfx / Pancakes / Jian / Whews … 平级、互为备份。新增源在此登记一行即可，
    // 接线、启停、状态聚合与跨源合并都由通用逻辑处理。
    jian_ = new JianSource(this);
    whews_ = new WhewsSource(this);
    simulated_ = new SimulatedSource(this);
    sources_ << new WolfxSource(this) << new PancakesSource(this) << jian_ << whews_
             << simulated_;
    // NIED Kmoni 测站显示源：独立于事件源注册表（不产事件、不参与预警/去重/历史）。
    stationSource_ = new KmoniStationSource(this);
    // S-Net 海底观测网测站显示源：同上。
    snetSource_ = new SnetStationSource(this);
    // KMA PEWS 测站显示源：同上。
    kmaSource_ = new KmaStationSource(this);
    // PancakesSeis 测站显示源：同上，独立于事件源注册表。
    seisSource_ = new PancakesSeisSource(this);
    // Wolfx SeisJS 测站显示源：同上。
    wolfxSeisSource_ = new WolfxSeisSource(this);
    // 测站快照：地图项直接读这个对象，避免每 100 ms 经历 QVariantList/JS 数组的往返转换。
    stationSnapshot_ = new StationSnapshot(this);
    // 本地震源推算（Beta）：消费上面五路测站强度，1 Hz 起算；不产事件、不参与告警。
    sourceEstimation_ = new SourceEstimationService(settings_, this);
    sourceEstimation_->setNowProvider([this]() { return nowMs(); });
    connect(sourceEstimation_, &SourceEstimationService::changed, this,
            &AppController::sourceEstimateChanged);
    clock_ = new NtpClock(this);
    autoStart_ = new AutoStartService(this);
    updater_ = new UpdateService(this);

    historyStore_ = new HistoryStore();
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDir);
    historyStore_->init(dataDir + "/komira_quake_history.db");
    qInfo() << "[app] history db:" << (historyStore_->isReady() ? "ready" : "unavailable")
            << (dataDir + "/komira_quake_history.db");

    wire();

    countdownTimer_.setInterval(1000);
    connect(&countdownTimer_, &QTimer::timeout, this, &AppController::onTick);

    // 各测站源都可能按站推送；UI 不需要按网络帧频率重建整张测站表。
    stationIntensityRefreshTimer_.setSingleShot(true);
    stationIntensityRefreshTimer_.setInterval(100);
    connect(&stationIntensityRefreshTimer_, &QTimer::timeout, this, [this]() {
        refreshStationSnapshot();
        // 卡片（stationSources）仍走属性通知；地图图层走快照的 changed()。
        emit stationIntensityChanged();
    });

    // 波前圆按显示帧率重算，只在焦点事件"还年轻"时跑（见 updateWaveTimer）。
    // 目录事件逐条写库会变成「每条一个事务 + 一次 prune」，启动/复显时能把 GUI 线程
    // 卡住 1–2 s（实测 200 条约 400 ms）。攒 80 ms 一次事务写回。
    historyFlushTimer_.setSingleShot(true);
    historyFlushTimer_.setInterval(80);
    connect(&historyFlushTimer_, &QTimer::timeout, this, &AppController::flushHistoryWrites);

    waveTimer_.setInterval(16);
    connect(&waveTimer_, &QTimer::timeout, this, &AppController::updateWaveRadii);

    // 自动焦点跨过 10 分钟窗口时刷新一次 HUD 可见性（无需等下一次数据/告警）。
    hudRefreshTimer_.setSingleShot(true);
    connect(&hudRefreshTimer_, &QTimer::timeout, this, [this]() { emit hudEventChanged(); });

    directoryUiTimer_.setSingleShot(true);
    connect(&directoryUiTimer_, &QTimer::timeout, this, [this]() {
        refreshEventModel();
        emit hudEventChanged();
    });

    // 区县烈度着色：单次定时器合并连发事件，只算最后一次；计算本体派给 worker 线程。
    districtTimer_.setSingleShot(true);
    connect(&districtTimer_, &QTimer::timeout, this, &AppController::recomputeDistrictIntensity);
    connect(&districtWatcher_, &QFutureWatcher<QVariantMap>::finished, this,
            &AppController::onDistrictComputed);
}

void AppController::scheduleStationIntensityRefresh() {
    if (!stationIntensityRefreshTimer_.isActive()) stationIntensityRefreshTimer_.start();
}

void AppController::noteStationTablesChanged() {
    // 台站表可能变化时才让 refreshStationSnapshot 重建元数据。震度帧走另一条路径，
    // 不受影响 —— 悬停浮层的站名只在首包/名录更新/启用切换时才会变。
    stationMetaDirty_ = true;
    scheduleStationIntensityRefresh();
}

long long AppController::nowMs() const {
    return nowProvider_ ? nowProvider_() : clock_->now();
}

AppController::~AppController() {
    flushHistoryWrites();   // 退出前把还没落盘的目录事件写掉
    if (historyStore_) delete historyStore_;
}

void AppController::wire() {
    // 校时：数据源与告警编排都走 clock_ 的时间基准。《NATIVE_PORT_SPEC》 §13。
    connect(clock_, &NtpClock::changed, this, &AppController::clockChanged);
    // 逐源接线：校时注入、事件与状态回调；新增源无需改这里。
    for (EarthquakeSource* source : sources_) {
        source->setNowProvider([this]() { return nowMs(); });
        source->setMonoProvider([this]() { return clock_->elapsedMs(); });
        connect(source, &EarthquakeSource::eventReceived, this,
                [this, source](const EarthquakeEvent& e, SourceEventKind kind) {
                    handleEvent(e, false, kind == SourceEventKind::Directory, source);
                });
        connect(source, &EarthquakeSource::infoChanged, this, &AppController::statusChanged);
    }

    // Jian 需登录令牌：启动注入已持久化的 rt_；登录成功后持久化新 rt_ 并立即重新鉴权。
    jian_->setRefreshToken(settings_->jianRefreshToken());
    connect(jian_, &JianSource::refreshTokenObtained, this, [this](const QString& token) {
        settings_->setJianRefreshToken(token);
        jian_->setRefreshToken(token);
    });

    // Whews 需 wat_ 令牌：启动注入已持久化的令牌，设置页改动后立即生效。
    whews_->setToken(settings_->whewsToken());

    // 模拟源仅开发自测：需开发者模式开启且地址非空，否则 isConfigured() 为假、不会连接。
    simulated_->setDevMode(settings_->developerMode());
    simulated_->setUrl(settings_->simulatedUrl());

    // 定位只影响"本地烈度/倒计时/全屏预警"，不影响数据源连接。
    connect(location_, &LocationService::changed, this, [this]() {
        emit locationChanged();
        for (EarthquakeSource* source : sources_) {
            if (location_->hasLocation())
                source->setUserLocation(location_->latitude(), location_->longitude());
            else
                source->clearUserLocation();
        }
        recomputeEvents();
    });

    connect(settings_, &SettingsStore::changed, this, [this]() {
        const IntensityStandard standard = settings_->intensityStandard() == 1
            ? IntensityStandard::Jma : IntensityStandard::Csis;
        // 开发者模式与地址须先于启停生效：否则刚开启开发者模式要等下次启动才连上。
        simulated_->setDevMode(settings_->developerMode());
        simulated_->setUrl(settings_->simulatedUrl());
        for (EarthquakeSource* source : sources_) {
            source->setStandard(standard);
            // 未配置凭据的源不启动：不建立任何连接。
            if (startServices_ && settings_->isSourceEnabled(source->id()) && source->isConfigured())
                source->start();
            else source->stop();
        }
        // 测站显示源独立于事件源门控：关闭后立即断开连接并清空图层数据。
        const auto syncStation = [this](bool enabled, auto* source) {
            if (startServices_ && enabled) source->start(); else source->stop();
        };
        syncStation(settings_->isStationSourceEnabled(StationSourceIds::kKmoni), stationSource_);
        syncStation(settings_->isStationSourceEnabled(StationSourceIds::kSnet), snetSource_);
        syncStation(settings_->isStationSourceEnabled(StationSourceIds::kKma), kmaSource_);
        syncStation(settings_->isStationSourceEnabled(StationSourceIds::kPancakesSeis), seisSource_);
        syncStation(settings_->isStationSourceEnabled(StationSourceIds::kWolfxSeis), wolfxSeisSource_);
        // 启用/停用会改变合并后测站表的构成：悬停元数据必须整表重建一次。
        noteStationTablesChanged();
        // 震源推算的门槛/负证据/开关变化：作废旧解，按新判据重新起算。
        sourceEstimation_->reloadSettings();
        // 地图标记的显示开关只影响呈现，但该属性没有别的通知源：不发这一下，
        // QML 会一直停在关闭时那份 `{ visible: false }` 上，重新打开也不恢复。
        emit sourceEstimateChanged();
        sound_->setEnabled(settings_->enableSoundAlert());
        sound_->setVolume(settings_->alertVolume());
        clock_->setCustomServer(settings_->customNtpServer());
        clock_->setEnabled(startServices_ && settings_->enableNtpSync());
        announcer_->onMuteChanged();
        recomputeEvents();
        emit statusChanged();
        maybeAutoCheckUpdates();
    });

    // 启动时应用一次设置
    sound_->setEnabled(settings_->enableSoundAlert());
    sound_->setVolume(settings_->alertVolume());

    if (historyStore_->isReady()) {
        const auto tombstones = historyStore_->loadTombstones(nowMs());
        for (auto it = tombstones.cbegin(); it != tombstones.cend(); ++it)
            sessions_.finish(it.key().toStdString(), it.value());
        for (const auto& e : historyStore_->loadRecent(200)) history_.push_back(e);
        rebuildHistory();
    }

    // 恢复上次定位（手动或 IP）；有记录就不再自动 IP，避免覆盖用户基准地。
    location_->restore();

    // 数据源与定位解耦：先连数据源，定位并行获取；校时并行后台进行。
    const IntensityStandard startStandard =
        settings_->intensityStandard() == 1 ? IntensityStandard::Jma : IntensityStandard::Csis;
    for (EarthquakeSource* source : sources_) {
        source->setStandard(startStandard);
        source->setDirectoryPollingEnabled(directoryRefreshEnabled_);
        if (startServices_ && settings_->isSourceEnabled(source->id()) && source->isConfigured())
            source->start();
    }
    if (startServices_) {
        // NIED 测站显示源：随应用启停（不参与 EEW 源门控）。台站表变化会连带重建悬停
        // 元数据（noteStationTablesChanged），震度帧只刷新数值。
        connect(stationSource_, &KmoniStationSource::stationsChanged, this,
                [this]() { noteStationTablesChanged(); });
        connect(stationSource_, &KmoniStationSource::intensityChanged, this,
                [this]() { scheduleStationIntensityRefresh(); });
        if (settings_->isStationSourceEnabled(StationSourceIds::kKmoni))
            stationSource_->start();
        // S-Net 海底观测网测站显示源：同上，与 Kmoni 共用 JMA 震度色阶。
        connect(snetSource_, &SnetStationSource::stationsChanged, this,
                [this]() { noteStationTablesChanged(); });
        connect(snetSource_, &SnetStationSource::intensityChanged, this,
                [this]() { scheduleStationIntensityRefresh(); });
        if (settings_->isStationSourceEnabled(StationSourceIds::kSnet))
            snetSource_->start();
        // KMA PEWS 测站显示源：同上，按韩国 MMI 整数烈度着色。
        connect(kmaSource_, &KmaStationSource::stationsChanged, this,
                [this]() { noteStationTablesChanged(); });
        connect(kmaSource_, &KmaStationSource::mmiChanged, this,
                [this]() { scheduleStationIntensityRefresh(); });
        if (settings_->isStationSourceEnabled(StationSourceIds::kKma))
            kmaSource_->start();
        // PancakesSeis 测站显示源：同上，与 Kmoni 共用地图测站图层。
        connect(seisSource_, &PancakesSeisSource::stationsChanged, this,
                [this]() { noteStationTablesChanged(); });
        connect(seisSource_, &PancakesSeisSource::dataChanged, this,
                [this]() { scheduleStationIntensityRefresh(); });
        if (settings_->isStationSourceEnabled(StationSourceIds::kPancakesSeis))
            seisSource_->start();
        // Wolfx SeisJS 测站显示源：同上，共用地图测站图层。
        connect(wolfxSeisSource_, &WolfxSeisSource::stationsChanged, this,
                [this]() { noteStationTablesChanged(); });
        connect(wolfxSeisSource_, &WolfxSeisSource::dataChanged, this,
                [this]() { scheduleStationIntensityRefresh(); });
        if (settings_->isStationSourceEnabled(StationSourceIds::kWolfxSeis))
            wolfxSeisSource_->start();
        clock_->setCustomServer(settings_->customNtpServer());
        clock_->setEnabled(settings_->enableNtpSync());
        clock_->start();
        if (!location_->hasLocation()) location_->requestCurrentPosition();
    }
    maybeAutoCheckUpdates();
}

bool AppController::startHidden() const {
    return settings_ && settings_->silentStart();
}

bool AppController::alertEligible() const {
    return hasActiveWarning_ && announcer_->eligible(activeWarning_);
}

void AppController::maybeAutoCheckUpdates() {
    if (!startServices_ || autoUpdateChecked_ || !settings_->autoCheckUpdates()) return;
    autoUpdateChecked_ = true;
    // 延后到界面与数据源就绪后再联网，避免与启动风暴抢带宽。
    QTimer::singleShot(4000, this, [this]() { updater_->check(true); });
}

bool AppController::hasLocation() const { return location_->hasLocation(); }
QString AppController::locationName() const { return location_->locationName(); }
double AppController::userLatitude() const { return location_->latitude(); }
double AppController::userLongitude() const { return location_->longitude(); }
QString AppController::locationStatusText() const { return location_->statusText(); }

QVariantList AppController::sources() const {
    auto describe = [](const DataSourceInfo& info, bool enabled, bool configured) {
        QVariantMap m;
        m["id"] = info.id;
        m["name"] = info.name;
        m["enabled"] = enabled;
        // 是否已配置（与启动门控一致）：左下角徽章据此只展示真正在跑的源。
        m["configured"] = configured;
        m["status"] = info.statusLabel();
        m["statusTag"] = info.statusTag();
        m["latency"] = info.latencyMs >= 0 ? QStringLiteral("%1 ms").arg(info.latencyMs)
                                           : QStringLiteral("— ms");
        switch (info.directoryStatus) {
        case ConnectionStatus::Connected: m["directory"] = QStringLiteral("目录刷新成功"); break;
        case ConnectionStatus::Connecting: m["directory"] = QStringLiteral("目录刷新中"); break;
        case ConnectionStatus::Error:
            m["directory"] = QStringLiteral("目录刷新失败：") + info.directoryError; break;
        default:         m["directory"] = QStringLiteral("目录未刷新"); break;
        }
        m["directoryStatusTag"] = directoryTag(info.directoryStatus);
        m["directoryLatency"] = info.directoryLatencyMs >= 0
            ? QStringLiteral("%1 ms").arg(info.directoryLatencyMs) : QStringLiteral("— ms");
        m["description"] = info.description;
        return m;
    };
    QVariantList list;
    for (EarthquakeSource* source : sources_)
        list.push_back(describe(source->info(), settings_->isSourceEnabled(source->id()),
                                source->isConfigured()));
    return list;
}

QVariantMap AppController::clockInfo() const { return clock_->info(); }

QString AppController::clockTextUtc8() const {
    // 固定 +08:00 时区渲染，与设备时区无关。
    const QDateTime stamp =
        QDateTime::fromMSecsSinceEpoch(nowMs(), QTimeZone(kClockUtc8OffsetSeconds));
    return stamp.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

QVariantList AppController::history() const {
    QVariantList list;
    for (const auto& e : history_) list.push_back(toMap(e));
    return list;
}

QVariantList AppController::eventList() const {
    // 目录条目用权威数据；同一物理地震（发震时刻 + 震中）只保留一条，实时预警命中目录时
    // 用目录数据并标记 isActive，未命中目录的实时预警另行补上。
    QList<EarthquakeEvent> catalog;
    for (const auto& e : history_) {
        int found = -1;
        for (int i = 0; i < catalog.size(); ++i) {
            if (samePhysicalEvent(catalog[i], e)) { found = i; break; }
        }
        if (found < 0) { catalog.push_back(e); continue; }
        // 同一地震若有自动/正式两条，取推送时间较晚（通常为正式）的一条。
        const long long prev = catalog[found].reportTime > 0 ? catalog[found].reportTime
                                                             : catalog[found].timestamp;
        const long long next = e.reportTime > 0 ? e.reportTime : e.timestamp;
        if (next > prev) catalog[found] = e;
    }

    QList<EarthquakeEvent> emitted;
    auto alreadyEmitted = [&emitted](const EarthquakeEvent& e) {
        for (const auto& p : emitted) {
            if (samePhysicalEvent(p, e)) return true;
        }
        return false;
    };
    QVariantList out;
    const IntensityStandard standard =
        settings_->intensityStandard() == 1 ? IntensityStandard::Jma : IntensityStandard::Csis;
    auto push = [&out, standard](const EarthquakeEvent& e, bool active) {
        QVariantMap m = toMap(e, standard);
        m["isActive"] = active;
        out.push_back(m);
    };

    QList<bool> catalogUsed(catalog.size(), false);
    for (const auto& [key, state] : sessions_.active) {
        const EarthquakeEvent& live = state.event;
        if (alreadyEmitted(live)) continue;
        int found = -1;
        for (int i = 0; i < catalog.size(); ++i) {
            if (!catalogUsed[i] && samePhysicalEvent(catalog[i], live)) { found = i; break; }
        }
        if (found >= 0) {
            catalogUsed[found] = true;
            push(catalog[found], true);
            emitted.push_back(catalog[found]);
        } else {
            push(live, true);
            emitted.push_back(live);
        }
    }
    for (int i = 0; i < catalog.size(); ++i) {
        if (catalogUsed[i] || alreadyEmitted(catalog[i])) continue;
        push(catalog[i], false);
        emitted.push_back(catalog[i]);
    }

    std::sort(out.begin(), out.end(), [](const QVariant& a, const QVariant& b) {
        return a.toMap().value(QStringLiteral("timestamp")).toLongLong() >
               b.toMap().value(QStringLiteral("timestamp")).toLongLong();
    });
    return out;
}

QVariantList AppController::activeWarnings() const {
    QVariantList list;
    for (const auto& [key, state] : sessions_.active) list.push_back(toMap(state.event));
    return list;
}

QVariant AppController::activeWarning() const {
    if (!hasActiveWarning_) return QVariant();
    return toMap(activeWarning_);
}

const EarthquakeEvent* AppController::displayedEvent() const {
    if (hasMapFocus_) return &mapFocus_;
    if (!history_.isEmpty()) return &history_.first();
    return nullptr;
}

QVariant AppController::mapEvent() const {
    const EarthquakeEvent* event = displayedEvent();
    if (!event) return QVariant();
    QVariantMap map = toMap(*event);
    map["ended"] = sessions_.finished(event->identity());
    return map;
}

bool AppController::hudWindowOpen(const EarthquakeEvent& event, long long now) const {
    // 以最后一报的推送时间为准；目录事件没有推送时间则退回发震时刻。
    const long long reference = event.reportTime > 0 ? event.reportTime : event.timestamp;
    if (reference <= 0) return false;
    return now - reference <= kHudWindowMs;
}

QVariant AppController::hudEvent() const {
    // 用户主动点击的焦点最优先：点击后始终显示 HUD，不受 10 分钟窗口限制。
    if (hasMapFocus_ && mapFocusManual_) return toMap(mapFocus_);
    if (hasActiveWarning_) return toMap(activeWarning_);
    // 自动接管的实时事件：转为历史事件后只在最后一报推送的 10 分钟内继续展示。
    if (hasMapFocus_) {
        const bool stillActive = sessions_.active.count(mapFocus_.identity()) > 0;
        if (stillActive || hudWindowOpen(mapFocus_, nowMs())) return toMap(mapFocus_);
    }
    return QVariant();
}

std::vector<std::string> AppController::orderedActiveKeys() const {
    std::vector<std::pair<long long, std::string>> ordered;
    ordered.reserve(sessions_.active.size());
    for (const auto& [key, state] : sessions_.active)
        ordered.emplace_back(state.event.timestamp, key);
    // 发震时刻倒序（最新在前）；同一时刻按 identity 稳定排序。
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first > b.first;
        return a.second < b.second;
    });
    std::vector<std::string> keys;
    keys.reserve(ordered.size());
    for (const auto& entry : ordered) keys.push_back(entry.second);
    return keys;
}

int AppController::hudCount() const { return static_cast<int>(sessions_.active.size()); }

int AppController::hudIndex() const {
    const auto keys = orderedActiveKeys();
    for (size_t i = 0; i < keys.size(); ++i)
        if (keys[i] == selectedWarning_) return static_cast<int>(i);
    return 0;
}

void AppController::hudPrev() { shiftHud(-1); }
void AppController::hudNext() { shiftHud(1); }

void AppController::shiftHud(int delta) {
    const auto keys = orderedActiveKeys();
    if (keys.empty()) return;
    int index = 0;
    for (size_t i = 0; i < keys.size(); ++i)
        if (keys[i] == selectedWarning_) { index = static_cast<int>(i); break; }
    const int count = static_cast<int>(keys.size());
    index = ((index + delta) % count + count) % count;
    selectedWarning_ = keys[static_cast<size_t>(index)];
    // 切换即同步地图焦点（非手动），使地图与 HUD 指向同一事件。
    const auto it = sessions_.active.find(selectedWarning_);
    if (it != sessions_.active.end()) {
        announcer_->stopOutput();
        setMapFocus(it->second.event, false);
    }
    syncWarning();
}

void AppController::scheduleHudRefresh() {
    hudRefreshTimer_.stop();
    if (!hasMapFocus_ || mapFocusManual_) return;
    if (sessions_.active.count(mapFocus_.identity())) return;
    const long long reference = mapFocus_.reportTime > 0 ? mapFocus_.reportTime : mapFocus_.timestamp;
    if (reference <= 0) return;
    const long long remaining = reference + kHudWindowMs - nowMs();
    if (remaining <= 0 || remaining > 2'147'483'647LL) return;
    hudRefreshTimer_.start(static_cast<int>(remaining));
}

QVariantMap AppController::waveRadii() const {
    QVariantMap m;
    m["pKm"] = wavePKm_;
    m["sKm"] = waveSKm_;
    m["pOpacity"] = wavePOpacity_;
    m["sOpacity"] = waveSOpacity_;
    m["sFillOpacity"] = waveSFillOpacity_;
    return m;
}

void AppController::refreshStationSnapshot() {
    if (!stationSnapshot_) return;
    std::vector<StationSnapshot::Entry> entries;
    // 悬停浮层的元数据只在台站表变化时重建一次（stationMetaDirty_）：五路台站表合计
    // 约 2500 条，逐帧重建会产生上万次 QString 拷贝。条目与元数据同序生成，故
    // `Entry::meta` 恒等于条目下标。
    const bool withMeta = stationMetaDirty_;
    std::vector<StationSnapshot::StationMeta> metas;
    if (withMeta) {
        const qsizetype estimate = (stationSource_ ? stationSource_->stations().size() : 0) +
                                   (snetSource_ ? snetSource_->stations().size() : 0) +
                                   (kmaSource_ ? kmaSource_->stations().size() : 0) +
                                   (seisSource_ ? seisSource_->stations().size() : 0) +
                                   (wolfxSeisSource_ ? wolfxSeisSource_->stations().size() : 0);
        metas.reserve(static_cast<std::size_t>(std::max<qsizetype>(0, estimate)));
        entries.reserve(static_cast<std::size_t>(std::max<qsizetype>(0, estimate)));
    }

    // 只在重建元数据时构造 StationMeta（QString 拷贝虽为引用计数，但逐帧对约 2500 条
    // 做一次也不该发生）；平时只填 Entry::meta 下标。
    auto push = [&](StationSnapshot::Entry e, auto&& fillMeta) {
        e.meta = static_cast<int>(entries.size());
        entries.push_back(e);
        if (withMeta) {
            StationSnapshot::StationMeta m;
            fillMeta(m);
            metas.push_back(std::move(m));
        }
    };

    // NIED Kmoni：raw 为 JMA 計測震度（缺测哨兵 -3）。
    if (stationSource_ && stationSource_->hasData()
        && settings_->isStationSourceEnabled(StationSourceIds::kKmoni)) {
        const auto& stations = stationSource_->stations();
        const auto& intensity = stationSource_->intensity();
        for (int i = 0; i < stations.size(); ++i) {
            StationSnapshot::Entry e;
            e.lat = stations[i].latitude;
            e.lon = stations[i].longitude;
            e.raw = i < intensity.size() ? intensity[i] : KmoniStationProtocol::kMissing;
            e.csis = false;
            push(e, [&](StationSnapshot::StationMeta& m) {
                m.name = stations[i].name;
                m.area = stations[i].region;
                m.code = stations[i].code;
                m.type = stations[i].stationType;
                m.source = QStringLiteral("NIED Kmoni");
            });
        }
    }
    // S-Net 海底观测网：raw 为 JMA 計測震度（缺测 -3，饱和 99 原样保留）。
    if (snetSource_ && snetSource_->hasData()
        && settings_->isStationSourceEnabled(StationSourceIds::kSnet)) {
        const auto& stations = snetSource_->stations();
        const auto& intensity = snetSource_->intensity();
        for (int i = 0; i < stations.size(); ++i) {
            StationSnapshot::Entry e;
            e.lat = stations[i].latitude;
            e.lon = stations[i].longitude;
            e.raw = i < intensity.size() ? intensity[i] : SnetStationProtocol::kMissing;
            push(e, [&](StationSnapshot::StationMeta& m) {
                m.name = stations[i].name;
                m.code = stations[i].code;
                m.type = stations[i].stationType;
                m.source = QStringLiteral("S-Net");
            });
        }
    }
    // KMA PEWS：raw 为韩国 MMI 整数烈度（缺测 -3），走独立的 KMA 色阶。
    if (kmaSource_ && kmaSource_->hasData()
        && settings_->isStationSourceEnabled(StationSourceIds::kKma)) {
        const auto& stations = kmaSource_->stations();
        const auto& mmi = kmaSource_->mmi();
        for (int i = 0; i < stations.size(); ++i) {
            StationSnapshot::Entry e;
            e.lat = stations[i].latitude;
            e.lon = stations[i].longitude;
            e.raw = i < mmi.size() ? mmi[i] : KmaStationProtocol::kMissing;
            e.kma = true;
            push(e, [&](StationSnapshot::StationMeta& m) {
                m.source = QStringLiteral("KMA PEWS");
            });
        }
    }
    // PancakesSeis：设备端已按 GB/T 17742-2020 算好仪器烈度（帧 `i`），直接进 CSIS
    // 域，不再用 PGA 二次折算；缺测哨兵 -1。
    if (seisSource_ && seisSource_->hasData()
        && settings_->isStationSourceEnabled(StationSourceIds::kPancakesSeis)) {
        const auto& stations = seisSource_->stations();
        const auto& intensity = seisSource_->intensity();
        for (int i = 0; i < stations.size(); ++i) {
            StationSnapshot::Entry e;
            e.lat = stations[i].latitude;
            e.lon = stations[i].longitude;
            const double value =
                i < intensity.size() ? intensity[i] : PancakesSeisProtocol::kMissingIntensity;
            e.raw = value > PancakesSeisProtocol::kMissingIntensity ? value : kMissingCsis;
            e.csis = true;
            push(e, [&](StationSnapshot::StationMeta& m) {
                m.name = stations[i].name;
                m.area = stations[i].region;
                m.code = stations[i].sid >= 0 ? QString::number(stations[i].sid) : QString();
                m.source = QStringLiteral("PancakesSeis");
            });
        }
    }
    // Wolfx SeisJS：报文自带中国地震烈度 `Intensity`，直接进入 CSIS 域。
    if (wolfxSeisSource_ && wolfxSeisSource_->hasData()
        && settings_->isStationSourceEnabled(StationSourceIds::kWolfxSeis)) {
        const auto& stations = wolfxSeisSource_->stations();
        for (const auto& station : stations) {
            StationSnapshot::Entry e;
            e.lat = station.latitude;
            e.lon = station.longitude;
            e.raw = station.csis >= 0 ? static_cast<double>(station.csis) : kMissingCsis;
            e.csis = true;
            push(e, [&](StationSnapshot::StationMeta& m) {
                m.name = station.location;
                m.code = station.id;
                m.source = QStringLiteral("Wolfx SeisJS");
            });
        }
    }

    if (withMeta) {
        stationMetas_ = std::move(metas);
        stationMetaDirty_ = false;
        if (entries.size() != stationMetas_.size()) {
            // 不该发生：条目与元数据同序生成。留一条日志便于定位，随后照样下发。
            qWarning() << "[station] meta/entry size mismatch" << entries.size()
                       << stationMetas_.size();
        }
        if (sourceEstimation_) sourceEstimation_->ingestObservations(entries);
        stationSnapshot_->setEntries(std::move(entries), stationMetas_);
        return;
    }
    if (stationMetas_.size() != entries.size()) {
        // 启用/停用了某个测站源却漏标脏：下一帧补一次完整重建（悬停先没有元数据）。
        stationMetaDirty_ = true;
        scheduleStationIntensityRefresh();
    }
    if (sourceEstimation_) sourceEstimation_->ingestObservations(entries);
    stationSnapshot_->setEntries(std::move(entries));
}

QVariantMap AppController::sourceEstimateInfo() const {
    return sourceEstimation_ ? sourceEstimation_->info() : QVariantMap();
}

QVariantMap AppController::sourceEstimateMarker() const {
    if (!settings_ || !settings_->showEstimatedEpicenter()) {
        QVariantMap hidden;
        hidden["visible"] = false;
        return hidden;
    }
    return sourceEstimation_ ? sourceEstimation_->marker() : QVariantMap();
}

QVariantList AppController::stationSources() const {
    // 每个测站源一张卡片：NIED / S-Net 用 JMA 計測震度，Pancakes / Wolfx 用中国地震烈度
    // （CSIS），KMA 用韩国 MMI。
    QVariantList list;
    if (stationSource_ && settings_->isStationSourceEnabled(StationSourceIds::kKmoni)) {
        const double raw = stationSource_->hasData() ? stationSource_->maxIntensity()
                                                      : KmoniStationProtocol::kMissing;
        list.push_back(stationCard(QStringLiteral("kmoni"), QStringLiteral("NIED"),
                                   raw > KmoniStationProtocol::kMissing, raw,
                                   QStringLiteral("jma"), stationSource_->stations().size(),
                                   stationSource_->frameTime()));
    }
    if (snetSource_ && settings_->isStationSourceEnabled(StationSourceIds::kSnet)) {
        const double raw = snetSource_->hasData() ? snetSource_->maxIntensity()
                                                  : SnetStationProtocol::kMissing;
        list.push_back(stationCard(QStringLiteral("snet"), QStringLiteral("S-Net"),
                                   raw > SnetStationProtocol::kMissing, raw,
                                   QStringLiteral("jma"), snetSource_->stations().size(),
                                   snetSource_->frameTime()));
    }
    if (kmaSource_ && settings_->isStationSourceEnabled(StationSourceIds::kKma)) {
        const double raw = kmaSource_->hasData() ? kmaSource_->maxMmi()
                                                 : KmaStationProtocol::kMissing;
        list.push_back(stationCard(QStringLiteral("kma_station"), QStringLiteral("KMA"),
                                   raw > KmaStationProtocol::kMissing, raw,
                                   QStringLiteral("kma"), kmaSource_->stations().size(),
                                   kmaSource_->frameTime()));
    }
    if (seisSource_ && settings_->isStationSourceEnabled(StationSourceIds::kPancakesSeis)) {
        // 卡片直接展示设备下发的最大仪器烈度（GB/T 17742-2020），不再由 PGA 折算。
        const double raw = seisSource_->maxIntensity();
        const bool hasData =
            seisSource_->hasData() && raw > PancakesSeisProtocol::kMissingIntensity;
        const QString frameTime = seisSource_->lastTimestamp() > 0
            ? QDateTime::fromMSecsSinceEpoch(seisSource_->lastTimestamp(),
                                             QTimeZone(kClockUtc8OffsetSeconds))
                  .toString(QStringLiteral("yyyyMMddHHmmss"))
            : QString();
        list.push_back(stationCard(QStringLiteral("pancakes_seis"), QStringLiteral("PancakesSeis"),
                                   hasData, raw, QStringLiteral("csis"),
                                   seisSource_->stations().size(), frameTime));
    }
    if (wolfxSeisSource_ && settings_->isStationSourceEnabled(StationSourceIds::kWolfxSeis)) {
        const int maxCsis = wolfxSeisSource_->hasData() ? wolfxSeisSource_->maxCsis()
                                                        : WolfxSeisProtocol::kMissingIntensity;
        const bool hasData = maxCsis >= 0;
        list.push_back(stationCard(QStringLiteral("wolfx_seis"), QStringLiteral("WolfxSeisJS"),
                                   hasData, hasData ? static_cast<double>(maxCsis) : kMissingCsis,
                                   QStringLiteral("csis"), wolfxSeisSource_->stations().size(),
                                   wolfxSeisSource_->frameTime()));
    }
    return list;
}

bool AppController::isMapFocused(const QString& id) const {
    return hasMapFocus_ && QString::fromStdString(mapFocus_.identity()) == id;
}

void AppController::toggleMapFocus(const QString& id) {
    const auto active = sessions_.active.find(id.toStdString());
    if (active != sessions_.active.end()) {
        if (selectedWarning_ != active->first) announcer_->stopOutput();
        selectedWarning_ = active->first;
        syncWarning();
        setMapFocus(active->second.event, true);
        return;
    }
    for (const auto& [key, state] : sessions_.active) {
        if (QString::fromStdString(state.event.id) == id) {
            if (selectedWarning_ != key) announcer_->stopOutput();
            selectedWarning_ = key;
            syncWarning();
            setMapFocus(state.event, true);
            return;
        }
    }
    if (isMapFocused(id)) {
        // 再次点击同一事件：取消焦点并收起 HUD（活动预警仍会保持显示）。
        clearMapFocus();
        return;
    }
    if (hasActiveWarning_ && (QString::fromStdString(activeWarning_.identity()) == id ||
                             QString::fromStdString(activeWarning_.id) == id)) {
        setMapFocus(activeWarning_, true);
        return;
    }
    for (const EarthquakeEvent& event : history_) {
        if (QString::fromStdString(event.identity()) == id || QString::fromStdString(event.id) == id) {
            // 如果该历史条目正是当前正在活跃的预警（同一物理地震），优先切换到活跃预警会话
            for (const auto& [key, state] : sessions_.active) {
                if (samePhysicalEvent(state.event, event)) {
                    if (selectedWarning_ != key) announcer_->stopOutput();
                    selectedWarning_ = key;
                    syncWarning();
                    setMapFocus(state.event, true);
                    return;
                }
            }
            setMapFocus(event, true);
            return;
        }
    }
}

void AppController::setMapFocus(const EarthquakeEvent& event, bool manual) {
    mapFocus_ = event;
    hasMapFocus_ = true;
    mapFocusManual_ = manual;
    updateWaveTimer();
    scheduleHudRefresh();
    scheduleDistrictRefresh();
    emit mapEventChanged();
    emit mapFocusChanged();
    emit hudEventChanged();
    emit centerMapRequested(event.latitude, event.longitude);
}

void AppController::clearMapFocus() {
    if (!hasMapFocus_) return;
    hasMapFocus_ = false;
    mapFocusManual_ = false;
    updateWaveTimer();
    scheduleHudRefresh();
    scheduleDistrictRefresh();
    emit mapEventChanged();
    emit mapFocusChanged();
    emit hudEventChanged();
    emit resetMapRequested();
}

void AppController::scheduleDistrictRefresh() {
    // 区县着色跟着焦点事件走，但数据源会连发多报；用单次定时器把一串更新合并成一次计算。
    districtTimer_.stop();
    districtTimer_.start(80);
}

QVariantMap AppController::computeDistrictIntensity(const VectorMapData& data, double magnitude,
                                                    double depthKm, double lat, double lon) {
    QVariantMap result;
    const VectorLayer* layer = data.layer("district");
    if (!layer || magnitude <= 0.0) return result;

    // 影响半径：超出这个距离烈度已低于可感下限，直接剪掉，避免对 1011 个面全量算距离。
    // M7 深源可达 2000 km，此时剪枝失效，但最坏也只是 34 万次线段距离（桌面约 3 ms）。
    const double radiusKm = IntensityCalculator::distanceForCsis(magnitude, depthKm, 1.0);
    if (radiusKm <= 0.0) return result;

    for (const VectorFeature& f : layer->features) {
        const GeoBounds b = boundsOf(f.polygon);
        // 包围盒离震中就超过影响半径的区县必不受影响，先按保守上界剪掉。
        if (b.maxDistanceKm(lat, lon) > radiusKm) continue;
        const double dist = GeoGeometry::distanceToPolygonKm(f.polygon, lat, lon);
        const double raw = IntensityCalculator::rawCsis(magnitude, dist, depthKm);
        // 低于可感下限（<0.5）不着色，否则整片中国会被灰色铺满。
        if (raw >= 0.5) result.insert(QString::fromStdString(f.name), raw);
    }
    return result;
}

void AppController::recomputeDistrictIntensity() {
    // 已有一次计算在跑：只记待办，等它结束后再按届时最新状态算一次。这样任一时刻只有
    // 一个任务读矢量数据，连发事件也不会把任务堆积进线程池。
    if (districtRunning_) {
        districtPending_ = true;
        return;
    }

    const EarthquakeEvent* event = displayedEvent();
    // 区县着色跟随矢量图层：纯矢量底图本就画矢量，栅格底图需开启「叠加矢量层」。
    // 与 QML 的 vectorActive（MapView.qml）同口径。
    const bool vectorActive =
        settings_->vectorOverlay() || settings_->basemapId() == QLatin1String("vector");
    // 取消/终态事件仍可能暂时留在 mapFocus_ 中，用于完成地图与 HUD 的收尾通知；
    // 但它们不再代表有效影响范围，不能把已经清掉的区县颜色重新算回来。
    const bool usable = event && !event->isCanceled && !sessions_.finished(event->identity()) &&
                        vectorActive && event->magnitude > 0.0;
    if (!usable) {
        applyDistrictIntensity({});
        return;
    }

    // 矢量数据只在首次着色时加载一次（3.7 MiB）；此后作为不可变快照交给 worker 读取。
    if (!districtData_) {
        auto data = std::make_shared<VectorMapData>();
        if (data->loadFromFile((assetRoot_ + QStringLiteral("/map_bounds.kvb")).toStdString()))
            districtData_ = std::move(data);
    }
    if (!districtData_) {
        applyDistrictIntensity({});
        return;
    }

    // 参数在主线程快照后按值捕获：worker 只持有不可变数据与纯值，不触碰任何 QObject。
    const std::shared_ptr<const VectorMapData> snapshot = districtData_;
    const double magnitude = event->magnitude;
    const double depthKm = event->depth;
    const double lat = event->latitude;
    const double lon = event->longitude;
    districtRunning_ = true;
    districtWatcher_.setFuture(QtConcurrent::run([snapshot, magnitude, depthKm, lat, lon] {
        return computeDistrictIntensity(*snapshot, magnitude, depthKm, lat, lon);
    }));
}

void AppController::onDistrictComputed() {
    // 时效性由「同一时刻只有一个任务在跑」保证：期间到来的新请求只置 pending，
    // 故这里拿到的结果必是最近一次派发的，不会用旧结果覆盖新状态。
    districtRunning_ = false;
    applyDistrictIntensity(districtWatcher_.result());
    if (districtPending_) {
        districtPending_ = false;
        recomputeDistrictIntensity();
    }
}

void AppController::applyDistrictIntensity(QVariantMap next) {
    if (next == districtIntensity_) return;
    districtIntensity_ = std::move(next);
    emit districtIntensityChanged();
}

void AppController::updateWaveTimer() {
    // 波前圆只服务于显式焦点事件：无焦点时地图只画"最近一次事件"的 X 标记，
    // 不画波前圆（否则会给历史目录条目留下无意义的圆）。
    const EarthquakeEvent* event = hasMapFocus_ ? &mapFocus_ : nullptr;
    const long long now = nowMs();
    // 目录里的历史地震早已超出量程，只有"还年轻"的焦点事件才需要按帧重算。
    const bool young = event && !event->isCanceled && !sessions_.finished(event->identity()) &&
                       event->timestamp > 0 && now - event->timestamp <= kWaveWindowMs;
    if (young) {
        updateWaveRadii();
        if (!waveTimer_.isActive()) waveTimer_.start();
        return;
    }
    waveTimer_.stop();
    if (wavePKm_ != -1.0 || waveSKm_ != -1.0) {
        wavePKm_ = -1.0;
        waveSKm_ = -1.0;
        wavePOpacity_ = 0.0;
        waveSOpacity_ = 0.0;
        waveSFillOpacity_ = 0.0;
        emit waveRadiiChanged();
    }
}

void AppController::updateWaveRadii() {
    if (!hasMapFocus_) return;
    const EarthquakeEvent* event = &mapFocus_;
    if (event->timestamp <= 0) return;
    const double seconds = (nowMs() - event->timestamp) / 1000.0;
    if (seconds < 0.0) return;

    // 反解走时表得到波前半径（km）。两张表在 2000 km 处拼接，切换是连续的。
    TravelTimeService& travel = TravelTimeService::instance();
    double p = travel.distanceForTime(event->depth, seconds, true, TravelTimeService::kDefaultTable);
    double s = travel.distanceForTime(event->depth, seconds, false, TravelTimeService::kDefaultTable);
    // jma2001 到量程上限会 clamp 回 2000 km，`>` 永远触发不了，故用 `>=` 才能切到 jb 表；
    // 端点处 jb 返回值与 jma2001 连续，不会跳变。
    if (p >= kWaveTableSwitchKm) p = travel.distanceForTime(event->depth, seconds, true, QStringLiteral("jb"));
    if (s >= kWaveTableSwitchKm) s = travel.distanceForTime(event->depth, seconds, false, QStringLiteral("jb"));

    // jb 表上限 10000 km 同样会 clamp，到端点即视为超出量程不再画。
    double pKm = p >= kWaveMaxRadiusKm ? -1.0 : p;
    double sKm = s >= kWaveMaxRadiusKm ? -1.0 : s;
    // 波前"影响半径"：CSIS 降到可感下限（I）时的震中距。这是有意取代 kanameishi 的
    // 经验式 clamp(50·M²,200,2000)。震级未知（<=0）时无法反解，退回量程上限，保证波前仍可见。
    const double fadeKm = event->magnitude > 0.0
                              ? IntensityCalculator::distanceForCsis(event->magnitude, event->depth, kCsisFadeLevel)
                              : kWaveMaxRadiusKm;
    // 透明度归零即视为隐藏：半径记 -1，取景与绘制都会跳过。
    if (IntensityCalculator::waveOpacity(pKm, fadeKm, kWaveMaxRadiusKm) <= 0.0) pKm = -1.0;
    if (IntensityCalculator::waveOpacity(sKm, fadeKm, kWaveMaxRadiusKm) <= 0.0) sKm = -1.0;
    if (pKm == wavePKm_ && sKm == waveSKm_) return;
    wavePKm_ = pKm;
    waveSKm_ = sKm;
    wavePOpacity_ = pKm > 0.0 ? IntensityCalculator::waveOpacity(pKm, fadeKm, kWaveMaxRadiusKm) : 0.0;
    waveSOpacity_ = sKm > 0.0 ? IntensityCalculator::waveOpacity(sKm, fadeKm, kWaveMaxRadiusKm) : 0.0;
    waveSFillOpacity_ = sKm > 0.0 ? IntensityCalculator::waveFillOpacity(sKm, fadeKm) : 0.0;
    emit waveRadiiChanged();
    if (wavePKm_ < 0.0 && waveSKm_ < 0.0) waveTimer_.stop();
}

void AppController::requestLocation() { location_->requestCurrentPosition(); }

void AppController::setManualLocation(double latitude, double longitude, const QString& label) {
    location_->setManual(latitude, longitude, label);
}

void AppController::setWindowVisible(bool visible) {
    if (directoryRefreshEnabled_ == visible) return;
    directoryRefreshEnabled_ = visible;
    for (EarthquakeSource* source : sources_) source->setDirectoryPollingEnabled(visible);
    if (visible) refreshCatalog();
}

void AppController::refreshCatalog() {
    if (!directoryRefreshEnabled_) return;
    for (EarthquakeSource* source : sources_)
        if (settings_->isSourceEnabled(source->id())) source->refreshDirectory();
}

void AppController::loginJian(const QString& loginKey) {
    if (jian_) jian_->login(loginKey);
}

void AppController::setWhewsToken(const QString& token) {
    settings_->setWhewsToken(token);
    whews_->setToken(token);
}

void AppController::setSimulatedUrl(const QString& url) {
    settings_->setSimulatedUrl(url);
    // 写设置即触发 changed → 上面那段会把新地址推入源并按需启停；
    // 这里再显式推一次，保证在 changed 已被节流或顺序变化时也立即生效。
    simulated_->setUrl(settings_->simulatedUrl());
}

void AppController::refreshClock() {
    if (clock_) clock_->refresh();
}

QPointF AppController::wgs84ToGcj02(double lat, double lng) const {
    const auto p = CoordinateTransform::wgs84ToGcj02(lat, lng);
    return QPointF(p.second, p.first); // x=lon, y=lat
}

QPointF AppController::gcj02ToWgs84(double lat, double lng) const {
    const auto p = CoordinateTransform::gcj02ToWgs84(lat, lng);
    return QPointF(p.second, p.first);
}

void AppController::dismissWarningOverlay() {
    auto it = sessions_.active.find(selectedWarning_);
    if (it != sessions_.active.end()) it->second.hidden = true;
    overlayVisible_ = false;
    emit warningOverlayChanged();
}

void AppController::muteWarning() {
    auto it = sessions_.active.find(selectedWarning_);
    if (it == sessions_.active.end()) return;
    it->second.muted = true;
    announcer_->stopOutput();
    syncWarning();
}

void AppController::stopWarning() {
    auto it = sessions_.active.find(selectedWarning_);
    if (it == sessions_.active.end()) return;
    announcer_->finish(it->second.event, true);
    sessions_.finish(selectedWarning_, nowMs());
    historyStore_->saveTombstone(QString::fromStdString(selectedWarning_), nowMs());
    syncWarning();
}

QString AppController::colorForIntensity(double raw) const {
    return SeismicColors::intensityColor(raw).name();
}

QString AppController::colorForMagnitude(double magnitude) const {
    return SeismicColors::magnitudeColor(magnitude).name();
}

QString AppController::severityColor(const QString& levelTag) const {
    return SeismicColors::severity(levelFromTag(levelTag), darkMode_).name();
}

QString AppController::identityOf(const EarthquakeEvent& event) {
    return QString::fromStdString(event.identity());
}

bool AppController::samePhysicalEvent(const EarthquakeEvent& a, const EarthquakeEvent& b) {
    return QuakeCalculator::isSameQuake(a.timestamp, a.latitude, a.longitude,
                                        b.timestamp, b.latitude, b.longitude);
}

void AppController::handleEvent(const EarthquakeEvent& incoming, bool replay, bool inDirectory,
                                EarthquakeSource* source) {
    // 处置结果回报给源：墓碑 / 重复这些在链路上不报错，不回报对自建源就等于静默丢弃。
    const auto admit = [source](const EarthquakeEvent& e, AdmissionStatus status) {
        if (source) source->onAdmission(e, status);
    };
    EarthquakeEvent event = incoming;
    EewParser::UserLocation user;
    if (hasLocation()) user = std::make_pair(userLatitude(), userLongitude());
    EewParser::recompute(event, user, settings_->intensityStandard() == 1
        ? IntensityStandard::Jma : IntensityStandard::Csis);
    SentryService::instance()->addBreadcrumb(
        QStringLiteral("quake.event"),
        QStringLiteral("[%1] %2 M%3 (inDir=%4)")
            .arg(QString::fromStdString(event.eventId), QString::fromStdString(event.location))
            .arg(event.magnitude)
            .arg(inDirectory ? QStringLiteral("true") : QStringLiteral("false")),
        QStringLiteral("info"));
    // Directory and warning gates are deliberately independent.
    if (inDirectory) {
        // 逐源信息事件震级过滤：只影响目录展示，不拦截实时预警链路。
        if (source && !settings_->passesInfoMagnitudeFilter(source->id(), event.magnitude)) {
            admit(event, AdmissionStatus::Filtered);
            return;
        }
        const auto decision = gate_.admit(event, nowMs());
        if (decision == EventGateDecision::Duplicate || decision == EventGateDecision::Stale) {
            admit(event, AdmissionStatus::Duplicate);
            return;
        }
        // 同一物理地震可能已由实时预警或别的源先入库：新事件只通知一次。
        bool alreadyKnown = false;
        for (const auto& h : history_) {
            if (samePhysicalEvent(h, event)) { alreadyKnown = true; break; }
        }
        if (!alreadyKnown) {
            for (const auto& entry : sessions_.active) {
                if (samePhysicalEvent(entry.second.event, event)) { alreadyKnown = true; break; }
            }
        }
        if (isAlertLevel(event.warningLevel)) event.warningLevel = WarningLevel::Watch;
        const bool changed = upsertHistory(event);
        if (changed) {
            queueHistoryWrite(event);
            // 仅对「刚发生」的新信息提醒；启动时回填几百条历史不应刷屏。
            const long long stamp = event.reportTime > 0 ? event.reportTime : event.timestamp;
            const long long now = nowMs();
            const bool recent = stamp > 0 && stamp <= now + 60'000 && now - stamp <= 30 * 60'000;
            if (!alreadyKnown && recent) {
                const IntensityStandard standard = settings_->intensityStandard() == 1
                    ? IntensityStandard::Jma : IntensityStandard::Csis;
                emit notifiableEvent(toMap(event, standard), QStringLiteral("info"));
            }
        }
        if (hasMapFocus_ && mapFocus_.identity() == event.identity() &&
            !sessions_.active.count(event.identity())) {
            // 目录事件没有推送时间；保留实时链路已知的最后一报推送时间，
            // 使 10 分钟窗口仍以「最后一报推送时间」为准。
            const long long reportTime = mapFocus_.reportTime;
            mapFocus_ = event;
            if (mapFocus_.reportTime <= 0) mapFocus_.reportTime = reportTime;
        }
        scheduleDirectoryUiRefresh();
        admit(event, AdmissionStatus::Applied);
        return;
    }
    // 不做震级过滤：所有实时事件都进入生命周期（用于展示/HUD/倒计时），
    // 是否提醒由 AlertAnnouncer 依据「预警总开关 + 本地烈度」决定。
    const auto change = sessions_.accept(event, nowMs());
    if (change == WarningSession::Change::Ignored) {
        // 墓碑与门控去重都归为 Ignored，但成因不同：前者是「这个事件已结束过」，
        // 后者是「这一报和已收的重复」。分开回报才能定位问题。
        admit(event, sessions_.finished(event.identity()) ? AdmissionStatus::Tombstoned
                                                          : AdmissionStatus::Duplicate);
        return;
    }
    if (change == WarningSession::Change::Ended) {
        historyStore_->saveTombstone(identityOf(event), nowMs());
        announcer_->finish(event, selectedWarning_ == event.identity());
        if (hasMapFocus_ && mapFocus_.identity() == event.identity()) {
            mapFocus_ = event;
            emit mapEventChanged();
            emit hudEventChanged();
        }
        syncWarning();
        admit(event, AdmissionStatus::Ended);
        return;
    }
    const bool added = change == WarningSession::Change::Added;
    if (added && !replay && !event.isCanceled) {
        const IntensityStandard standard = settings_->intensityStandard() == 1
            ? IntensityStandard::Jma : IntensityStandard::Csis;
        // 只有「达成本地门槛 + 预警级」才算 alert；其余实时事件归 eew（默认不打扰）。
        const bool alertTier = announcer_->eligible(event) && isAlertLevel(event.warningLevel);
        emit notifiableEvent(toMap(event, standard),
                             alertTier ? QStringLiteral("alert") : QStringLiteral("eew"));
    }
    const long long now = nowMs();
    const bool eventIsProtected = isProtectedAlert(event, now);

    bool currentIsProtected = false;
    auto curIt = sessions_.active.find(selectedWarning_);
    if (curIt != sessions_.active.end()) {
        currentIsProtected = isProtectedAlert(curIt->second.event, now);
    }

    if (added) {
        // 若当前正处于触发本地烈度限制且有倒计时的关键预警保护中，而新事件未达本地预警门槛（普通观测数据/情报/远震）：
        // 允许新数据存入会话供观测切换，但绝不掐断当前警报声音、不抢走当前预警的主焦点和倒计时，避免遮挡。
        if (currentIsProtected && !eventIsProtected) {
            sessions_.active.at(event.identity()).hidden = replay;
        } else {
            announcer_->stopOutput();
            selectedWarning_ = event.identity();
            sessions_.active.at(selectedWarning_).hidden = replay;
            setMapFocus(event, false);
        }
    } else if (hasMapFocus_ && mapFocus_.identity() == event.identity()) {
        mapFocus_ = event;
        updateWaveTimer();
        emit mapEventChanged();
        emit hudEventChanged();
    }
    syncWarning();
    const auto& state = sessions_.active.at(event.identity());
    if (!replay && !state.muted && selectedWarning_ == event.identity() &&
        change != WarningSession::Change::Corrected)
        announcer_->onWarning(event);
    admit(event, AdmissionStatus::Applied);
}

bool AppController::upsertHistory(const EarthquakeEvent& event) {
    for (int i = 0; i < history_.size(); ++i) {
        if (history_[i].id == event.id) {
            if (sameStoredEvent(history_[i], event)) return false;
            history_[i] = event;
            return true;
        }
    }
    history_.push_back(event);
    return true;
}

void AppController::queueHistoryWrite(const EarthquakeEvent& event) {
    if (!historyStore_ || !historyStore_->isReady()) return;
    pendingHistoryWrites_[event.id] = event;   // 同 id 覆盖，避免重复写
    if (!historyFlushTimer_.isActive()) historyFlushTimer_.start();
}

void AppController::flushHistoryWrites() {
    historyFlushTimer_.stop();
    if (pendingHistoryWrites_.empty()) return;
    QVector<EarthquakeEvent> batch;
    batch.reserve(static_cast<int>(pendingHistoryWrites_.size()));
    for (const auto& entry : pendingHistoryWrites_) batch.push_back(entry.second);
    pendingHistoryWrites_.clear();
    if (!historyStore_ || !historyStore_->isReady()) return;
    // upsertAll 内部是一个事务；prune 每次同步最多跑一次。
    historyStore_->upsertAll(batch);
    historyStore_->prune(400);
}

bool AppController::isProtectedAlert(const EarthquakeEvent& e, long long clockNow) const {
    return !e.isCanceled && announcer_ && announcer_->eligible(e) && e.remainingSeconds(clockNow) >= 0;
}

void AppController::syncWarning() {
    const long long now = nowMs();
    auto selected = sessions_.active.find(selectedWarning_);
    const bool currentIsProtected = (selected != sessions_.active.end() && isProtectedAlert(selected->second.event, now));

    // 优选保障：如果有满足「触发本地烈度限制且有倒计时」的关键预警，
    // 绝不能让非关键事件（如远方观测事件）作为 activeWarning 占据主焦点。
    if (!currentIsProtected) {
        for (auto it = sessions_.active.begin(); it != sessions_.active.end(); ++it) {
            if (isProtectedAlert(it->second.event, now)) {
                selected = it;
                selectedWarning_ = it->first;
                break;
            }
        }
    }

    if (selected == sessions_.active.end() && !sessions_.active.empty()) {
        selected = sessions_.active.begin();
        selectedWarning_ = selected->first;
    }
    hasActiveWarning_ = selected != sessions_.active.end();
    overlayVisible_ = false;
    countdown_ = -1;
    if (hasActiveWarning_) {
        activeWarning_ = selected->second.event;
        // 未达提醒门槛（总开关 / 本地烈度过滤）时不给倒计时，避免被过滤掉的事件仍显示预警卡。
        const bool eligible = announcer_->eligible(activeWarning_);
        countdown_ = eligible ? activeWarning_.remainingSeconds(now) : -1;
        overlayVisible_ = !selected->second.hidden && eligible;
        countdownTimer_.start();
    } else {
        selectedWarning_.clear();
        countdownTimer_.stop();
    }
    updateWaveTimer();
    scheduleHudRefresh();
    scheduleDistrictRefresh();
    emit mapEventChanged();
    emit warningChanged();
    emit countdownChanged();
    emit warningOverlayChanged();
    emit hudEventChanged();
    refreshEventModel();
    emit eventListChanged();
}

void AppController::recomputeEvents() {
    EewParser::UserLocation user;
    if (hasLocation()) user = std::make_pair(userLatitude(), userLongitude());
    const auto standard = settings_->intensityStandard() == 1
        ? IntensityStandard::Jma : IntensityStandard::Csis;
    for (auto& e : history_) {
        EewParser::recompute(e, user, standard);
        if (isAlertLevel(e.warningLevel)) e.warningLevel = WarningLevel::Watch;
    }
    for (auto& [key, state] : sessions_.active) EewParser::recompute(state.event, user, standard);
    if (hasMapFocus_) {
        EewParser::recompute(mapFocus_, user, standard);
        if (!sessions_.active.count(mapFocus_.identity()) && isAlertLevel(mapFocus_.warningLevel))
            mapFocus_.warningLevel = WarningLevel::Watch;
    }
    const auto selected = sessions_.active.find(selectedWarning_);
    if (selected != sessions_.active.end() && !announcer_->eligible(selected->second.event))
        announcer_->stopOutput();
    onTick();
    syncWarning();
    rebuildHistory();
}

void AppController::onTick() {
    const long long now = nowMs();
    const auto old = activeWarning_;
    const auto ended = sessions_.tick(now);
    for (const auto& key : ended) {
        historyStore_->saveTombstone(QString::fromStdString(key), now);
        EarthquakeEvent e = old;
        // Only the selected event owns the shared audio output.
        if (key == old.identity()) announcer_->finish(e, true);
    }
    if (!ended.empty()) syncWarning();
    auto it = sessions_.active.find(selectedWarning_);
    if (it == sessions_.active.end()) return;
    activeWarning_ = it->second.event;
    // 未达提醒门槛的事件不给倒计时，也不触发倒计时/到时音（被过滤掉的事件只展示）。
    const int seconds = announcer_->eligible(activeWarning_)
        ? activeWarning_.remainingSeconds(now) : -1;
    if (seconds != countdown_) {
        countdown_ = seconds;
        emit countdownChanged();
        emit hudEventChanged();
    }
    if (it->second.muted) return;
    if (seconds > 0) announcer_->onCountdown(activeWarning_, now);
    else if (seconds == 0 && !it->second.arrived) {
        it->second.arrived = true;
        announcer_->onArrived(activeWarning_);
    }
}

void AppController::rebuildHistory() {
    std::sort(history_.begin(), history_.end(),
              [](const EarthquakeEvent& a, const EarthquakeEvent& b) {
                  return a.timestamp > b.timestamp;
              });
    while (history_.size() > 200) history_.removeLast();
    refreshEventModel();
    // 无焦点时地图画的就是 history_[0]，列表一变就要重画。
    updateWaveTimer();
    emit mapEventChanged();
    emit historyChanged();
    emit eventListChanged();
}

void AppController::refreshEventModel() {
    if (eventListModel_) eventListModel_->setEvents(eventList());
}

void AppController::scheduleDirectoryUiRefresh() {
    if (!directoryUiTimer_.isActive()) directoryUiTimer_.start(0);
}

} // namespace komira
