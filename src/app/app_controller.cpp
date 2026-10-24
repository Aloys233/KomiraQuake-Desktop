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
#include <cctype>
#include <cmath>
#include <unordered_map>
#include <utility>

#include "core/coordinate_transform.h"
#include "core/intensity_calculator.h"
#include "core/ip_geo_lookup.h"
#include "core/quake_calculator.h"
#include "core/quake_key.h"
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
#include "source/sub_source_catalog.h"
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
/// 同一偏移的毫秒形式。格式化墙钟时用 `fromMSecsSinceEpoch(ts + 偏移, QTimeZone::utc())`：
/// 先把时间戳平移到 UTC+8 再按 UTC 渲染，不必每行构造带偏移的 QTimeZone。
/// （`Qt::UTC` 版重载在 Qt 6.9 起已弃用，会报 -Wdeprecated-declarations。）
constexpr qint64 kClockUtc8OffsetMs = 8LL * 3600LL * 1000LL;

/// 发震时刻的固定 UTC+8 文本（与设备时区无关）。列表/HUD 共用同一条格式化路径。
QString utc8Text(long long epochMs) {
    return QDateTime::fromMSecsSinceEpoch(epochMs + kClockUtc8OffsetMs, QTimeZone::utc())
        .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

/// 本地震源推算在焦点竞争中的优先级。高于 USGS/GQ，低于权威地震机构。
constexpr int kEstimateFocusRank = 2;

/// 报数机构优先级（数值越大越优先）：
///   3 = 权威地震机构（JMA / CEA / CENC / CWA / KMA / 省级地震局等）
///   2 = 本地震源推算（`kEstimateFocusRank`，不是事件，故不经过本函数）
///   1 = 其它机构（EMSC / GFZ / BMKG 等）
///   0 = USGS / GQ
///
/// 用户要求「USGS 和 GQ 永远是最低的」：它们**不得**抢走更高优先级事件的焦点，
/// 同优先级之间仍按发震时刻/受保护预警的既有规则裁决。
int agencyPriority(const EarthquakeEvent& e) {
    const QString raw = QString::fromStdString(e.sourceAgency).trimmed();
    const QString agency = raw.toUpper();
    if (agency == QLatin1String("USGS") || agency == QLatin1String("GQ")) return 0;
    // 兼容旧库/旧数据源未写 sourceAgency 的条目：标题里带 GlobalQuake / USGS 也按最低档。
    if (agency.isEmpty()) {
        const QString source = QString::fromStdString(e.source);
        if (source.contains(QLatin1String("GlobalQuake")) || source.contains(QLatin1String("USGS")))
            return 0;
    }
    static const QStringList kAuthoritative = {
        QStringLiteral("JMA"), QStringLiteral("CEA"), QStringLiteral("CENC"),
        QStringLiteral("CWA"), QStringLiteral("KMA"), QStringLiteral("HKO"),
        QStringLiteral("SC"), QStringLiteral("FJ"), QStringLiteral("CQ"),
        QStringLiteral("SA"),
    };
    if (kAuthoritative.contains(agency)) return 3;
    // 省级机构在 Whews 里是中文名（北京地震局 / 云南地震局 / 宁夏地震局），upper() 无效。
    if (raw.contains(QStringLiteral("地震局"))) return 3;
    return 1;
}

/// 只比较会落库的字段：目录每次整表重发，字段没变就不必再写一次库。
bool sameStoredEvent(const EarthquakeEvent& a, const EarthquakeEvent& b) {
    return a.eventId == b.eventId && a.magnitude == b.magnitude && a.latitude == b.latitude &&
           a.longitude == b.longitude && a.depth == b.depth && a.location == b.location &&
           a.timestamp == b.timestamp && a.reportTime == b.reportTime && a.source == b.source &&
           a.sourceProvider == b.sourceProvider && a.sourceAgency == b.sourceAgency &&
           a.sourceId == b.sourceId &&
           a.distanceKm == b.distanceKm && a.estimatedIntensity == b.estimatedIntensity &&
           a.rawIntensity == b.rawIntensity && a.maxIntensityRaw == b.maxIntensityRaw &&
           a.maxIntensityText == b.maxIntensityText && a.pWaveArrival == b.pWaveArrival &&
           a.sWaveArrival == b.sWaveArrival && a.warningLevel == b.warningLevel &&
           a.reportNum == b.reportNum && a.isFinal == b.isFinal && a.isCanceled == b.isCanceled &&
           a.isInfo == b.isInfo && a.determination == b.determination;
}

QString sourceTagFor(const EarthquakeEvent& event) {
    const QString provider = QString::fromStdString(event.sourceProvider).trimmed();
    const QString agency = QString::fromStdString(event.sourceAgency).trimmed();
    if (provider.isEmpty()) return agency;
    return agency.isEmpty() ? provider : provider + QStringLiteral("·") + agency;
}

void appendSourceTag(QStringList& tags, const EarthquakeEvent& event) {
    const QString tag = sourceTagFor(event);
    if (!tag.isEmpty() && !tags.contains(tag)) tags.push_back(tag);
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
    m["timeText"] = utc8Text(e.timestamp);
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
    m["isInfo"] = e.isInfo;
    // 报数文案在 C++ 侧成型：情报没有「第 N 报」语义（上游一条电文即一次定稿，
    // 解析器给的 reportNum 恒为 1），各处 QML 不必各自判断。
    if (e.isInfo)
        m["reportNumText"] = e.isCanceled ? QStringLiteral("取消情报") : QStringLiteral("情报");
    else if (e.isCanceled)
        m["reportNumText"] = QStringLiteral("取消报");
    else
        m["reportNumText"] = QStringLiteral("第 %1 报").arg(e.reportNum > 0 ? e.reportNum : 1);
    m["determinationText"] = e.determination == QLatin1String("reviewed")
                                 ? QStringLiteral("正式测定")
                                 : (e.determination == QLatin1String("automatic")
                                        ? QStringLiteral("自动测定")
                                        : QString());
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
    // 时效筛选（近 24h）用校时后的 now，与列表时间文本同源。
    eventModel_->setNowProvider([this]() { return nowMs(); });
    systemDark_ = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    darkMode_ = resolveDarkMode();
    // themeMode 是唯一的用户意图来源；系统配色只在「跟随系统」时参与判定。
    // 只听逐项信号：聚合的 changed() 在拖动音量滑块时每帧都发，若挂在这里会让
    // 主题判定 + 区县重算跟着滑块抖动。
    connect(settings_, &SettingsStore::themeModeChanged, this, [this]() {
        if (darkMode_ != resolveDarkMode()) {
            darkMode_ = resolveDarkMode();
            emit darkModeChanged();
        }
    });
    // 区县着色依赖「矢量叠加 / 底图」与事件本身：只在相关项变化时重算。
    connect(settings_, &SettingsStore::vectorOverlayChanged, this,
            [this]() { scheduleDistrictRefresh(); });
    connect(settings_, &SettingsStore::basemapIdChanged, this,
            [this]() { scheduleDistrictRefresh(); });
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
            &AppController::onSourceEstimateChanged);
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
        // 卡片数据只在**内容真变了**时才重建并通知：10 Hz 下数值多数帧是不动的
        // （静稳台网亦然），旧实现每帧都为五路源重算文本并强制 QML 重绑。
        // 地图图层另走 StationSnapshot::changed()，与这里解耦。
        if (rebuildStationCards()) emit stationIntensityChanged();
    });

    // 波前圆按显示帧率重算，只在焦点事件"还年轻"时跑（见 updateWaveTimer）。
    // 目录事件逐条写库会变成「每条一个事务 + 一次 prune」，启动/复显时能把 GUI 线程
    // 卡住 1–2 s（实测 200 条约 400 ms）。攒 80 ms 一次事务写回。
    historyFlushTimer_.setSingleShot(true);
    historyFlushTimer_.setInterval(80);
    connect(&historyFlushTimer_, &QTimer::timeout, this, &AppController::flushHistoryWrites);

    // 16 ms 的粗定时器在负载下会被合并到 100+ ms，波前圆看起来一卡一卡；
    // 显式要求精确定时器（内核高精度唤醒），代价只在它活跃的那几十秒。
    waveTimer_.setTimerType(Qt::PreciseTimer);
    waveTimer_.setInterval(16);
    connect(&waveTimer_, &QTimer::timeout, this, &AppController::updateWaveRadii);

    // 自动焦点跨过 10 分钟窗口时刷新一次 HUD 可见性（无需等下一次数据/告警）。
    hudRefreshTimer_.setSingleShot(true);
    connect(&hudRefreshTimer_, &QTimer::timeout, this, [this]() { emit hudEventChanged(); });

    directoryUiTimer_.setSingleShot(true);
    connect(&directoryUiTimer_, &QTimer::timeout, this, [this]() {
        refreshEventModel();
        refreshOpenEventDetail();
        emit hudEventChanged();
    });

    // 数据源状态：250 ms 合并窗口。状态徽章与设置页只需人眼可辨的刷新率，
    // 而 WS 心跳/延迟更新是每帧一条。
    statusRefreshTimer_.setSingleShot(true);
    statusRefreshTimer_.setInterval(250);
    connect(&statusRefreshTimer_, &QTimer::timeout, this, [this]() {
        if (rebuildSources()) emit statusChanged();
    });

    // 区县烈度着色：单次定时器合并连发事件，只算最后一次；计算本体派给 worker 线程。
    districtTimer_.setSingleShot(true);
    connect(&districtTimer_, &QTimer::timeout, this, &AppController::recomputeDistrictIntensity);
    connect(&districtWatcher_, &QFutureWatcher<QVariantMap>::finished, this,
            &AppController::onDistrictComputed);

    // 属性缓存打底：QML 在首帧就会读 `app.sources` / `app.stationSources`。
    // 若等到第一条网络帧才填充，界面会先闪一下「空列表」再长出卡片。
    rebuildSources();
    rebuildStationCards();
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
    for (EarthquakeSource* source : std::as_const(sources_)) {
        source->setNowProvider([this]() { return nowMs(); });
        source->setMonoProvider([this]() { return clock_->elapsedMs(); });
        connect(source, &EarthquakeSource::eventReceived, this,
                [this, source](const EarthquakeEvent& e, SourceEventKind kind) {
                    handleEvent(e, false, kind == SourceEventKind::Directory, source);
                });
        // 高频状态（每条 WS 报文都会 infoChanged）合并到 250 ms 一次：
        // 见 scheduleStatusRefresh 的说明。
        connect(source, &EarthquakeSource::infoChanged, this,
                &AppController::scheduleStatusRefresh);
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
        for (EarthquakeSource* source : std::as_const(sources_)) {
            if (location_->hasLocation())
                source->setUserLocation(location_->latitude(), location_->longitude());
            else
                source->clearUserLocation();
        }
        recomputeEvents();
    });

    wireSettings();

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
    // 启动时也要先装好子源过滤器：持久化的禁用集合必须从第一帧起生效。
    applySubSourceFilters();
    for (EarthquakeSource* source : std::as_const(sources_)) {
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

void AppController::wireSettings() {
    // 逐项接线：每段只在**真正相关**的设置项变化时才跑。
    //
    // 旧实现把 41 个设置项全挂在聚合的 SettingsStore::changed 上：拖动一次音量滑块
    // （每帧一次 setAlertVolume）就会重推五路数据源的启停、重算全部事件的烈度、
    // 发 statusChanged、甚至触发一次自动检查更新 —— 滑块拖动期间 GUI 线程被反复占满。
    // 现在拆成下面几条独立的连接：写哪个键就只跑哪一段。

    // ---- 1. 数据源门控：只随「启用集合 / 凭据 / 开发者模式 / 强度标准」变化 ----
    const auto applySources = [this]() { applySourceSettings(); };
    connect(settings_, &SettingsStore::disabledSourcesChanged, this, applySources);
    connect(settings_, &SettingsStore::disabledStationSourcesChanged, this, applySources);
    // 子源启停不影响连接本身：只重推过滤谓词并立即刷新目录（开回频道时不必等下一轮轮询）。
    connect(settings_, &SettingsStore::disabledSubSourcesChanged, this, [this]() {
        applySubSourceFilters();
        for (EarthquakeSource* source : std::as_const(sources_)) {
            if (startServices_ && settings_->isSourceEnabled(source->id()) && source->isConfigured())
                source->refreshDirectory();
        }
        if (rebuildSources()) emit statusChanged();
    });
    connect(settings_, &SettingsStore::jianRefreshTokenChanged, this, applySources);
    connect(settings_, &SettingsStore::whewsTokenChanged, this, applySources);
    connect(settings_, &SettingsStore::developerModeChanged, this, applySources);
    connect(settings_, &SettingsStore::simulatedUrlChanged, this, applySources);
    connect(settings_, &SettingsStore::intensityStandardChanged, this, applySources);

    // ---- 2. 测站图层显隐 ----
    // showStationLayer 只影响 QML 的 visible 绑定（StationMapItem.visible 直接绑设置），
    // 不改变台站表，故**不**触发 noteStationTablesChanged()：旧实现每次改任意设置都
    // 强制重建约 2500 条悬停元数据，是设置页拖动时的主要浪费之一。

    // ---- 3. 震源推算：只有它的参数/开关会作废旧解 ----
    const auto reloadEstimation = [this]() { sourceEstimation_->reloadSettings(); };
    connect(settings_, &SettingsStore::sourceEstimationEnabledChanged, this, reloadEstimation);
    connect(settings_, &SettingsStore::sourceEstimationRegionParamsChanged, this, reloadEstimation);
    connect(settings_, &SettingsStore::enabledSourceEstimationRegionsChanged, this, reloadEstimation);
    connect(settings_, &SettingsStore::sourceEstimationHoldSecondsChanged, this, reloadEstimation);
    // 开发者模式决定「中国 / 韩国 / 台湾」是否可用（可用才生效），因此它也会改变
    // 各区的生效开关，必须同样作废旧解 —— 否则退出开发者模式后那三区仍在算。
    connect(settings_, &SettingsStore::developerModeChanged, this, reloadEstimation);
    // 地图标记的显示开关只影响呈现，但该属性没有别的通知源：不发这一下，
    // QML 会一直停在关闭时那份 `{ visible: false }` 上，重新打开也不恢复。
    // 同时：标记被隐藏时推算不能再把镜头拉到一个看不见的点上。
    connect(settings_, &SettingsStore::showEstimatedEpicenterChanged, this, [this]() {
        if (!settings_->showEstimatedEpicenter() && !focusEstimate_.isEmpty()) {
            dropEstimateFocus();
            emit resetMapRequested();
        }
        emit sourceEstimateChanged();
    });

    // ---- 4. 声音：音量滑块只改音量，不碰别的 ----
    connect(settings_, &SettingsStore::enableSoundAlertChanged, this, [this]() {
        sound_->setEnabled(settings_->enableSoundAlert());
    });
    connect(settings_, &SettingsStore::alertVolumeChanged, this, [this]() {
        sound_->setVolume(settings_->alertVolume());
    });
    connect(settings_, &SettingsStore::isMutedChanged, this,
            [this]() { announcer_->onMuteChanged(); });

    // ---- 5. 校时 ----
    connect(settings_, &SettingsStore::customNtpServerChanged, this, [this]() {
        clock_->setCustomServer(settings_->customNtpServer());
    });
    connect(settings_, &SettingsStore::enableNtpSyncChanged, this, [this]() {
        clock_->setEnabled(startServices_ && settings_->enableNtpSync());
    });

    // ---- 6. 告警门控相关：过滤阈值/总开关/定位变化会改变已收事件的“是否需要提醒”，
    //         但不改变事件本身的距离与烈度 —— 走轻量的 syncWarning，不重算全表。 ----
    connect(settings_, &SettingsStore::enableWarningsChanged, this, [this]() {
        syncWarning();
        emit statusChanged();
    });
    connect(settings_, &SettingsStore::localIntensityFilterChanged, this, [this]() {
        syncWarning();
        emit statusChanged();
    });
    // 逐源信息事件震级过滤在展示层求值：改阈值/切「不接收」后立即重筛侧栏，
    // 不必等下一次目录刷新或重启。
    connect(settings_, &SettingsStore::infoMagnitudeChanged, this,
            [this]() { scheduleDirectoryUiRefresh(); });

    // ---- 7. 自动检查更新 ----
    connect(settings_, &SettingsStore::autoCheckUpdatesChanged, this,
            [this]() { maybeAutoCheckUpdates(); });
}

void AppController::applySubSourceFilters() {
    for (EarthquakeSource* source : std::as_const(sources_)) {
        const QString sourceId = source->id();
        source->setSubSourceFilter([this, sourceId](const QString& subId) {
            return settings_->isSubSourceEnabled(sourceId, subId);
        });
    }
}

void AppController::applySourceSettings() {
    const IntensityStandard standard = settings_->intensityStandard() == 1
        ? IntensityStandard::Jma : IntensityStandard::Csis;
    // 开发者模式与地址须先于启停生效：否则刚开启开发者模式要等下次启动才连上。
    simulated_->setDevMode(settings_->developerMode());
    simulated_->setUrl(settings_->simulatedUrl());
    // 凭据可能刚被写入：先推给需要的源，再按门控启停。
    jian_->setRefreshToken(settings_->jianRefreshToken());
    whews_->setToken(settings_->whewsToken());
    applySubSourceFilters();
    for (EarthquakeSource* source : std::as_const(sources_)) {
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
    // 右上角的测站源小卡只列出「已启用」的源：启停后要刷新一次属性通知。
    if (rebuildStationCards()) emit stationIntensityChanged();
    // 左下角徽章只显示 enabled && configured，启停/凭据变化后必须同步刷新。
    if (rebuildSources()) emit statusChanged();
    // 强度标准变了：已收事件的“显示烈度”需要按新口径重算一次。
    recomputeEvents();
    emit statusChanged();
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

bool AppController::rebuildSources() {
    auto describe = [this](const DataSourceInfo& info, bool enabled, bool configured) {
        QVariantMap m;
        m["id"] = info.id;
        m["name"] = info.name;
        m["enabled"] = enabled;
        // 是否已配置（与启动门控一致）：左下角徽章据此只展示真正在跑的源。
        m["configured"] = configured;
        m["status"] = info.statusLabel();
        m["statusTag"] = info.statusTag();
        // 延迟分两路给出：数值（-1 = 未测出）供 QML 判空与排序，文本供直接展示。
        // 旧版只给文本，QML 用 `latency !== "— ms"` 判断「有没有测到」——字符串比较既
        // 脆弱又容易被本地化文案撞车，且拿不到数值就没法做「<100 ms 才算良好」这类判断。
        m["latencyMs"] = info.latencyMs;
        m["latencyText"] = info.latencyMs >= 0 ? QStringLiteral("%1 ms").arg(info.latencyMs)
                                              : QString();
        switch (info.directoryStatus) {
        case ConnectionStatus::Connected: m["directory"] = QStringLiteral("目录刷新成功"); break;
        case ConnectionStatus::Connecting: m["directory"] = QStringLiteral("目录刷新中"); break;
        case ConnectionStatus::Error:
            m["directory"] = QStringLiteral("目录刷新失败：") + info.directoryError; break;
        default:         m["directory"] = QStringLiteral("目录未刷新"); break;
        }
        m["directoryStatusTag"] = directoryTag(info.directoryStatus);
        m["directoryLatencyMs"] = info.directoryLatencyMs;
        m["directoryLatencyText"] = info.directoryLatencyMs >= 0
            ? QStringLiteral("%1 ms").arg(info.directoryLatencyMs) : QString();
        m["description"] = info.description;
        // 覆盖范围。设置页右栏的概况行读它区分「全球聚合」与「自测」；少这一项时
        // QML 里 region.length 会读到 undefined，每帧刷一条 TypeError。
        m["region"] = info.region;
        // 子数据源（频道）元数据 + 当前启停状态。QML 的逐项绑定必须读这个快照：
        // isSubSourceEnabled() 是 Q_INVOKABLE，绑在函数调用上的绑定不会自动重算。
        QVariantList subs;
        for (const SubSourceInfo& sub : subSourcesFor(info.id)) {
            QVariantMap item;
            item["id"] = sub.id;
            item["name"] = sub.name;
            item["kind"] = sub.kind == SourceEventKind::Live
                ? QStringLiteral("live") : QStringLiteral("directory");
            item["enabled"] = settings_->isSubSourceEnabled(info.id, sub.id);
            subs.push_back(item);
        }
        m["subSources"] = subs;
        return m;
    };
    QVariantList list;
    list.reserve(sources_.size());
    for (EarthquakeSource* source : std::as_const(sources_))
        list.push_back(describe(source->info(), settings_->isSourceEnabled(source->id()),
                                source->isConfigured()));
    if (list == sourcesCache_) return false;
    sourcesCache_ = std::move(list);
    return true;
}

void AppController::scheduleStatusRefresh() {
    if (!statusRefreshTimer_.isActive()) statusRefreshTimer_.start();
}

QVariantMap AppController::clockInfo() const { return clock_->info(); }

QString AppController::clockTextUtc8() const {
    // 固定 +08:00 时区渲染，与设备时区无关。
    return utc8Text(nowMs());
}

QVariantList AppController::history() const {
    // 懒构造 + 缓存：属性只在真正被读（或首次 emit 后 QML 重新求值）时才付这
    // 200 × 全字段 map 的代价。目录刷新期间不再无条件重建。
    if (!historyDirty_) return historyCache_;
    QVariantList list;
    list.reserve(history_.size());
    for (const auto& e : std::as_const(history_)) list.push_back(toMap(e));
    historyCache_ = std::move(list);
    historyDirty_ = false;
    return historyCache_;
}

/// 同一份上游报文的几条转送记录之间选代表：机构优先级 > 报次（越新越完整）>
/// 聚合源名（**字典序固定**）。刻意不用推送时刻做平手裁决：两路轮询时刻不同，
/// 用时刻会让代表记录在聚合源之间来回换，行的 id（identity）随之变化，列表会
/// 「自己跳」。同一报次的内容来自同一份上游报文，取哪一路在语义上没有区别。
bool preferCatalogRecord(const EarthquakeEvent& a, const EarthquakeEvent& b) {
    const int rankA = agencyPriority(a);
    const int rankB = agencyPriority(b);
    if (rankA != rankB) return rankA > rankB;
    if (a.reportNum != b.reportNum) return a.reportNum > b.reportNum;
    const QString providerA = QString::fromStdString(a.sourceProvider).toCaseFolded();
    const QString providerB = QString::fromStdString(b.sourceProvider).toCaseFolded();
    if (providerA != providerB) return providerA < providerB;
    return a.id < b.id;
}

/// 上游报文 ID：去掉聚合商加的频道前缀（`jma_eqlist:` / `jma_eew:` / `cenc:` …）。
/// 同一条上游报文在不同链路里的前缀可能不同（甚至没有），所以比较前统一剥掉。
std::string upstreamEventId(const EarthquakeEvent& e) {
    const std::string& id = e.eventId.empty() ? e.id : e.eventId;
    const auto colon = id.find(':');
    if (colon == std::string::npos) return id;
    const std::string rest = id.substr(colon + 1);
    return rest.empty() ? id : rest;
}

std::string lowercaseAgency(const std::string& agency) {
    std::string out = agency;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

/// 目录去重键。**只在明确是同一份上游报文时相同**：
/// 同一报数机构 + 同一上游事件 ID；数据源提供方（Wolfx / Pancakes / Jian…）不参与，
/// 因为它们是同一份报文的重复转送。
///
/// 刻意不含「发震时刻 + 震中」窗口：同一机构附近连续发生的两场小地震会因此被压成
/// 一条，且「最佳记录」的震级/地点/时间会覆盖掉另一条 —— 等于丢事件。
std::string catalogKey(const EarthquakeEvent& e) {
    if (e.sourceAgency.empty()) return e.identity();
    return lowercaseAgency(e.sourceAgency) + "|" + upstreamEventId(e);
}

/// 跨链路兜底键：**报数机构 + 发震时刻**（要求毫秒级相同，不开窗口）。
/// CENC 会按分发链路给同一场地震不同的 EventID（同一 M7.0：Wolfx 拿到
/// `AU.20261022223344.0`、Jian 拿到 `CC.20261022224454.0` —— 中段是各链路的报文
/// 时刻，不是发震时刻），此时 catalogKey 对不上，只能靠发震时刻认人：同一机构不会在
/// 同一毫秒发出两场地震，再配合行内震中邻近复核，不会把相邻小震压成一条。
std::string catalogAgencyTimeKey(const EarthquakeEvent& e) {
    if (e.sourceAgency.empty() || e.timestamp <= 0) return {};
    return lowercaseAgency(e.sourceAgency) + "|" + std::to_string(e.timestamp);
}

/// 第三级兜底：**机构 + 发震时刻的分钟桶**。
///
/// 各链路对同一份速报的秒数处理不同 —— Wolfx 的 cenc_eqlist 保留秒（`16:47:57`），
/// Whews / Jian 的同一条只到分钟（`16:47:00`）—— 毫秒级相等的兜底键因此对不上，
/// 同一场地震会在列表里占两行。分钟桶能覆盖这种截断差（情报链路一律是**截断**而不是
/// 四舍五入，见 core/quake_key.h），桶内再按震中距离复核，相邻两场小震不会被压成一条。
std::vector<std::string> catalogAgencyMinuteKeys(const EarthquakeEvent& e) {
    std::vector<std::string> keys;
    if (e.sourceAgency.empty() || e.timestamp <= 0) return keys;
    // 实时预警不参与这条兜底：它一秒一报，同一分钟内本来就有多条。
    if (!e.isInfo) return keys;
    const std::string agency = lowercaseAgency(e.sourceAgency);
    for (const QString& key : quakeSameMinuteKeys(e.sourceAgency, e.timestamp, e.latitude,
                                                  e.longitude)) {
        // 去掉机构前缀（这里统一用小写机构拼接，和 catalogAgencyTimeKey 同形）。
        const QString rest = key.section(QLatin1Char('|'), 1);
        if (!rest.isEmpty()) keys.push_back(agency + "|" + rest.toStdString());
    }
    return keys;
}

std::vector<AppController::CatalogEntry> AppController::catalogEvents() const {
    // 归并口径 = **同一份上游报文的重复聚合链路**：先按 catalogKey（机构 + 上游 ID），
    // 再由 catalogAgencyTimeKey（机构 + 发震时刻 + 震中邻近）兜底 —— Wolfx / Pancakes /
    // Jian / Whews 各自转送的同一报合成一行，并保留全部来源标签。
    //
    // 除此之外各数据源的目录记录保持独立 —— 不做时间窗合并（相邻 30 s 内的两场小地震
    // 不会被压成一条，也不会让「最佳记录」覆盖掉另一条的震级/地点/时间）：兜底键要求
    // 发震时刻**完全相同**，那只会命中同一份报文的不同 ID。按 Kanameishi 的口径，只有
    // 明确重复的聚合源才合并。
    //
    // 实时会话不参与建行：只按 identity 给已有目录行打 active 标记。
    std::unordered_map<std::string, int> slotByKey;
    slotByKey.reserve(static_cast<std::size_t>(history_.size()));
    // 上游 ID 对不上时的兜底索引：机构 + 发震时刻 → 已建行的下标（见 catalogAgencyTimeKey）。
    std::unordered_map<std::string, std::vector<int>> slotsByAgencyTime;
    slotsByAgencyTime.reserve(static_cast<std::size_t>(history_.size()));
    // 第二级兜底：机构 + 发震时刻分钟桶 + 震中网格 → 已建行的下标（见 catalogAgencyMinuteKeys）。
    std::unordered_map<std::string, std::vector<int>> slotsByAgencyMinute;
    slotsByAgencyMinute.reserve(static_cast<std::size_t>(history_.size()));
    std::vector<CatalogEntry> out;
    out.reserve(static_cast<std::size_t>(history_.size()));

    const auto passesInfoFilter = [this](const EarthquakeEvent& e) {
        // 旧库记录没有 sourceId：无从复核逐源阈值，按「通过」处理。
        if (e.sourceId.empty()) return true;
        return settings_->passesInfoMagnitudeFilter(QString::fromStdString(e.sourceId), e.magnitude);
    };
    // 同一份上游报文在两条链路里的秒数处理可能不同（一条保留秒、一条截断到分钟），
    // 震中也会随测定修正抖动几十公里内的零头；候选行必须再按「同一场地震」复核。
    const auto adoptFromBucket = [&out](const EarthquakeEvent& incoming,
                                        const std::vector<int>& bucket) {
        for (const int index : bucket) {
            const EarthquakeEvent& candidate = out[static_cast<std::size_t>(index)].event;
            if (QuakeCalculator::haversineDistance(candidate.latitude, candidate.longitude,
                                                   incoming.latitude, incoming.longitude)
                > QuakeCalculator::kSameQuakeRadiusKm)
                continue;
            return index;
        }
        return -1;
    };
    // 分钟桶只在「确实有链路丢精度」时才算同一份报文：整分钟的发震时刻说明这一路把秒
    // 截断了，才可能与另一路差几十秒（见 core/quake_key.h 的同名判据）。
    // 两路都带秒却不同秒，是同一分钟内的两场地震。
    const auto truncatedToMinute = [](const EarthquakeEvent& e) {
        return quakeTimestampTruncatedToMinute(e.timestamp);
    };

    for (const auto& e : std::as_const(history_)) {
        const std::string key = catalogKey(e);
        int slot = -1;
        if (const auto exact = slotByKey.find(key); exact != slotByKey.end()) {
            slot = exact->second;
        } else if (const std::string timeKey = catalogAgencyTimeKey(e); !timeKey.empty()) {
            // 上游 ID 不同但可能仍是同一份报文（CENC 跨链路换 ID）：按发震时刻找回原行，
            // 桶内只有同一机构的少数几行，逐个复核震中距离。
            if (const auto bucket = slotsByAgencyTime.find(timeKey);
                bucket != slotsByAgencyTime.end()) {
                slot = adoptFromBucket(e, bucket->second);
            }
        }
        if (slot < 0) {
            // 第三级：同机构、同一分钟、同震中网格 —— 秒被截断到分钟的另一条链路。
            // 只在**这一条**或行内代表记录确实是整分钟时才启用，否则同一分钟内两场
            // 都带秒的地震会被并成一条。
            const auto tryMinute = [&](const EarthquakeEvent& other) {
                if (!truncatedToMinute(e) && !truncatedToMinute(other)) return false;
                const long long delta = e.timestamp > other.timestamp ? e.timestamp - other.timestamp
                                                                      : other.timestamp - e.timestamp;
                return delta <= 60'000;
            };
            for (const std::string& minuteKey : catalogAgencyMinuteKeys(e)) {
                const auto bucket = slotsByAgencyMinute.find(minuteKey);
                if (bucket == slotsByAgencyMinute.end()) continue;
                for (const int index : bucket->second) {
                    if (!tryMinute(out[static_cast<std::size_t>(index)].event)) continue;
                    slot = adoptFromBucket(e, {index});
                    if (slot >= 0) break;
                }
                if (slot >= 0) break;
            }
        }

        if (slot < 0) {
            CatalogEntry entry;
            entry.event = e;
            entry.active = sessions_.active.count(e.identity()) > 0;
            entry.passesInfoFilter = passesInfoFilter(e);
            appendSourceTag(entry.sourceTags, e);
            slot = static_cast<int>(out.size());
            if (const std::string timeKey = catalogAgencyTimeKey(e); !timeKey.empty())
                slotsByAgencyTime[timeKey].push_back(slot);
            for (const std::string& minuteKey : catalogAgencyMinuteKeys(e))
                slotsByAgencyMinute[minuteKey].push_back(slot);
            out.push_back(std::move(entry));
        } else {
            CatalogEntry& entry = out[static_cast<std::size_t>(slot)];
            entry.passesInfoFilter = entry.passesInfoFilter || passesInfoFilter(e);
            appendSourceTag(entry.sourceTags, e);
            if (preferCatalogRecord(e, entry.event)) entry.event = e;
            // 代表记录被换掉后，active 也要按它的 identity 重算（同一上游报文的
            // 不同链路可能只有一路还在活动会话里）。
            if (sessions_.active.count(e.identity()) > 0) entry.active = true;
            // 兜底命中行时也把新键登记到这一行，后续同键记录直接命中。
            if (const std::string timeKey = catalogAgencyTimeKey(e); !timeKey.empty())
                slotsByAgencyTime[timeKey].push_back(slot);
            for (const std::string& minuteKey : catalogAgencyMinuteKeys(e))
                slotsByAgencyMinute[minuteKey].push_back(slot);
        }
        // 兜底命中的另一条 ID 也指向同一行，后续同 ID 记录直接走精确键。
        slotByKey.emplace(key, slot);
    }

    // 排序：发震时刻倒序；同一时刻按机构优先级，最后按 identity 稳定收尾。
    // 次级键必须确定，否则目录整表重发时同刻事件的行序会随遍历顺序抖动。
    std::sort(out.begin(), out.end(), [](const CatalogEntry& a, const CatalogEntry& b) {
        if (a.event.timestamp != b.event.timestamp) return a.event.timestamp > b.event.timestamp;
        const int rankA = agencyPriority(a.event);
        const int rankB = agencyPriority(b.event);
        if (rankA != rankB) return rankA > rankB;
        return a.event.identity() < b.event.identity();
    });
    return out;
}

QVariantList AppController::eventList() const {
    // 兼容入口：完整字段的 QVariantMap 数组。QML 已改用 eventModel 的逐列 role，
    // 这里仅供 C++ 测试/调试使用，故不再随刷新 emit 信号（那曾让每次目录更新
    // 都重建一整份约 200×40 项的 QVariant 树，即使 QML 根本没人读）。
    // 与列表一致：被逐源震级过滤挡住的目录不出现（过滤在展示层求值）。
    const auto catalog = catalogEvents();
    QVariantList out;
    out.reserve(static_cast<qsizetype>(catalog.size()));
    const IntensityStandard standard =
        settings_->intensityStandard() == 1 ? IntensityStandard::Jma : IntensityStandard::Csis;
    for (const auto& entry : catalog) {
        if (!entry.passesInfoFilter) continue;
        QVariantMap m = toMap(entry.event, standard);
        m["isActive"] = entry.active;
        m["sourceTags"] = entry.sourceTags;
        out.push_back(m);
    }
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
    // 推算焦点期间不画任何事件：否则十字/波前会落在另一个位置，与镜头所指的推定震中
    // 相互矛盾（用户会以为那就是同一场地震的震中）。
    if (!focusEstimate_.isEmpty()) return nullptr;
    if (hasMapFocus_) return &mapFocus_;
    if (!history_.isEmpty()) return &history_.first();
    return nullptr;
}

QVariant AppController::focusEstimate() const {
    if (focusEstimate_.isEmpty()) return QVariant();
    return focusEstimate_;
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
    // 推算焦点期间主 HUD 让位给震源推算卡（它才是此刻的主展示）；活动预警仍走
    // 右侧的 WarningHud，不受影响。
    if (!focusEstimate_.isEmpty()) return QVariant();
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
    struct Entry {
        int rank = 1;
        long long timestamp = 0;
        std::string key;
    };
    std::vector<Entry> ordered;
    ordered.reserve(sessions_.active.size());
    for (const auto& [key, state] : sessions_.active)
        ordered.push_back({agencyPriority(state.event), state.event.timestamp, key});
    // 先按机构优先级（JMA/CEA > 其它 > USGS/GQ），同优先级按发震时刻倒序，最后按
    // identity 稳定排序。HUD 的 `n/N` 跟随这个顺序，翻页时第一页就是最该看的事件。
    std::sort(ordered.begin(), ordered.end(), [](const Entry& a, const Entry& b) {
        if (a.rank != b.rank) return a.rank > b.rank;
        if (a.timestamp != b.timestamp) return a.timestamp > b.timestamp;
        return a.key < b.key;
    });
    std::vector<std::string> keys;
    keys.reserve(ordered.size());
    for (const Entry& entry : ordered) keys.push_back(entry.key);
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
        // 用户主动翻页 = 主动选中：按手动焦点处理，后续低优先级事件不得把它顶掉。
        setMapFocus(it->second.event, true);
    }
    // 翻页是显式选择：本次复核必须保留它，不能让 syncWarning 的自动挑选把页码顶回去。
    syncWarning(true);
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
        auto metaList = std::make_shared<const std::vector<StationSnapshot::StationMeta>>(
            std::move(metas));
        stationMetas_ = metaList;
        stationMetaDirty_ = false;
        if (entries.size() != metaList->size()) {
            // 不该发生：条目与元数据同序生成。留一条日志便于定位，随后照样下发。
            qWarning() << "[station] meta/entry size mismatch" << entries.size()
                       << metaList->size();
        }
        // 一帧只构造一次：快照与震源推算共享同一份只读数据，不再各自复制一遍
        // （约 2500 条，10 Hz 下等于每秒几 MB 的纯分配抖动）。
        auto frame = std::make_shared<const std::vector<StationSnapshot::Entry>>(std::move(entries));
        if (sourceEstimation_) sourceEstimation_->ingestObservations(frame);
        stationSnapshot_->setSharedEntries(std::move(frame), std::move(metaList));
        return;
    }
    if (!stationMetas_ || stationMetas_->size() != entries.size()) {
        // 启用/停用了某个测站源却漏标脏：下一帧补一次完整重建（悬停先没有元数据）。
        stationMetaDirty_ = true;
        scheduleStationIntensityRefresh();
    }
    auto frame = std::make_shared<const std::vector<StationSnapshot::Entry>>(std::move(entries));
    if (sourceEstimation_) sourceEstimation_->ingestObservations(frame);
    stationSnapshot_->setSharedEntries(std::move(frame));
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
    // 属性 getter 直接返回缓存：QML 里每个 delegate 都会读一次它，旧实现每次都为
    // 五路源重算文本/时间格式/颜色并新建 5 张 QVariantMap。
    return stationSourcesCache_;
}

bool AppController::rebuildStationCards() {
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
        const QString frameTime =
            seisSource_->lastTimestamp() > 0
                ? QDateTime::fromMSecsSinceEpoch(seisSource_->lastTimestamp() + kClockUtc8OffsetMs,
                                                 QTimeZone::utc())
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
    if (list == stationSourcesCache_) return false;
    stationSourcesCache_ = std::move(list);
    return true;
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
    for (const EarthquakeEvent& event : std::as_const(history_)) {
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
    // 事件焦点接管（权威机构事件或用户点击）：推算焦点让位。
    focusEstimate_.clear();
    mapFocus_ = event;
    hasMapFocus_ = true;
    mapFocusManual_ = manual;
    updateWaveTimer();
    scheduleHudRefresh();
    scheduleDistrictRefresh();
    // 焦点只影响侧栏两行（旧的、新的）：走定向 dataChanged，不重建整表。
    syncEventModelFocus();
    emit mapEventChanged();
    emit mapFocusChanged();
    emit hudEventChanged();
    emit centerMapRequested(event.latitude, event.longitude);
}

void AppController::clearMapFocus() {
    if (!hasMapFocus_ && focusEstimate_.isEmpty()) return;
    hasMapFocus_ = false;
    mapFocusManual_ = false;
    focusEstimate_.clear();
    updateWaveTimer();
    scheduleHudRefresh();
    scheduleDistrictRefresh();
    syncEventModelFocus();
    emit mapEventChanged();
    emit mapFocusChanged();
    emit hudEventChanged();
    emit resetMapRequested();
}

QVariantMap AppController::bestEstimateMarker() const {
    if (!sourceEstimation_) return {};
    QVariantMap marker = sourceEstimation_->marker();
    // 顶层载荷会带一份完整的 markers 数组；焦点只需单条，剥掉可避免每秒把整表
    // 复制进 Q_PROPERTY，也让「载荷有没有变」的比较更稳定。
    marker.remove(QStringLiteral("markers"));
    // 只有可见的解才值得抢镜头；顶层就是置信度最高的那一区。
    if (marker.value(QStringLiteral("visible")).toBool() &&
        marker.contains(QStringLiteral("latitude"))) {
        return marker;
    }
    const QVariantList list = sourceEstimation_->marker().value(QStringLiteral("markers")).toList();
    for (const QVariant& entry : list) {
        const QVariantMap candidate = entry.toMap();
        if (candidate.value(QStringLiteral("visible")).toBool() &&
            candidate.contains(QStringLiteral("latitude"))) {
            return candidate;
        }
    }
    return {};
}

void AppController::setEstimateFocus(const QVariantMap& estimate) {
    if (estimate.isEmpty()) {
        clearMapFocus();
        return;
    }
    focusEstimate_ = estimate;
    hasMapFocus_ = false;
    mapFocusManual_ = false;
    // 事件波前必须停掉：它们属于另一个震源（或上一次事件）。
    updateWaveTimer();
    scheduleHudRefresh();
    scheduleDistrictRefresh();
    syncEventModelFocus();
    emit mapEventChanged();
    emit mapFocusChanged();
    emit hudEventChanged();
    emit centerMapRequested(estimate.value(QStringLiteral("latitude")).toDouble(),
                            estimate.value(QStringLiteral("longitude")).toDouble());
}

void AppController::dropEstimateFocus() {
    if (focusEstimate_.isEmpty()) return;
    focusEstimate_.clear();
    updateWaveTimer();
    scheduleHudRefresh();
    scheduleDistrictRefresh();
    syncEventModelFocus();
    emit mapEventChanged();
    emit mapFocusChanged();
    emit hudEventChanged();
}

void AppController::onSourceEstimateChanged() {
    const QVariantMap best = bestEstimateMarker();
    // 「显示推定震中」关掉时，推算不参与焦点：否则镜头会飞到一个用户看不到的点上。
    const bool markerVisible = settings_ && settings_->showEstimatedEpicenter();
    if (!focusEstimate_.isEmpty()) {
        if (best.isEmpty() || !markerVisible) {
            // 解消失了（保持窗口到期 / 关开关）：回到默认视野，别停在空地。
            dropEstimateFocus();
            emit resetMapRequested();
        } else if (best != focusEstimate_) {
            // 每秒刷新只更新载荷，不重新取景 —— 1 Hz 的镜头补间会持续抖动。
            // 只有「换了一区」（比如中国区解过期、日本区接力）才把镜头带过去。
            const QString previousId = focusEstimate_.value(QStringLiteral("id")).toString();
            const QString nextId = best.value(QStringLiteral("id")).toString();
            focusEstimate_ = best;
            // 反推时刻/深度可能随新报修正，波前半径要跟着重算。
            updateWaveTimer();
            emit mapFocusChanged();
            if (previousId != nextId) {
                emit centerMapRequested(best.value(QStringLiteral("latitude")).toDouble(),
                                        best.value(QStringLiteral("longitude")).toDouble());
            }
        }
        emit sourceEstimateChanged();
        return;
    }
    // 推算参与焦点竞争：只在「没有事件焦点」或「当前焦点是 USGS/GQ」时接管，
    // 且用户手动选中的事件永远优先。权威机构（JMA/CEA…）的事件不会被推算顶掉。
    const bool lowPriorityFocus = hasMapFocus_ && !mapFocusManual_ &&
                                  agencyPriority(mapFocus_) == 0;
    if (!best.isEmpty() && markerVisible && !mapFocusManual_ &&
        (!hasMapFocus_ || lowPriorityFocus)) {
        setEstimateFocus(best);
    }
    emit sourceEstimateChanged();
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
    // 波前圆只服务于显式焦点：事件焦点（活动预警/用户点击）或本地推算焦点。
    // 无焦点时地图只画"最近一次事件"的 X 标记，不画波前圆（否则会给历史目录条目
    // 留下无意义的圆）。
    const long long now = nowMs();
    bool young = false;
    if (!focusEstimate_.isEmpty()) {
        // 推算焦点：用反推的发震时刻。没有时刻（originEpochMs 为 0）就不画。
        const long long origin = focusEstimate_.value(QStringLiteral("originEpochMs")).toLongLong();
        young = origin > 0 && now - origin <= kWaveWindowMs;
    } else if (hasMapFocus_) {
        // 目录里的历史地震早已超出量程，只有"还年轻"的焦点事件才需要按帧重算。
        const EarthquakeEvent& event = mapFocus_;
        young = !event.isCanceled && !sessions_.finished(event.identity()) &&
                event.timestamp > 0 && now - event.timestamp <= kWaveWindowMs;
    }
    if (young) {
        updateWaveRadii();
        if (!waveTimer_.isActive()) waveTimer_.start();
        return;
    }
    waveTimer_.stop();
    clearWaveRadii();
}

void AppController::clearWaveRadii() {
    if (wavePKm_ == -1.0 && waveSKm_ == -1.0) return;
    wavePKm_ = -1.0;
    waveSKm_ = -1.0;
    wavePOpacity_ = 0.0;
    waveSOpacity_ = 0.0;
    waveSFillOpacity_ = 0.0;
    emit waveRadiiChanged();
}

void AppController::updateWaveRadii() {
    if (focusEstimate_.isEmpty() && !hasMapFocus_) {
        clearWaveRadii();
        return;
    }

    // 波前来源：推算焦点用推定震源的震级/深度/反推时刻；否则用焦点事件。
    // 半径只依赖这三个量，地图侧只需知道画在哪里 —— 中心由 QML 的焦点标记决定。
    long long origin = 0;
    double depth = 10.0;
    double magnitude = 0.0;
    if (!focusEstimate_.isEmpty()) {
        origin = focusEstimate_.value(QStringLiteral("originEpochMs")).toLongLong();
        depth = focusEstimate_.value(QStringLiteral("depthKm")).toDouble();
        magnitude = focusEstimate_.value(QStringLiteral("magnitude")).toDouble();
        if (!(depth > 0.0)) depth = 10.0;
    } else {
        origin = mapFocus_.timestamp;
        depth = mapFocus_.depth;
        magnitude = mapFocus_.magnitude;
    }
    if (origin <= 0) {
        // 没有可用的发震时刻（推算解尚缺反推时刻）：不能把上一个事件/上一个解的
        // 波前留在屏幕上。
        clearWaveRadii();
        return;
    }
    const double seconds = (nowMs() - origin) / 1000.0;
    if (seconds < 0.0) {
        clearWaveRadii();
        return;
    }

    // 反解走时表得到波前半径（km）。两张表在 2000 km 处拼接，切换是连续的。
    // 表名用 static：旧实现每帧构造两个 QStringLiteral("jb")，60 Hz 下是纯分配噪声。
    static const QString kJbTable = QStringLiteral("jb");
    TravelTimeService& travel = TravelTimeService::instance();
    double p = travel.distanceForTime(depth, seconds, true, TravelTimeService::kDefaultTable);
    double s = travel.distanceForTime(depth, seconds, false, TravelTimeService::kDefaultTable);
    // jma2001 到量程上限会 clamp 回 2000 km，`>` 永远触发不了，故用 `>=` 才能切到 jb 表；
    // 端点处 jb 返回值与 jma2001 连续，不会跳变。
    if (p >= kWaveTableSwitchKm) p = travel.distanceForTime(depth, seconds, true, kJbTable);
    if (s >= kWaveTableSwitchKm) s = travel.distanceForTime(depth, seconds, false, kJbTable);

    // jb 表上限 10000 km 同样会 clamp，到端点即视为超出量程不再画。
    double pKm = p >= kWaveMaxRadiusKm ? -1.0 : p;
    double sKm = s >= kWaveMaxRadiusKm ? -1.0 : s;
    // 波前"影响半径"：CSIS 降到可感下限（I）时的震中距。这是有意取代 kanameishi 的
    // 经验式 clamp(50·M²,200,2000)。震级未知（<=0）时无法反解，退回量程上限，保证波前仍可见。
    //
    // 该值只取决于 (magnitude, depth)，而两者在事件生命周期内几乎不变 —— 旧实现每帧
    // 都跑一次 40 次迭代的二分（40 次 rawCsis，含 pow/log）。按参数缓存，只在换事件时重算。
    if (waveCacheMagnitude_ != magnitude || waveCacheDepth_ != depth) {
        waveCacheMagnitude_ = magnitude;
        waveCacheDepth_ = depth;
        waveCacheFadeKm_ = magnitude > 0.0
                               ? IntensityCalculator::distanceForCsis(magnitude, depth,
                                                                      kCsisFadeLevel)
                               : kWaveMaxRadiusKm;
    }
    const double fadeKm = waveCacheFadeKm_;

    // 透明度归零即视为隐藏：半径记 -1，取景与绘制都会跳过。
    const double pOpacity =
        pKm > 0.0 ? IntensityCalculator::waveOpacity(pKm, fadeKm, kWaveMaxRadiusKm) : 0.0;
    const double sOpacity =
        sKm > 0.0 ? IntensityCalculator::waveOpacity(sKm, fadeKm, kWaveMaxRadiusKm) : 0.0;
    if (pOpacity <= 0.0) pKm = -1.0;
    if (sOpacity <= 0.0) sKm = -1.0;
    // 只有真正变化的字段才值得发通知：QML 侧的绑定（含 Behavior 动画）据此重算。
    if (pKm == wavePKm_ && sKm == waveSKm_ && pOpacity == wavePOpacity_
        && sOpacity == waveSOpacity_) {
        if (wavePKm_ < 0.0 && waveSKm_ < 0.0) waveTimer_.stop();
        return;
    }
    wavePKm_ = pKm;
    waveSKm_ = sKm;
    wavePOpacity_ = pKm > 0.0 ? pOpacity : 0.0;
    waveSOpacity_ = sKm > 0.0 ? sOpacity : 0.0;
    waveSFillOpacity_ =
        sKm > 0.0 ? IntensityCalculator::waveFillOpacity(sKm, fadeKm) : 0.0;
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
    for (EarthquakeSource* source : std::as_const(sources_)) source->setDirectoryPollingEnabled(visible);
    if (visible) refreshCatalog();
}

void AppController::refreshCatalog() {
    if (!directoryRefreshEnabled_) return;
    for (EarthquakeSource* source : std::as_const(sources_))
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
    queueTombstoneWrite(QString::fromStdString(selectedWarning_), nowMs());
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
    // 情报链路的发震时刻常只到分钟（JMA `time_full` 把秒截断为 :00），与实时预警最多
    // 差 59 s；只用 30 s 窗口会把它判成「另一场地震」，于是同一场地震先由预警提醒、
    // 再由情报提醒一次。跨类型（预警 ↔ 情报）放宽到 90 s，同类型仍是 30 s —— 相邻
    // 两场小震不会被压成一场。
    if (a.timestamp <= 0 || b.timestamp <= 0) return false;
    const long long delta = a.timestamp > b.timestamp ? a.timestamp - b.timestamp
                                                      : b.timestamp - a.timestamp;
    const long long window = a.isInfo != b.isInfo ? 90'000 : QuakeCalculator::kSameQuakeWindowMs;
    if (delta > window) return false;
    return QuakeCalculator::haversineDistance(a.latitude, a.longitude, b.latitude, b.longitude)
           <= QuakeCalculator::kSameQuakeRadiusKm;
}

void AppController::handleEvent(const EarthquakeEvent& incoming, bool replay, bool inDirectory,
                                EarthquakeSource* source) {
    // 处置结果回报给源：墓碑 / 重复这些在链路上不报错，不回报对自建源就等于静默丢弃。
    const auto admit = [source](const EarthquakeEvent& e, AdmissionStatus status) {
        if (source) source->onAdmission(e, status);
    };
    EarthquakeEvent event = incoming;
    // 接收时刻由仓库层统一打戳：数据源只负责解析，归档/详情页都按这个时间展示
    // 「每个源收到数据的时间」。
    event.receivedAt = nowMs();
    // 记录抓取该条目的数据源 ID：设置页的逐源震级过滤在展示层复核时需要它，
    // 重启后从库里读回的历史也要能按同一口径过滤（见 HistoryStore 的 source_id 列）。
    if (source) event.sourceId = source->id().toStdString();
    EewParser::UserLocation user;
    if (hasLocation()) user = std::make_pair(userLatitude(), userLongitude());
    EewParser::recompute(event, user, settings_->intensityStandard() == 1
        ? IntensityStandard::Jma : IntensityStandard::Csis);
    // 归档必须在去重门控之前：同一报经 Wolfx 与 Pancakes 各送一次时，EventGate 会
    // 判后者重复并丢弃，但详情页要显示「这一报被哪几个源收到」。
    recordReport(event);
    SentryService::instance()->addBreadcrumb(
        QStringLiteral("quake.event"),
        QStringLiteral("[%1] %2 M%3 (inDir=%4)")
            .arg(QString::fromStdString(event.eventId), QString::fromStdString(event.location))
            .arg(event.magnitude)
            .arg(inDirectory ? QStringLiteral("true") : QStringLiteral("false")),
        QStringLiteral("info"));
    // Directory and warning gates are deliberately independent.
    if (inDirectory) {
        // 逐源信息事件震级过滤只在**展示层**求值（见 refreshEventModel）：这里仅决定
        // 要不要提醒。记录一律照常入库 —— 否则把阈值调高再调回来时，中间那段目录
        // 会永久丢失，表现为「改完设置要重启才像生效」。
        const bool visibleByFilter =
            !source || settings_->passesInfoMagnitudeFilter(source->id(), event.magnitude);
        const auto decision = gate_.admit(event, nowMs());
        if (decision == EventGateDecision::Duplicate || decision == EventGateDecision::Stale) {
            admit(event, AdmissionStatus::Duplicate);
            return;
        }
        // 同一物理地震可能已由实时预警或别的源先入库：新事件只通知一次。
        bool alreadyKnown = false;
        for (const auto& h : std::as_const(history_)) {
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
            if (visibleByFilter && !alreadyKnown && recent) {
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
        queueTombstoneWrite(identityOf(event), nowMs());
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
        const int newRank = agencyPriority(event);
        // 当前焦点是谁：推算（kEstimateFocusRank）> 事件（按机构）。-1 = 没有焦点。
        int currentRank = -1;
        if (!focusEstimate_.isEmpty()) currentRank = kEstimateFocusRank;
        else if (hasMapFocus_) currentRank = agencyPriority(mapFocus_);

        // 新事件只有「不比当前焦点低」才抢镜头：
        //   · USGS/GQ（0）永远抢不走 JMA/CEA（3）或推算（2）；
        //   · 用户手动选中的焦点只在更高优先级事件到来时让位；
        //   · 受保护的关键预警不被同/低级事件打断（原有语义），但权威机构事件仍可接管。
        bool takeFocus = false;
        if (currentRank < 0) takeFocus = true;
        else if (mapFocusManual_) takeFocus = newRank > currentRank;
        else if (currentIsProtected && !eventIsProtected) takeFocus = newRank > currentRank;
        else takeFocus = newRank >= currentRank;

        if (takeFocus) {
            announcer_->stopOutput();
            // 焦点目标不一定是刚到的那条：会话里若已有更高优先级事件（典型：JMA 已活跃、
            // GQ 又报来一版），镜头必须给 JMA，而不是被这条低优先级新报牵走。
            // 同优先级再比受保护预警与发震时刻，与 syncWarning 的选择口径一致。
            const auto preferred = [&](const EarthquakeEvent& a, const EarthquakeEvent& b) {
                const int rankA = agencyPriority(a);
                const int rankB = agencyPriority(b);
                if (rankA != rankB) return rankA > rankB;
                const bool protectedA = isProtectedAlert(a, now);
                const bool protectedB = isProtectedAlert(b, now);
                if (protectedA != protectedB) return protectedA;
                return a.timestamp > b.timestamp;
            };
            const EarthquakeEvent* target = &event;
            for (const auto& [key, state] : sessions_.active) {
                if (preferred(state.event, *target)) target = &state.event;
            }
            selectedWarning_ = target->identity();
            sessions_.active.at(selectedWarning_).hidden = replay;
            // 推算优先于 USGS/GQ：低优先级事件即使接管会话，也把**镜头**交给可见的
            // 推定震中（推算卡因此成为主展示，事件仍留在列表/会话里）。
            const QVariantMap estimate =
                (agencyPriority(*target) == 0 && settings_ && settings_->showEstimatedEpicenter())
                    ? bestEstimateMarker() : QVariantMap();
            if (!estimate.isEmpty()) setEstimateFocus(estimate);
            else setMapFocus(*target, false);
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
            historyDirty_ = true;
            return true;
        }
    }
    history_.push_back(event);
    historyDirty_ = true;
    return true;
}

void AppController::queueHistoryWrite(const EarthquakeEvent& event) {
    if (!historyStore_ || !historyStore_->isReady()) return;
    pendingHistoryWrites_[event.id] = event;   // 同 id 覆盖，避免重复写
    if (!historyFlushTimer_.isActive()) historyFlushTimer_.start();
}

void AppController::flushHistoryWrites() {
    historyFlushTimer_.stop();
    // 先把待写的墓碑（预警结束的终态）落盘，再写事件：两者都在 GUI 线程的 SQLite 上，
    // 每 80 ms 一次的合并写已经把事务数压到最低。
    if (!historyStore_ || !historyStore_->isReady()) {
        pendingHistoryWrites_.clear();
        pendingReportWrites_.clear();
        return;
    }
    if (!pendingReportWrites_.empty()) {
        QVector<StoredReport> batch;
        batch.reserve(static_cast<qsizetype>(pendingReportWrites_.size()));
        for (const auto& entry : pendingReportWrites_) batch.push_back(entry.second);
        pendingReportWrites_.clear();
        historyStore_->upsertReports(batch);
    }
    if (!pendingHistoryWrites_.empty()) {
        QVector<EarthquakeEvent> batch;
        batch.reserve(static_cast<int>(pendingHistoryWrites_.size()));
        for (const auto& entry : pendingHistoryWrites_) batch.push_back(entry.second);
        pendingHistoryWrites_.clear();
        // upsertAll 内部是一个事务；prune 每次同步最多跑一次。
        historyStore_->upsertAll(batch);
        historyStore_->prune(400);
    }
    flushTombstoneWrites();
}

void AppController::queueTombstoneWrite(const QString& identity, long long endedAt) {
    if (!historyStore_ || !historyStore_->isReady() || identity.isEmpty()) return;
    pendingTombstones_[identity.toStdString()] = endedAt;
    if (!historyFlushTimer_.isActive()) historyFlushTimer_.start();
}

void AppController::flushTombstoneWrites() {
    if (pendingTombstones_.empty()) return;
    if (!historyStore_ || !historyStore_->isReady()) {
        pendingTombstones_.clear();
        return;
    }
    // 每个墓碑是一次独立事务（saveTombstone 内含 3 条语句）。旧实现在预警结束/取消的
    // 瞬间同步调用，一次取消报就会在 GUI 线程上做 3 次 SQL —— 合并进同一条 80 ms 写回。
    const auto batch = std::move(pendingTombstones_);
    pendingTombstones_.clear();
    for (const auto& [identity, endedAt] : batch)
        historyStore_->saveTombstone(QString::fromStdString(identity), endedAt);
}

bool AppController::isProtectedAlert(const EarthquakeEvent& e, long long clockNow) const {
    return !e.isCanceled && announcer_ && announcer_->eligible(e) && e.remainingSeconds(clockNow) >= 0;
}

void AppController::syncWarning(bool keepSelection) {
    const long long now = nowMs();
    // 选择顺序（用户要求）：机构优先级 > 受保护预警 > 发震时刻。
    //   · JMA/CEA 等权威机构的事件永远排在 USGS/GQ 前面 —— 即使后者正带着本地倒计时；
    //   · 同机构下仍优先「达成本地烈度且有倒计时」的关键预警；
    //   · 最后取较新的一报。
    auto selected = sessions_.active.find(selectedWarning_);
    const auto better = [&](const EarthquakeEvent& a, const EarthquakeEvent& b) {
        const int rankA = agencyPriority(a);
        const int rankB = agencyPriority(b);
        if (rankA != rankB) return rankA > rankB;
        const bool protectedA = isProtectedAlert(a, now);
        const bool protectedB = isProtectedAlert(b, now);
        if (protectedA != protectedB) return protectedA;
        return a.timestamp > b.timestamp;
    };
    // 从当前选择出发、只在「严格更优」时替换：完全打平时保留用户翻页/系统上次的
    // 选择，否则同一毫秒的两条事件会让 HUD 的「下一个」按了不动。
    //
    // keepSelection（用户刚翻页）时整段跳过：翻页本身已经决定了看哪一条，这里再按
    // 优先级重挑会把选择改回最高优先级那条，而 mapFocus 已由 setMapFocus 换成新页 ——
    // 结果是 HUD 内容在两条之间来回、页码却一直停在「1 / N」。选中项失效时仍走下面的
    // 兜底循环（selected == end）。
    if (!keepSelection || selected == sessions_.active.end()) {
        for (auto it = sessions_.active.begin(); it != sessions_.active.end(); ++it) {
            if (it == selected) continue;
            if (selected == sessions_.active.end() || better(it->second.event, selected->second.event))
                selected = it;
        }
    }
    if (selected != sessions_.active.end()) selectedWarning_ = selected->first;
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
    // 侧栏一律走 0 ms 合并：一次预警更新里 syncWarning 可能被连调多次（accept→sync、
    // tick→sync、rebuildHistory→refresh），旧实现每次都整表重建 200 行。
    scheduleDirectoryUiRefresh();
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
        queueTombstoneWrite(QString::fromStdString(key), now);
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
    scheduleDirectoryUiRefresh();
    // 无焦点时地图画的就是 history_[0]，列表一变就要重画。
    updateWaveTimer();
    emit mapEventChanged();
    historyDirty_ = true;
    emit historyChanged();
}

void AppController::refreshEventModel() {
    if (!eventListModel_) return;
    const auto catalog = catalogEvents();
    std::vector<EventListModel::Row> rows;
    rows.reserve(catalog.size());
    for (const auto& entry : catalog) {
        // 逐源震级过滤在这里求值：设置页改阈值/切「不接收」后只需重建一次行模型，
        // 已入库的记录不会被删掉，改回来还能重新出现。
        if (!entry.passesInfoFilter) continue;
        rows.push_back(makeRow(entry.event, entry.active, entry.sourceTags));
    }
    eventListModel_->setRows(std::move(rows));
    focusedEventId_ = hasMapFocus_ ? QString::fromStdString(mapFocus_.identity()) : QString();
    eventListModel_->setFocusedId(focusedEventId_);
}

EventListModel::Row AppController::makeRow(const EarthquakeEvent& event, bool active,
                                           const QStringList& sourceTags) const {
    // 只算 delegate 真正会读的字段：文本/颜色/标注在这里一次成型，QML 侧不再做格式化。
    EventListModel::Row row;
    row.id = QString::fromStdString(event.identity());
    row.location = QString::fromStdString(event.location);
    row.source = QString::fromStdString(event.source);
    row.magnitudeText = QString::number(event.magnitude, 'f', 1);
    row.depthText = QString::number(event.depth, 'f', 0);
    row.timestamp = event.timestamp;
    row.magnitude = event.magnitude;
    row.isActive = active;
    // 发震时刻：与校时时钟一致，固定渲染为 UTC+8（与 HUD/事件卡的 timeText 同源）。
    row.timeText = utc8Text(event.timestamp);
    row.sourceTag = sourceTagFor(event);
    row.sourceTags = sourceTags;
    if (row.sourceTags.isEmpty() && !row.sourceTag.isEmpty()) row.sourceTags.push_back(row.sourceTag);

    const bool hasDistance = event.distanceKm >= 0.0;
    row.hasDistance = hasDistance;
    row.distance = hasDistance ? event.distanceKm : -1.0;
    row.distanceText = hasDistance ? QString::number(event.distanceKm, 'f', 0) : QString();

    // 列表徽章：固定展示「震源最大烈度」（震中当地量），不随定位变化。
    const QString maxText = QString::fromStdString(event.maxIntensityText);
    const bool hasMaxIntensity = event.maxIntensityRaw > 0.0 || !maxText.isEmpty();
    double listRaw = event.maxIntensityRaw;
    QString listText = maxText;
    if (!hasMaxIntensity) {
        // 源报缺失时在震中（距离 0）按衰减关系估算，保证列表总有可读烈度。
        listRaw = IntensityCalculator::rawCsis(event.magnitude, 0.0, event.depth);
        listText = settings_->intensityStandard() == 1
                       ? QString::fromStdString(
                             IntensityCalculator::formatJma(event.magnitude, 0.0, event.depth))
                       : QString::fromStdString(IntensityCalculator::formatCsis(listRaw));
    }
    row.intensity = listText.isEmpty() ? QStringLiteral("--") : listText;
    row.intensityLabel = QStringLiteral("最大烈度");
    row.intensityColor = SeismicColors::intensityColor(listRaw).name();
    row.levelTag = QString::fromLatin1(warningTag(event.warningLevel));
    return row;
}

QStringList AppController::reportCandidateKeys(const EarthquakeEvent& event) {
    QStringList keys = quakeGroupKeys(event.sourceAgency, event.timestamp, event.latitude,
                                      event.longitude);
    if (keys.isEmpty()) keys.push_back(QString::fromStdString(event.identity()));
    return keys;
}

QString AppController::reportSlotKey(const EarthquakeEvent& event) {
    // 预警：同报被多路聚合源收到要合成一行，所以槽就是报数。
    // 情报：没有报数（解析器一律给 1），同一场地震还会先出「自动测定」、后出「正式
    // 测定」两条，必须各自留行；用发布时刻做槽，同一份报文重复轮询才会命中同一行。
    if (event.isCanceled && !event.isInfo) return QStringLiteral("c");
    if (!event.isInfo) return QStringLiteral("n:") + QString::number(event.reportNum > 0 ? event.reportNum : 1);
    const long long stamp = event.reportTime > 0 ? event.reportTime : event.timestamp;
    return QStringLiteral("i:") + QString::number(stamp);
}

QString AppController::resolveReportSlot(const ReportGroup& group, const EarthquakeEvent& event) {
    const QString base = reportSlotKey(event);
    const ReportRecord* existing = nullptr;
    for (const ReportRecord& record : group.reports) {
        if (record.reportKey == base) {
            existing = &record;
            break;
        }
    }
    if (!existing) return base;
    // 同一槽已被占用：要么是同一份报文（另一路聚合源转送），要么是同一分钟同一网格里
    // 另一场地震的第 1 报。两者只能靠一起点时刻与上游 ID 区分 —— 同一场地震的各链路
    // 起点时刻一致（差 ≤2 s），相邻两场小震不会。
    if (event.isInfo) return base;   // 情报的槽含发布时刻，撞车即同一份报文
    const long long delta = existing->timestamp > event.timestamp
                                ? existing->timestamp - event.timestamp
                                : event.timestamp - existing->timestamp;
    if (existing->timestamp <= 0 || event.timestamp <= 0 || delta <= 2'000) return base;
    // 不同地震：给报次槽加一个稳定的后缀（上游 ID），同一路重复轮询仍命中同一行。
    const std::string upstream = upstreamEventId(event);
    return base + QLatin1Char('#')
           + (upstream.empty() ? QString::number(event.timestamp)
                               : QString::fromStdString(upstream));
}

QString AppController::resolveReportGroupKey(const EarthquakeEvent& event) const {
    // 先按候选键找**已存在**且校验通过的组：分钟进位、网格跨格时，事件的主键会落在
    // 新的一格里，但它仍然是刚才那场地震的同一报/下一报，必须并进旧组。
    const QStringList keys = reportCandidateKeys(event);
    for (const QString& key : keys) {
        const auto it = reportGroups_.constFind(key);
        if (it == reportGroups_.constEnd()) continue;
        if (quakeGroupMergeable(it->timestamp, it->latitude, it->longitude, event.timestamp,
                                event.latitude, event.longitude))
            return key;
    }
    return keys.isEmpty() ? QString() : keys.first();
}

void AppController::recordReport(const EarthquakeEvent& event) {
    const QString key = resolveReportGroupKey(event);
    if (key.isEmpty()) return;

    ReportGroup& group = reportGroups_[key];
    group.lastReceivedAt = std::max(group.lastReceivedAt, event.receivedAt);
    // 组的参照震源只在首次归档时记录：后续报次的震中/时刻会随测定修正小幅漂移，
    // 若跟着改，「同一场地震」的判定基准会漂走。
    if (group.timestamp <= 0) {
        group.timestamp = event.timestamp;
        group.latitude = event.latitude;
        group.longitude = event.longitude;
    }

    const QString slot = resolveReportSlot(group, event);
    const int num = event.reportNum > 0 ? event.reportNum : 1;
    ReportRecord* record = nullptr;
    for (ReportRecord& existing : group.reports) {
        if (existing.reportKey == slot) {
            record = &existing;
            break;
        }
    }
    bool contentChanged = false;
    if (!record) {
        group.reports.append(ReportRecord{});
        record = &group.reports.last();
        record->reportKey = slot;
        record->reportNum = num;
        contentChanged = true;
    }
    const auto assign = [&contentChanged](auto& field, const auto& value) {
        if (field == value) return;
        field = value;
        contentChanged = true;
    };
    assign(record->reportNum, num);
    assign(record->timestamp, event.timestamp);
    assign(record->reportTime, event.reportTime);
    assign(record->magnitude, event.magnitude);
    assign(record->depth, event.depth);
    assign(record->latitude, event.latitude);
    assign(record->longitude, event.longitude);
    assign(record->location, QString::fromStdString(event.location));
    assign(record->maxIntensityText, QString::fromStdString(event.maxIntensityText));
    assign(record->isFinal, event.isFinal);
    assign(record->isCanceled, event.isCanceled);
    assign(record->isInfo, event.isInfo);
    assign(record->determination, QString::fromStdString(event.determination));

    const QString sourceId = QString::fromStdString(event.sourceId);
    const QString tag = sourceTagFor(event);
    const QString raw = QString::fromStdString(event.rawJson);
    ReportSourceRecord* sourceRecord = nullptr;
    for (ReportSourceRecord& existing : record->sources) {
        if (existing.sourceId == sourceId && existing.sourceTag == tag) {
            sourceRecord = &existing;
            break;
        }
    }
    bool sourceAdded = false;
    bool sourceChanged = false;
    if (!sourceRecord) {
        record->sources.append(ReportSourceRecord{sourceId, tag, event.receivedAt, raw});
        sourceRecord = &record->sources.last();
        sourceAdded = true;
    } else if (sourceRecord->rawJson != raw) {
        sourceRecord->rawJson = raw;
        sourceChanged = true;
    }

    if (!contentChanged && !sourceAdded && !sourceChanged) return;   // 目录轮询的重复帧
    if (contentChanged) {
        for (const ReportSourceRecord& source : std::as_const(record->sources))
            queueReportWrite(key, *record, source);
    } else {
        queueReportWrite(key, *record, *sourceRecord);
    }
    pruneReportGroups();
    refreshDetailIfShowing(key);
}

/// 详情是否正打开着这场地震（可能因「同一报被不同源送达而换了代表记录」导致 detailKey
/// 与本组的键不同）：按物理同一性判断，避免报次更新后详情页停在旧数据上。
void AppController::refreshDetailIfShowing(const QString& groupKey) {
    if (detailId_.isEmpty()) return;
    if (detailKey_ == groupKey) {
        rebuildEventDetail();
        return;
    }
    const ReportGroup group = reportGroups_.value(groupKey);
    if (quakeGroupMergeable(detailEvent_.timestamp, detailEvent_.latitude, detailEvent_.longitude,
                            group.timestamp, group.latitude, group.longitude)) {
        // 同一场地震换了组键（分钟进位/网格跨格）：把详情指向新组，后续报次才读得到。
        detailKey_ = groupKey;
        mergeStoredReports(groupKey);
        rebuildEventDetail();
    }
}

void AppController::queueReportWrite(const QString& groupKey, const ReportRecord& record,
                                     const ReportSourceRecord& source) {
    StoredReport stored;
    stored.quakeKey = groupKey;
    stored.reportKey = record.reportKey;
    stored.reportNum = record.reportNum;
    stored.sourceId = source.sourceId;
    stored.sourceTag = source.sourceTag;
    stored.receivedAt = source.receivedAt;
    stored.rawJson = source.rawJson;
    stored.timestamp = record.timestamp;
    stored.reportTime = record.reportTime;
    stored.magnitude = record.magnitude;
    stored.depth = record.depth;
    stored.latitude = record.latitude;
    stored.longitude = record.longitude;
    stored.location = record.location;
    stored.maxIntensityText = record.maxIntensityText;
    stored.isFinal = record.isFinal;
    stored.isCanceled = record.isCanceled;
    stored.isInfo = record.isInfo;
    stored.determination = record.determination;
    // 待写表按「组 + 报次槽 + 源」去重：同一报经两路聚合源送达是两行（各自存原文），
    // 同一路重复轮询只覆盖一行。
    pendingReportWrites_[groupKey + QChar(0x1f) + record.reportKey + QChar(0x1f)
                         + (source.sourceId.isEmpty() ? source.sourceTag : source.sourceId)] = stored;
    if (!historyFlushTimer_.isActive()) historyFlushTimer_.start();
}

void AppController::pruneReportGroups() {
    // 纯展示缓存，按最后接收时间淘汰即可；不设上限的话长时间运行会把每场小震的
    // 原始报文都留在内存里。
    constexpr int kMaxGroups = 400;
    if (reportGroups_.size() <= kMaxGroups) return;
    QVector<QPair<long long, QString>> order;
    order.reserve(reportGroups_.size());
    for (auto it = reportGroups_.constBegin(); it != reportGroups_.constEnd(); ++it)
        order.push_back({it.value().lastReceivedAt, it.key()});
    std::sort(order.begin(), order.end());
    const int removeCount = reportGroups_.size() - kMaxGroups;
    for (int i = 0; i < removeCount; ++i) reportGroups_.remove(order.at(i).second);
}

void AppController::mergeStoredReports(const QString& groupKey) {
    if (!historyStore_ || !historyStore_->isReady()) return;
    // 读回时把候选键（本格本分钟、相邻分钟、相邻格）下的记录全并进**详情这一组**：
    // 写入端已把同一场地震归一到同一个键，但旧版本入库时可能落在相邻网格/分钟里，
    // 那些行若只读回各自的组，详情页仍然看不到。
    QStringList keys;
    if (!detailEvent_.location.empty()) keys = reportCandidateKeys(detailEvent_);
    if (keys.isEmpty() && !groupKey.isEmpty()) keys.push_back(groupKey);
    // 没打开详情时（例如归档预热）按传入键读，不做跨格合并。
    const QString target = detailEvent_.location.empty() ? groupKey : resolveReportGroupKey(detailEvent_);
    if (target.isEmpty()) return;
    QSet<QString> loaded;
    for (const QString& key : keys) {
        if (key.isEmpty() || loaded.contains(key)) continue;
        loaded.insert(key);
        mergeStoredReportsOf(target, key);
    }
}

void AppController::mergeStoredReportsOf(const QString& targetKey, const QString& sourceKey) {
    if (!historyStore_ || !historyStore_->isReady() || targetKey.isEmpty()) return;
    ReportGroup& group = reportGroups_[targetKey];
    for (const StoredReport& stored : historyStore_->loadReports(sourceKey)) {
        // 相邻键下的行必须复核真是同一场地震：网格相邻不等于同一场。
        if (!quakeGroupMergeable(stored.timestamp, stored.latitude, stored.longitude,
                                 group.timestamp, group.latitude, group.longitude)
            && group.timestamp > 0)
            continue;
        if (group.timestamp <= 0) {
            group.timestamp = stored.timestamp;
            group.latitude = stored.latitude;
            group.longitude = stored.longitude;
        }
        ReportRecord* record = nullptr;
        const QString slot = stored.reportKey.isEmpty()
                                 ? QStringLiteral("n:") + QString::number(stored.reportNum)
                                 : stored.reportKey;
        for (ReportRecord& existing : group.reports) {
            if (existing.reportKey == slot) {
                record = &existing;
                break;
            }
        }
        if (!record) {
            group.reports.append(ReportRecord{});
            record = &group.reports.last();
            record->reportKey = slot;
            record->reportNum = stored.reportNum;
            record->timestamp = stored.timestamp;
            record->reportTime = stored.reportTime;
            record->magnitude = stored.magnitude;
            record->depth = stored.depth;
            record->latitude = stored.latitude;
            record->longitude = stored.longitude;
            record->location = stored.location;
            record->maxIntensityText = stored.maxIntensityText;
            record->isFinal = stored.isFinal;
            record->isCanceled = stored.isCanceled;
            record->isInfo = stored.isInfo;
            record->determination = stored.determination;
        }
        bool hasSource = false;
        for (const ReportSourceRecord& existing : std::as_const(record->sources)) {
            if (existing.sourceId == stored.sourceId && existing.sourceTag == stored.sourceTag) {
                hasSource = true;
                break;
            }
        }
        if (!hasSource) {
            record->sources.append(ReportSourceRecord{stored.sourceId, stored.sourceTag,
                                                      stored.receivedAt, stored.rawJson});
        }
        group.lastReceivedAt = std::max(group.lastReceivedAt, stored.receivedAt);
    }
}

void AppController::showEventDetail(const QString& id) {
    if (id.isEmpty()) return;
    detailId_ = id;
    bool found = false;
    for (const CatalogEntry& entry : catalogEvents()) {
        if (QString::fromStdString(entry.event.identity()) != id) continue;
        detailEvent_ = entry.event;
        detailSourceTags_ = entry.sourceTags;
        detailActive_ = entry.active;
        found = true;
        break;
    }
    if (!found) {
        closeEventDetail();
        return;
    }
    // 归档键要用「已存在的那一组」：同一场地震先由相邻分钟/相邻网格归档时，主键会
    // 落在新的一格里，直接按主键读会看不到刚收到的报次。
    detailKey_ = resolveReportGroupKey(detailEvent_);
    mergeStoredReports(detailKey_);
    rebuildEventDetail();
}
void AppController::closeEventDetail() {
    if (detailId_.isEmpty() && eventDetailCache_.isEmpty()) return;
    detailId_.clear();
    detailKey_.clear();
    detailEvent_ = EarthquakeEvent{};
    detailSourceTags_.clear();
    detailActive_ = false;
    eventDetailCache_.clear();
    emit eventDetailChanged();
}

void AppController::refreshOpenEventDetail() {
    if (detailId_.isEmpty()) return;
    bool found = false;
    for (const CatalogEntry& entry : catalogEvents()) {
        if (QString::fromStdString(entry.event.identity()) != detailId_) continue;
        detailEvent_ = entry.event;
        detailSourceTags_ = entry.sourceTags;
        detailActive_ = entry.active;
        found = true;
        break;
    }
    if (!found) {
        // 目录裁剪后条目可能已经不在列表里；详情页不能继续显示一条不存在的事件。
        closeEventDetail();
        return;
    }
    // 归档键可能与这条记录的「主键」不同（同一场地震先由相邻分钟/相邻网格归档），
    // 必须取进程内已存在的那一组，否则详情页看不到刚收到的报次。
    const QString key = resolveReportGroupKey(detailEvent_);
    if (key != detailKey_) {
        detailKey_ = key;
        mergeStoredReports(key);
    }
    rebuildEventDetail();
}

void AppController::rebuildEventDetail() {
    if (detailId_.isEmpty()) return;
    // 旧库记录（本功能上线前入库）没有 sourceProvider/agency，catalog 标签可能是空的；
    // 至少退回 event.source，否则详情页会显示「暂无来源记录」。
    QStringList displaySourceTags = detailSourceTags_;
    if (displaySourceTags.isEmpty()) {
        const QString tag = sourceTagFor(detailEvent_);
        if (!tag.isEmpty()) displaySourceTags.push_back(tag);
        else if (!detailEvent_.source.empty())
            displaySourceTags.push_back(QString::fromStdString(detailEvent_.source));
    }
    const EventListModel::Row row = makeRow(detailEvent_, detailActive_, displaySourceTags);

    QVariantMap detail;
    detail["id"] = detailId_;
    detail["location"] = row.location;
    detail["timeText"] = row.timeText;
    detail["magnitudeText"] = row.magnitudeText;
    detail["depthText"] = row.depthText;
    detail["distanceText"] = row.hasDistance ? row.distanceText : QStringLiteral("--");
    detail["hasDistance"] = row.hasDistance;
    detail["latitudeText"] = QString::number(detailEvent_.latitude, 'f', 4) + QChar(0x00B0);
    detail["longitudeText"] = QString::number(detailEvent_.longitude, 'f', 4) + QChar(0x00B0);
    detail["intensity"] = row.intensity;
    detail["intensityLabel"] = row.intensityLabel;
    detail["intensityColor"] = row.intensityColor;
    detail["maxIntensity"] = QString::fromStdString(detailEvent_.maxIntensityText);
    detail["active"] = detailActive_;
    const QString localIntensity = QString::fromStdString(detailEvent_.estimatedIntensity);
    detail["localIntensity"] = localIntensity;
    detail["hasLocalIntensity"] = row.hasDistance && !localIntensity.isEmpty()
                                  && localIntensity != QLatin1String("--");
    detail["sourceTags"] = displaySourceTags;
    QString statusText;
    if (detailEvent_.isCanceled) statusText = QStringLiteral("已取消");
    else if (detailEvent_.isFinal) statusText = QStringLiteral("最终报");
    detail["statusText"] = statusText;
    detail["reportNumText"] = detailEvent_.isInfo
                                  ? QStringLiteral("情报")
                                  : (detailEvent_.isCanceled && detailEvent_.reportNum >= 999999
                                         ? QStringLiteral("取消报")
                                         : (detailEvent_.reportNum > 0
                                                ? QStringLiteral("第 %1 报")
                                                      .arg(detailEvent_.reportNum)
                                                : QString()));

    const ReportGroup group = reportGroups_.value(detailKey_);
    QVector<ReportRecord> ordered = group.reports;
    // 排序：实时预警各报按报数升序在前，情报行按发布时刻在后。两类是不同性质的报文
    // （预警是同一场地震的多次修正，情报是事后定稿），混在一起按报数排会让都从 1
    // 开始的两条交替出现。
    std::sort(ordered.begin(), ordered.end(), [](const ReportRecord& a, const ReportRecord& b) {
        if (a.isInfo != b.isInfo) return !a.isInfo;
        if (a.isInfo) {
            const long long stampA = a.reportTime > 0 ? a.reportTime : a.timestamp;
            const long long stampB = b.reportTime > 0 ? b.reportTime : b.timestamp;
            if (stampA != stampB) return stampA < stampB;
            return a.determination < b.determination;
        }
        if (a.reportNum != b.reportNum) return a.reportNum < b.reportNum;
        return a.reportTime < b.reportTime;
    });
    // 旧数据没有报次归档：把 events 行里保留的最后一报当作单条记录展示，
    // 否则老地震的详情页只剩基本信息，用户会以为「看不了」。
    const bool synthesizedReports = ordered.isEmpty() && !detailEvent_.location.empty();
    if (synthesizedReports) {
        ReportRecord fallback;
        fallback.reportKey = reportSlotKey(detailEvent_);
        fallback.reportNum = detailEvent_.reportNum > 0 ? detailEvent_.reportNum : 1;
        fallback.timestamp = detailEvent_.timestamp;
        fallback.reportTime = detailEvent_.reportTime;
        fallback.magnitude = detailEvent_.magnitude;
        fallback.depth = detailEvent_.depth;
        fallback.latitude = detailEvent_.latitude;
        fallback.longitude = detailEvent_.longitude;
        fallback.location = QString::fromStdString(detailEvent_.location);
        fallback.maxIntensityText = QString::fromStdString(detailEvent_.maxIntensityText);
        fallback.isFinal = detailEvent_.isFinal;
        fallback.isCanceled = detailEvent_.isCanceled;
        fallback.isInfo = detailEvent_.isInfo;
        fallback.determination = QString::fromStdString(detailEvent_.determination);
        const QString raw = QString::fromStdString(detailEvent_.rawJson);
        for (const QString& tag : std::as_const(displaySourceTags)) {
            fallback.sources.append(ReportSourceRecord{
                QString::fromStdString(detailEvent_.sourceId), tag, detailEvent_.receivedAt, raw});
        }
        ordered.push_back(fallback);
    }
    detail["reportsSynthesized"] = synthesizedReports;

    // ── 数据源：按来源聚合报次；接收时间取最早/最晚，原始 JSON 取最新一报。
    QMap<QString, QVariantMap> bySource;
    for (const ReportRecord& report : ordered) {
        for (const ReportSourceRecord& source : report.sources) {
            const QString tag = source.sourceTag.isEmpty() ? source.sourceId : source.sourceTag;
            if (tag.isEmpty()) continue;
            auto it = bySource.find(tag);
            if (it == bySource.end()) {
                QVariantMap entry;
                entry["tag"] = tag;
                entry["sourceId"] = source.sourceId;
                entry["firstReceivedAt"] = source.receivedAt;
                entry["lastReceivedAt"] = source.receivedAt;
                entry["firstReceivedText"] = source.receivedAt > 0 ? utc8Text(source.receivedAt)
                                                                  : QStringLiteral("--");
                entry["lastReceivedText"] = entry.value("firstReceivedText");
                entry["reportCount"] = 1;
                entry["rawJson"] = source.rawJson;
                bySource.insert(tag, entry);
                continue;
            }
            QVariantMap& entry = it.value();
            if (source.receivedAt > 0
                && (entry.value("firstReceivedAt").toLongLong() <= 0
                    || source.receivedAt < entry.value("firstReceivedAt").toLongLong())) {
                entry["firstReceivedAt"] = source.receivedAt;
                entry["firstReceivedText"] = utc8Text(source.receivedAt);
            }
            if (source.receivedAt >= entry.value("lastReceivedAt").toLongLong()) {
                entry["lastReceivedAt"] = source.receivedAt;
                if (source.receivedAt > 0) entry["lastReceivedText"] = utc8Text(source.receivedAt);
            }
            entry["reportCount"] = entry.value("reportCount").toInt() + 1;
            if (!source.rawJson.isEmpty()) entry["rawJson"] = source.rawJson;
        }
    }
    // 归档为空（升级前的旧库、或只拿到 catalog 标签）时也要显示来源，只是没有时间/原文。
    // 主来源若 events 行上还留着原文（进程内更新过但未落库），也一并展示。
    const QString primaryTag = displaySourceTags.isEmpty() ? QString() : displaySourceTags.first();
    const QString eventRaw = QString::fromStdString(detailEvent_.rawJson);
    for (const QString& tag : std::as_const(displaySourceTags)) {
        if (tag.isEmpty() || bySource.contains(tag)) continue;
        QVariantMap entry;
        entry["tag"] = tag;
        entry["sourceId"] = QString::fromStdString(detailEvent_.sourceId);
        entry["firstReceivedAt"] = detailEvent_.receivedAt;
        entry["lastReceivedAt"] = detailEvent_.receivedAt;
        entry["firstReceivedText"] = detailEvent_.receivedAt > 0
                                         ? utc8Text(detailEvent_.receivedAt)
                                         : QStringLiteral("--");
        entry["lastReceivedText"] = entry.value("firstReceivedText");
        entry["reportCount"] = synthesizedReports ? 1 : 0;
        entry["rawJson"] = (tag == primaryTag) ? eventRaw : QString();
        bySource.insert(tag, entry);
    }
    QVariantList sources;
    sources.reserve(bySource.size());
    for (auto it = bySource.constBegin(); it != bySource.constEnd(); ++it) {
        QVariantMap entry = it.value();
        entry["hasRaw"] = !entry.value("rawJson").toString().isEmpty();
        sources.push_back(entry);
    }
    detail["sources"] = sources;

    // ── 报数：实时预警逐报在前，情报按发布时刻在后；每条右上角带全部来源标签。
    QVariantList reports;
    reports.reserve(ordered.size());
    for (const ReportRecord& report : ordered) {
        QVariantMap entry;
        entry["reportNum"] = report.reportNum;
        entry["isInfo"] = report.isInfo;
        // 情报没有「第 N 报」：上游一条电文就是一次定稿，解析器给的 reportNum 恒为 1，
        // 直接显示「第 1 报」会让人以为后面还有第 2 报。
        if (report.isInfo) {
            entry["numText"] = report.isCanceled ? QStringLiteral("取消情报")
                                                 : QStringLiteral("情报");
        } else {
            entry["numText"] = report.isCanceled && report.reportNum >= 999999
                                   ? QStringLiteral("取消报")
                                   : QStringLiteral("第 %1 报").arg(report.reportNum);
        }
        const long long stamp = report.reportTime > 0 ? report.reportTime : report.timestamp;
        entry["timeText"] = utc8Text(stamp);
        entry["magnitudeText"] = QString::number(report.magnitude, 'f', 1);
        entry["depthText"] = QString::number(report.depth, 'f', 0);
        entry["location"] = report.location;
        entry["maxIntensityText"] = report.maxIntensityText;
        // 测定状态只对情报有意义：正式测定覆盖自动测定是同一场地震的两次定稿，
        // 两条都留着，靠这个标签区分，否则看起来像重复条目。
        QString determinationText;
        if (report.isInfo) {
            if (report.determination == QLatin1String("reviewed"))
                determinationText = QStringLiteral("正式测定");
            else if (report.determination == QLatin1String("automatic"))
                determinationText = QStringLiteral("自动测定");
        }
        entry["determinationText"] = determinationText;
        QString reportStatus;
        if (report.isCanceled) reportStatus = QStringLiteral("已取消");
        else if (report.isInfo) reportStatus = QString();
        else if (report.isFinal) reportStatus = QStringLiteral("最终报");
        entry["statusText"] = reportStatus;
        QStringList tags;
        for (const ReportSourceRecord& source : report.sources) {
            const QString tag = source.sourceTag.isEmpty() ? source.sourceId : source.sourceTag;
            if (!tag.isEmpty() && !tags.contains(tag)) tags.push_back(tag);
        }
        entry["sourceTags"] = tags;
        reports.push_back(entry);
    }
    detail["reports"] = reports;

    // ── 周边区县烈度：只保留 ≥1 度（低于 1 度对读者没有意义，且会铺满整张表）。
    const QString districtSignature = QStringLiteral("%1|%2|%3|%4")
                                          .arg(detailEvent_.magnitude)
                                          .arg(detailEvent_.depth)
                                          .arg(detailEvent_.latitude)
                                          .arg(detailEvent_.longitude);
    if (detailDistrictSignature_ != districtSignature) {
        detailDistrictSignature_ = districtSignature;
        detailDistrictsCache_.clear();
        if (detailEvent_.magnitude > 0.0) {
            if (!districtData_) {
                auto data = std::make_shared<VectorMapData>();
                if (data->loadFromFile((assetRoot_ + QStringLiteral("/map_bounds.kvb")).toStdString()))
                    districtData_ = std::move(data);
            }
            if (districtData_) {
                const QVariantMap all = computeDistrictIntensity(
                    *districtData_, detailEvent_.magnitude, detailEvent_.depth,
                    detailEvent_.latitude, detailEvent_.longitude);
                for (auto it = all.constBegin(); it != all.constEnd(); ++it) {
                    const double raw = it.value().toDouble();
                    if (raw < 1.0) continue;
                    QVariantMap entry;
                    entry["name"] = it.key();
                    entry["raw"] = raw;
                    entry["intensityText"] =
                        QString::fromStdString(IntensityCalculator::formatCsis(raw));
                    entry["color"] = SeismicColors::intensityColor(raw).name();
                    detailDistrictsCache_.push_back(entry);
                }
                std::sort(detailDistrictsCache_.begin(), detailDistrictsCache_.end(),
                          [](const QVariant& a, const QVariant& b) {
                              const QVariantMap left = a.toMap();
                              const QVariantMap right = b.toMap();
                              if (left.value("raw").toDouble() != right.value("raw").toDouble())
                                  return left.value("raw").toDouble()
                                         > right.value("raw").toDouble();
                              return left.value("name").toString()
                                     < right.value("name").toString();
                          });
            }
        }
    }
    detail["districts"] = detailDistrictsCache_;

    eventDetailCache_ = detail;
    emit eventDetailChanged();
}

void AppController::syncEventModelFocus() {
    if (!eventListModel_) return;
    const QString focused = hasMapFocus_ ? QString::fromStdString(mapFocus_.identity()) : QString();
    if (focused == focusedEventId_) return;
    focusedEventId_ = focused;
    eventListModel_->setFocusedId(focused);
}

void AppController::scheduleDirectoryUiRefresh() {
    if (!directoryUiTimer_.isActive()) directoryUiTimer_.start(0);
}

} // namespace komira
