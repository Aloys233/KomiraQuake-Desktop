#include "app/app_controller.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QList>
#include <QStandardPaths>
#include <QStringList>
#include <QTimeZone>

#include <algorithm>

#include "core/coordinate_transform.h"
#include "core/intensity_calculator.h"
#include "core/ip_geo_lookup.h"
#include "core/quake_calculator.h"
#include "core/travel_time_service.h"
#include "prefs/settings_store.h"
#include "service/alert_announcer.h"
#include "service/alert_sound_service.h"
#include "service/autostart.h"
#include "service/location_service.h"
#include "service/ntp_clock.h"
#include "service/speech_service.h"
#include "service/update_service.h"
#include "source/eew_parser.h"
#include "source/pancakes_source.h"
#include "source/wolfx_source.h"
#include "store/history_store.h"
#include "theme/seismic_colors.h"

namespace komira {

namespace {

/// 校时时钟固定显示时区：UTC+8。《NATIVE_PORT_SPEC》 §12。
constexpr int kClockUtc8OffsetSeconds = 8 * 3600;

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
    // 烈度：有定位用本地预估，否则退回数据源报的最大烈度（HUD / 全屏预警 / 语音沿用本组字段）。
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

int statusRank(ConnectionStatus s) {
    switch (s) {
    case ConnectionStatus::Connected: return 3;
    case ConnectionStatus::Connecting: return 2;
    case ConnectionStatus::Error: return 1;
    case ConnectionStatus::Disconnected: return 0;
    }
    return 0;
}

/// 聚合多数据源的链路状态：任一启用源在线即视为在线（状态栏单条展示）。
DataSourceInfo combineSources(const QList<DataSourceInfo>& all, const QStringList& enabledIds) {
    QList<DataSourceInfo> list;
    for (const auto& info : all)
        if (enabledIds.contains(info.id)) list << info;
    if (list.isEmpty()) {
        DataSourceInfo none;
        none.id = "none";
        none.name = "无启用数据源";
        none.region = "全球";
        return none;
    }
    DataSourceInfo out = list.first();
    QStringList names;
    QStringList descriptions;
    for (const auto& info : list) {
        names << info.name;
        if (!info.description.isEmpty()) descriptions << info.description;
        if (statusRank(info.status) > statusRank(out.status)) out.status = info.status;
        if (statusRank(info.directoryStatus) > statusRank(out.directoryStatus))
            out.directoryStatus = info.directoryStatus;
        if (info.directoryLatencyMs >= 0
            && (out.directoryLatencyMs < 0 || info.directoryLatencyMs < out.directoryLatencyMs))
            out.directoryLatencyMs = info.directoryLatencyMs;
        if (info.directoryLastSuccess > out.directoryLastSuccess)
            out.directoryLastSuccess = info.directoryLastSuccess;
        if (out.directoryError.isEmpty() && !info.directoryError.isEmpty())
            out.directoryError = info.directoryError;
        if (info.lastHeartbeat > out.lastHeartbeat) out.lastHeartbeat = info.lastHeartbeat;
    }
    QStringList ids;
    for (const auto& info : list) ids << info.id;
    out.id = ids.join("+");
    out.name = names.join(QStringLiteral(" · "));
    out.description = descriptions.join("；");
    out.latencyMs = -1;
    for (const auto& info : list) {
        if (info.status == ConnectionStatus::Connected && info.latencyMs >= 0
            && (out.latencyMs < 0 || info.latencyMs < out.latencyMs))
            out.latencyMs = info.latencyMs;
    }
    return out;
}

} // namespace

AppController::AppController(QObject* parent, bool startServices)
    : QObject(parent), startServices_(startServices) {
    QString assetRoot = QCoreApplication::applicationDirPath() + "/assets";
    if (!QFileInfo::exists(assetRoot + "/travel_times.json"))
        assetRoot = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../share/komiraquake/assets");

    TravelTimeService::instance().loadFromFile(assetRoot + "/travel_times.json");
    CityCoordTable::instance().loadFromFile(assetRoot + "/china_cities.json");
    qInfo() << "[app] asset root:" << assetRoot
            << "| travel times:" << (TravelTimeService::instance().isLoaded() ? "loaded" : "MISSING")
            << "| city coords:" << (CityCoordTable::instance().isLoaded() ? "loaded" : "MISSING");

    settings_ = new SettingsStore(this);
    darkMode_ = settings_->darkMode();
    connect(settings_, &SettingsStore::changed, this, [this]() {
        if (darkMode_ != settings_->darkMode()) {
            darkMode_ = settings_->darkMode();
            emit darkModeChanged();
        }
    });
    location_ = new LocationService(this);
    sound_ = new AlertSoundService(this);
    sound_->setAssetRoot(assetRoot);
    speech_ = new SpeechService(this);
    announcer_ = new AlertAnnouncer(settings_, sound_, speech_, this);

    source_ = new WolfxSource(this);
    pancakes_ = new PancakesSource(this);
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

    // 波前圆按 10 Hz 重算，只在焦点事件"还年轻"时跑（见 updateWaveTimer）。
    waveTimer_.setInterval(100);
    connect(&waveTimer_, &QTimer::timeout, this, &AppController::updateWaveRadii);

    // 自动焦点跨过 10 分钟窗口时刷新一次 HUD 可见性（无需等下一次数据/告警）。
    hudRefreshTimer_.setSingleShot(true);
    connect(&hudRefreshTimer_, &QTimer::timeout, this, [this]() { emit hudEventChanged(); });
}

long long AppController::nowMs() const {
    return nowProvider_ ? nowProvider_() : clock_->now();
}

AppController::~AppController() {
    if (historyStore_) delete historyStore_;
}

void AppController::wire() {
    // 校时：数据源与告警编排都走 clock_ 的时间基准。《NATIVE_PORT_SPEC》 §13。
    source_->setNowProvider([this]() { return nowMs(); });
    source_->setMonoProvider([this]() { return clock_->elapsedMs(); });
    pancakes_->setNowProvider([this]() { return nowMs(); });
    pancakes_->setMonoProvider([this]() { return clock_->elapsedMs(); });
    connect(clock_, &NtpClock::changed, this, &AppController::clockChanged);

    connect(source_, &WolfxSource::eventReceived, this,
            [this](const EarthquakeEvent& e, WolfxEventKind kind) {
                handleEvent(e, false, kind == WolfxEventKind::Directory);
            });
    connect(source_, &WolfxSource::infoChanged, this, &AppController::statusChanged);
    connect(pancakes_, &PancakesSource::eventReceived, this,
            [this](const EarthquakeEvent& e, PancakesKind kind) {
                handleEvent(e, false, kind == PancakesKind::Directory);
            });
    connect(pancakes_, &PancakesSource::infoChanged, this, &AppController::statusChanged);

    // 定位只影响"本地烈度/倒计时/全屏预警"，不影响数据源连接。
    connect(location_, &LocationService::changed, this, [this]() {
        emit locationChanged();
        if (location_->hasLocation()) {
            source_->setUserLocation(location_->latitude(), location_->longitude());
            pancakes_->setUserLocation(location_->latitude(), location_->longitude());
        } else {
            source_->clearUserLocation();
            pancakes_->clearUserLocation();
        }
        recomputeEvents();
    });

    connect(settings_, &SettingsStore::changed, this, [this]() {
        const IntensityStandard standard = settings_->intensityStandard() == 1
            ? IntensityStandard::Jma : IntensityStandard::Csis;
        source_->setStandard(standard);
        pancakes_->setStandard(standard);
        speech_->setEnabled(settings_->enableSpeech());
        speech_->setRate(settings_->speechRate());
        speech_->setVolume(settings_->alertVolume());
        sound_->setEnabled(settings_->enableSoundAlert());
        sound_->setVolume(settings_->alertVolume());
        if (startServices_ && settings_->enabledWolfx()) source_->start(); else source_->stop();
        if (startServices_ && settings_->enabledPancakes()) pancakes_->start(); else pancakes_->stop();
        clock_->setCustomServer(settings_->customNtpServer());
        clock_->setEnabled(startServices_ && settings_->enableNtpSync());
        announcer_->onMuteChanged();
        recomputeEvents();
        emit statusChanged();
        maybeAutoCheckUpdates();
    });

    // 启动时应用一次设置
    speech_->setEnabled(settings_->enableSpeech());
    speech_->setRate(settings_->speechRate());
    speech_->setVolume(settings_->alertVolume());
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
    source_->setStandard(startStandard);
    pancakes_->setStandard(startStandard);
    if (startServices_) {
        if (settings_->enabledWolfx()) source_->start();
        if (settings_->enabledPancakes()) pancakes_->start();
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

QStringList AppController::enabledSourceIds() const {
    QStringList ids;
    if (settings_->enabledWolfx()) ids << SourceIds::kWolfx;
    if (settings_->enabledPancakes()) ids << SourceIds::kPancakes;
    return ids;
}

void AppController::maybeAutoCheckUpdates() {
    if (!startServices_ || autoUpdateChecked_ || !settings_->autoCheckUpdates()) return;
    autoUpdateChecked_ = true;
    // 延后到界面与数据源就绪后再联网，避免与启动风暴抢带宽。
    QTimer::singleShot(4000, this, [this]() { updater_->check(true); });
}

QString AppController::statusText() const {
    const DataSourceInfo info = combineSources(
        {source_->info(), pancakes_->info()},
        enabledSourceIds());
    switch (info.status) {
    case ConnectionStatus::Connected: return QStringLiteral("源在线");
    case ConnectionStatus::Connecting: return QStringLiteral("连接中");
    case ConnectionStatus::Error: return QStringLiteral("连接异常");
    case ConnectionStatus::Disconnected: break;
    }
    return QStringLiteral("未连接");
}

QString AppController::statusLevelTag() const {
    const DataSourceInfo info = combineSources(
        {source_->info(), pancakes_->info()},
        enabledSourceIds());
    switch (info.status) {
    case ConnectionStatus::Connected: return QStringLiteral("NORMAL");
    case ConnectionStatus::Error: return QStringLiteral("WARNING");
    default: return QStringLiteral("WATCH");
    }
}

bool AppController::hasLocation() const { return location_->hasLocation(); }
QString AppController::locationName() const { return location_->locationName(); }
double AppController::userLatitude() const { return location_->latitude(); }
double AppController::userLongitude() const { return location_->longitude(); }
QString AppController::locationStatusText() const { return location_->statusText(); }

QVariantList AppController::sources() const {
    auto describe = [](const DataSourceInfo& info, bool enabled) {
        QVariantMap m;
        m["id"] = info.id;
        m["name"] = info.name;
        m["enabled"] = enabled;
        m["status"] = info.statusLabel();
        m["statusTag"] = info.statusTag();
        m["latency"] = info.latencyMs >= 0 ? QStringLiteral("%1 ms").arg(info.latencyMs)
                                           : QStringLiteral("— ms");
        switch (info.directoryStatus) {
        case ConnectionStatus::Connected: m["directory"] = QStringLiteral("目录刷新成功"); break;
        case ConnectionStatus::Connecting: m["directory"] = QStringLiteral("目录刷新中"); break;
        case ConnectionStatus::Error:
            m["directory"] = QStringLiteral("目录刷新失败：") + info.directoryError; break;
        default: m["directory"] = QStringLiteral("目录未刷新"); break;
        }
        m["directoryLatency"] = info.directoryLatencyMs >= 0
            ? QStringLiteral("%1 ms").arg(info.directoryLatencyMs) : QStringLiteral("— ms");
        m["description"] = info.description;
        return m;
    };
    QVariantList list;
    list.push_back(describe(source_->info(), settings_->enabledWolfx()));
    list.push_back(describe(pancakes_->info(), settings_->enabledPancakes()));
    return list;
}

QVariantMap AppController::sourceInfo() const {
    const DataSourceInfo info = combineSources(
        {source_->info(), pancakes_->info()},
        enabledSourceIds());
    QVariantMap m;
    m["id"] = info.id;
    m["name"] = info.name;
    m["region"] = info.region;
    m["status"] = info.statusLabel();
    m["statusTag"] = info.statusTag();
    m["latency"] = info.latencyMs >= 0 ? QStringLiteral("%1 ms").arg(info.latencyMs)
                                       : QStringLiteral("— ms");
    m["description"] = info.description;
    QString directory;
    switch (info.directoryStatus) {
    case ConnectionStatus::Connected: directory = QStringLiteral("目录刷新成功"); break;
    case ConnectionStatus::Connecting: directory = QStringLiteral("目录刷新中"); break;
    case ConnectionStatus::Error: directory = QStringLiteral("目录刷新失败：") + info.directoryError; break;
    default: directory = QStringLiteral("目录未刷新"); break;
    }
    m["description"] = info.description + QStringLiteral(" · ") + directory;
    m["directoryStatus"] = directory;
    m["directoryLastSuccess"] = info.directoryLastSuccess;
    m["directoryLatencyMs"] = info.directoryLatencyMs;
    return m;
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
    return m;
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
    if (isMapFocused(id)) {
        // 再次点击同一事件：取消焦点并收起 HUD（活动预警仍会保持显示）。
        clearMapFocus();
        return;
    }
    if (hasActiveWarning_ && QString::fromStdString(activeWarning_.identity()) == id) {
        setMapFocus(activeWarning_, true);
        return;
    }
    for (const EarthquakeEvent& event : history_) {
        if (QString::fromStdString(event.identity()) == id) {
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
    emit mapEventChanged();
    emit mapFocusChanged();
    emit hudEventChanged();
    emit resetMapRequested();
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
    // P、S 波都离开中国范围后一起隐藏，避免留下无意义的巨大波前圆。
    if (QuakeCalculator::bothWavesBeyondChina(event->latitude, event->longitude, pKm, sKm)) {
        pKm = -1.0;
        sKm = -1.0;
    }
    if (pKm == wavePKm_ && sKm == waveSKm_) return;
    wavePKm_ = pKm;
    waveSKm_ = sKm;
    emit waveRadiiChanged();
    if (wavePKm_ < 0.0 && waveSKm_ < 0.0) waveTimer_.stop();
}

void AppController::setDarkMode(bool dark) {
    if (darkMode_ == dark) return;
    darkMode_ = dark;
    if (settings_) settings_->setDarkMode(dark);
    emit darkModeChanged();
}

void AppController::requestLocation() { location_->requestCurrentPosition(); }

void AppController::setManualLocation(double latitude, double longitude, const QString& label) {
    location_->setManual(latitude, longitude, label);
}

void AppController::refreshCatalog() {
    if (source_ && settings_->enabledWolfx()) source_->refreshDirectory();
    if (pancakes_ && settings_->enabledPancakes()) pancakes_->refreshDirectory();
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

bool AppController::speechAvailable() const { return speech_->available(); }

void AppController::sampleSpeech() { speech_->speakSample(); }

QString AppController::identityOf(const EarthquakeEvent& event) {
    return QString::fromStdString(event.identity());
}

bool AppController::samePhysicalEvent(const EarthquakeEvent& a, const EarthquakeEvent& b) {
    return QuakeCalculator::isSameQuake(a.timestamp, a.latitude, a.longitude,
                                        b.timestamp, b.latitude, b.longitude);
}

void AppController::handleEvent(const EarthquakeEvent& incoming, bool replay, bool inDirectory) {
    EarthquakeEvent event = incoming;
    EewParser::UserLocation user;
    if (hasLocation()) user = std::make_pair(userLatitude(), userLongitude());
    EewParser::recompute(event, user, settings_->intensityStandard() == 1
        ? IntensityStandard::Jma : IntensityStandard::Csis);
    // Directory and warning gates are deliberately independent.
    if (inDirectory) {
        const auto decision = gate_.admit(event, nowMs());
        if (decision == EventGateDecision::Duplicate || decision == EventGateDecision::Stale) return;
        if (isAlertLevel(event.warningLevel)) event.warningLevel = WarningLevel::Watch;
        upsertHistory(event);
        if (hasMapFocus_ && mapFocus_.identity() == event.identity() &&
            !sessions_.active.count(event.identity())) {
            // 目录事件没有推送时间；保留实时链路已知的最后一报推送时间，
            // 使 10 分钟窗口仍以「最后一报推送时间」为准。
            const long long reportTime = mapFocus_.reportTime;
            mapFocus_ = event;
            if (mapFocus_.reportTime <= 0) mapFocus_.reportTime = reportTime;
        }
        if (historyStore_->isReady()) {
            historyStore_->upsertAll({event});
            historyStore_->prune(400);
        }
        rebuildHistory();
        emit hudEventChanged();
        return;
    }
    // 不做震级过滤：所有实时事件都进入生命周期（用于展示/HUD/倒计时），
    // 是否提醒由 AlertAnnouncer 依据「预警总开关 + 本地烈度」决定。
    const auto change = sessions_.accept(event, nowMs());
    if (change == WarningSession::Change::Ignored) return;
    if (change == WarningSession::Change::Ended) {
        historyStore_->saveTombstone(identityOf(event), nowMs());
        announcer_->finish(event, selectedWarning_ == event.identity());
        if (hasMapFocus_ && mapFocus_.identity() == event.identity()) {
            mapFocus_ = event;
            emit mapEventChanged();
            emit hudEventChanged();
        }
        syncWarning();
        return;
    }
    const bool added = change == WarningSession::Change::Added;
    if (added) {
        announcer_->stopOutput();
        selectedWarning_ = event.identity();
        sessions_.active.at(selectedWarning_).hidden = replay;
        setMapFocus(event, false);
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
}

void AppController::upsertHistory(const EarthquakeEvent& event) {
    for (int i = 0; i < history_.size(); ++i) {
        if (history_[i].id == event.id) {
            history_[i] = event;
            return;
        }
    }
    history_.push_back(event);
}

void AppController::syncWarning() {
    auto selected = sessions_.active.find(selectedWarning_);
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
        countdown_ = eligible ? activeWarning_.remainingSeconds(nowMs()) : -1;
        overlayVisible_ = !selected->second.hidden && eligible;
        countdownTimer_.start();
    } else {
        selectedWarning_.clear();
        countdownTimer_.stop();
    }
    updateWaveTimer();
    scheduleHudRefresh();
    emit mapEventChanged();
    emit warningChanged();
    emit countdownChanged();
    emit warningOverlayChanged();
    emit hudEventChanged();
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
    // 无焦点时地图画的就是 history_[0]，列表一变就要重画。
    updateWaveTimer();
    emit mapEventChanged();
    emit historyChanged();
    emit eventListChanged();
}

} // namespace komira
