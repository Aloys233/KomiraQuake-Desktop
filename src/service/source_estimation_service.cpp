#include "service/source_estimation_service.h"

#include <QDateTime>
#include <QTimeZone>
#include <QFutureWatcher>
#include <QMetaObject>
#include <QThread>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <vector>

#include "core/intensity_calculator.h"
#include "core/quake_calculator.h"
#include "core/region_names.h"
#include "core/travel_time_service.h"
#include "prefs/settings_store.h"
#include "service/travel_time_arrivals.h"

namespace komira {

namespace {

/// 区域下标：与 `sourceRegions()` 同序，避免到处写 case。
std::size_t regionIndex(SourceRegion region) {
    const std::array<SourceRegion, 4>& regions = sourceRegions();
    for (std::size_t i = 0; i < regions.size(); ++i) {
        if (regions[i] == region) return i;
    }
    return 0;
}

/// 发震时刻文本（UTC+8 的 `HH:mm:ss`）。未给到时解（0）返回空串，卡片据此隐藏该行。
QString originText(long long epochMs) {
    if (epochMs <= 0) return QString();
    const QDateTime time = QDateTime::fromMSecsSinceEpoch(epochMs, QTimeZone::utc());
    return time.toOffsetFromUtc(8 * 3600).toString(QStringLiteral("HH:mm:ss"));
}

/// 发震时刻全文（`yyyy-MM-dd HH:mm:ss`，UTC+8）。普通 HUD 的 `timeText` 就是这个格式，
/// 震源推算卡改版后照抄同一行文字，两卡上下并排时时刻对得齐。
QString timestampText(long long epochMs) {
    if (epochMs <= 0) return QString();
    const QDateTime time = QDateTime::fromMSecsSinceEpoch(epochMs, QTimeZone::utc());
    return time.toOffsetFromUtc(8 * 3600).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

/// 强度展示文本：与各制式的既有口径一致。
///
/// 用在**烈度徽章**上，所以口径必须与普通 HUD 的徽章逐字一致：CSIS 走罗马数字
/// （`IntensityCalculator::formatCsis`，和事件 HUD 的「预估烈度」同源），JMA 走
/// 5弱/5強 分档，KMA（MMI）是整数。混用阿拉伯数字会让两张卡上下并排时不一致。
QString intensityText(IntensityFamily family, double value) {
    if (!std::isfinite(value)) return QStringLiteral("--");
    switch (family) {
    case IntensityFamily::Jma: {
        // 与 Kmoni 卡片同口径（5弱/5強 等分档）。
        if (value < 0.5) return QStringLiteral("0");
        if (value < 1.5) return QStringLiteral("1");
        if (value < 2.5) return QStringLiteral("2");
        if (value < 3.5) return QStringLiteral("3");
        if (value < 4.5) return QStringLiteral("4");
        if (value < 5.0) return QStringLiteral("5弱");
        if (value < 5.5) return QStringLiteral("5強");
        if (value < 6.0) return QStringLiteral("6弱");
        if (value < 6.5) return QStringLiteral("6強");
        return QStringLiteral("7");
    }
    case IntensityFamily::Kma:
        // 韩国 MMI：0–11 的整数。
        return QString::number(std::max(0, std::min(11, static_cast<int>(std::lround(value)))));
    case IntensityFamily::Csis:
        break;
    }
    return QString::fromStdString(IntensityCalculator::formatCsis(value));
}

} // namespace

SourceEstimationService::SourceEstimationService(SettingsStore* settings, QObject* parent)
    : QObject(parent), settings_(settings) {
    nowProvider_ = []() { return QDateTime::currentMSecsSinceEpoch(); };
    for (std::size_t i = 0; i < regions_.size(); ++i) {
        RegionState& region = regions_[i];
        region.region = sourceRegions()[i];
        region.family = sourceRegionFamily(region.region);
        const SourceRegionDefaults& defaults = sourceRegionDefaults(region.region);
        region.trigger = defaults.trigger;
        region.minStations = defaults.minStations;
        region.quietEvidence = defaults.quietEvidence;
        region.clusterRadiusKm = defaults.clusterRadiusKm;
        // 逐区开关默认关闭：由 syncConfig() 从设置里读，未启用就一直停摆。
        // 生效开关叠加可用性（非开发者模式只有日本区开放），见 syncConfig()。
        const QString id = QString::fromLatin1(sourceRegionId(region.region));
        region.enabled = settings_
                         && settings_->isSourceEstimationRegionEnabled(id)
                         && settings_->isSourceEstimationRegionAvailable(id);
        if (!region.enabled) region.state = QStringLiteral("disabled");
    }
    ticker_.setInterval(1000);
    connect(&ticker_, &QTimer::timeout, this, &SourceEstimationService::tick);
    connect(&watcher_, &QFutureWatcher<EstimateBatch>::finished, this,
            &SourceEstimationService::onEstimated);
    syncConfig();
    ticker_.start();
}

SourceEstimationService::~SourceEstimationService() {
    // worker 里只跑纯函数、按值持有一帧观测；析构时等它收尾，避免悬垂回调。
    if (watcher_.isRunning()) watcher_.waitForFinished();
}

void SourceEstimationService::setNowProvider(std::function<long long()> provider) {
    if (provider) nowProvider_ = std::move(provider);
}

SourceEstimationService::RegionState& SourceEstimationService::regionState(SourceRegion region) {
    return regions_[regionIndex(region)];
}

const SourceEstimationService::RegionState& SourceEstimationService::regionState(
    SourceRegion region) const {
    return regions_[regionIndex(region)];
}

long long SourceEstimationService::estimateOriginMs(const RegionState& region,
                                                    const SourceEstimate& estimate) const {
    if (!estimate.valid) return 0;
    // 日本区的到时路径已经直接解出 origin：那是真正的联合反演结果，不能再用中位数覆盖。
    if (estimate.method.rfind("arrival_time", 0) == 0 && estimate.originEpochMs > 0) {
        return estimate.originEpochMs;
    }
    std::vector<long long> candidates;
    candidates.reserve(static_cast<std::size_t>(region.states.size()));
    for (auto it = region.states.constBegin(); it != region.states.constEnd(); ++it) {
        const StationState& state = it.value();
        if (state.firstTriggerMs <= 0) continue;
        // 站坐标缺失（例如旧簿记条目）就跳过：没有距离就算不出 P 走时。
        if (!std::isfinite(state.latitude) || !std::isfinite(state.longitude)) continue;
        if (state.latitude == 0.0 && state.longitude == 0.0) continue;
        const double epicentralKm = QuakeCalculator::haversineDistance(
            estimate.latitude, estimate.longitude, state.latitude, state.longitude);
        if (!std::isfinite(epicentralKm)) continue;
        // 走时表（JMA2001 / JB）与地图波前、日本区到时反演同源，口径一致。
        const double pSeconds = TravelTimeService::instance()
                                    .estimate(estimate.depthKm, epicentralKm,
                                              epicentralKm > 2000.0 ? QStringLiteral("jb")
                                                                    : QStringLiteral("jma2001"))
                                    .first;
        const long long origin = state.firstTriggerMs -
                                 static_cast<long long>(std::llround(pSeconds * 1000.0));
        candidates.push_back(origin);
    }
    if (candidates.empty()) return 0;
    // 中位数：个别站晚触发几十秒（阈值高、离得远）不该把整条拉走。
    const auto middle = candidates.begin() + static_cast<std::ptrdiff_t>(candidates.size() / 2);
    std::nth_element(candidates.begin(), middle, candidates.end());
    return *middle;
}

bool SourceEstimationService::sameReport(const SourceEstimate& a, const SourceEstimate& b) {
    if (a.valid != b.valid) return false;
    if (!a.valid) return true;
    if (std::fabs(a.latitude - b.latitude) > kReportLocationDeg) return false;
    if (std::fabs(a.longitude - b.longitude) > kReportLocationDeg) return false;
    if (std::fabs(a.depthKm - b.depthKm) > kReportDepthKm) return false;
    if (std::fabs(a.magnitude - b.magnitude) > kReportMagnitude) return false;
    if (a.grade != b.grade) return false;
    return true;
}

QString SourceEstimationService::regionNameFor(double latitude, double longitude) {
    const std::string name = RegionNameTable::instance().nameAt(latitude, longitude);
    if (!name.empty()) return QString::fromStdString(name);
    // 表未加载（资产缺失）或该格无名称：退回坐标文本，卡片至少还能指出位置。
    return QStringLiteral("%1°N %2°E")
        .arg(QString::number(latitude, 'f', 2), QString::number(longitude, 'f', 2));
}

bool SourceEstimationService::syncConfig() {
    if (!settings_) return false;
    const bool enabled = settings_->sourceEstimationEnabled();
    const int hold = std::max(0, std::min(1800, settings_->sourceEstimationHoldSeconds()));
    bool changed = false;
    bool enabledChanged = false;
    if (enabled != enabled_) {
        enabled_ = enabled;
        changed = true;
        enabledChanged = true;
    }
    if (hold != holdSeconds_) {
        holdSeconds_ = hold;
        changed = true;
    }
    // 逐区读参数：每区独立，任一区变了就作废**该区**的旧解，其余区域继续跑。
    for (RegionState& region : regions_) {
        const QString id = QString::fromLatin1(sourceRegionId(region.region));
        const double trigger = std::max(0.5, std::min(12.0, settings_->sourceEstimationTriggerFor(id)));
        const int minStations =
            std::max(1, std::min(40, settings_->sourceEstimationMinStationsFor(id)));
        const bool quiet = settings_->sourceEstimationQuietEvidenceFor(id);
        const int cluster = std::max(50, std::min(3000, settings_->sourceEstimationClusterRadiusFor(id)));
        // 逐区开关：关掉的区整区停摆（不记账、不算、不发布）。**生效**开关还要叠加
        // 可用性：中国 / 韩国 / 台湾 默认不开放（只有日本区的算法在真实台网上标定过），
        // 开发者模式才放出来 —— 与设置页隐藏那三张卡是同一份判据。
        const bool regionEnabled = settings_->isSourceEstimationRegionEnabled(id)
                                   && settings_->isSourceEstimationRegionAvailable(id);
        const bool regionToggled = regionEnabled != region.enabled;
        region.enabled = regionEnabled;
        if (!regionToggled && trigger == region.trigger && minStations == region.minStations
            && quiet == region.quietEvidence && cluster == region.clusterRadiusKm) {
            continue;
        }
        region.trigger = trigger;
        region.minStations = minStations;
        region.quietEvidence = quiet;
        region.clusterRadiusKm = cluster;
        region.states.clear();
        region.triggeredCount = 0;
        // 关掉的区立刻清空（不能留一个关不掉的旧震中）；重新打开时从「收集中」起步。
        clearRegion(region, (!enabled_ || !regionEnabled)
                                ? QStringLiteral("disabled")
                                : QStringLiteral("collecting"));
        changed = true;
    }
    if (enabledChanged && !enabled_) {
        for (RegionState& region : regions_) clearRegion(region, QStringLiteral("disabled"));
    }
    return changed;
}

void SourceEstimationService::reloadSettings() {
    // 只有**推算相关**的设置真的变了才作废旧解：SettingsStore::changed 是全局信号，
    // 改主题或地图标记也会走到这里，那些变化不该把地图上的推定震中抹掉重算。
    const bool configChanged = syncConfig();
    if (!configChanged) return;
    if (!enabled_) {
        publish();
        return;
    }
    // 阈值/门槛变了，旧结果立刻作废：它是在另一套判据下算出来的。
    runEstimation();
    publish();
}

void SourceEstimationService::reset() {
    for (RegionState& region : regions_) {
        region.states.clear();
        clearRegion(region, enabled_ ? QStringLiteral("idle") : QStringLiteral("disabled"));
    }
    latestEntries_.reset();
    latestEntriesMs_ = 0;
    lastRunMs_ = 0;
    publish();
}

IntensityFamily SourceEstimationService::familyOf(const StationSnapshot::Entry& entry) {
    if (entry.kma) return IntensityFamily::Kma;
    if (entry.csis) return IntensityFamily::Csis;
    return IntensityFamily::Jma;
}

qint64 SourceEstimationService::stationKey(IntensityFamily family, double latitude,
                                           double longitude) {
    // 纬度 [-90,90] → [-90000,90000] 需 18 位；经度 [-180,180] → [-180000,180000] 需 19 位；
    // 加 200000 偏移后均可放进无符号区间，再用 20+20 位拼接，制式占最低 2 位。
    const qint64 lat = static_cast<qint64>(std::llround(latitude * 1000.0)) + 90000;
    const qint64 lon = static_cast<qint64>(std::llround(longitude * 1000.0)) + 180000;
    const qint64 familyBits = family == IntensityFamily::Jma ? 1
                            : family == IntensityFamily::Kma ? 2 : 0;
    return (lat << 22) | (lon << 2) | familyBits;
}

void SourceEstimationService::ingestObservations(std::vector<StationSnapshot::Entry> entries) {
    ingestObservations(
        std::make_shared<const std::vector<StationSnapshot::Entry>>(std::move(entries)));
}

void SourceEstimationService::ingestObservations(
    const StationSnapshot::EntryList& entries) {
    // 零拷贝接管：只持 shared_ptr，不复制条目表。
    latestEntries_ = entries ? entries : std::make_shared<const std::vector<StationSnapshot::Entry>>();
    latestEntriesMs_ = nowProvider_();
    if (!enabled_) return;
    const long long now = latestEntriesMs_;

    // 先清零各区触发计数，再逐站归区、按**该区自己的**阈值簿记首次触发时刻。
    for (RegionState& region : regions_) region.triggeredCount = 0;

    // 阈值按 (区, 测站制式) 预换算一次：`intensityFromCsis(csisEquivalent(...))` 里是
    // 分段映射，旧实现把它放在逐站循环里 —— 2500 站 × 10 Hz 就是每秒 2.5 万次换算，
    // 而结果只取决于 4 个区 × 3 种制式。
    std::array<std::array<double, 3>, 4> thresholds{};
    for (std::size_t r = 0; r < regions_.size(); ++r) {
        const RegionState& region = regions_[r];
        for (int f = 0; f < 3; ++f) {
            const auto family = static_cast<IntensityFamily>(f);
            thresholds[r][static_cast<std::size_t>(f)] =
                intensityFromCsis(family, csisEquivalent(region.family, region.trigger));
        }
    }

    for (const StationSnapshot::Entry& entry : *latestEntries_) {
        if (!std::isfinite(entry.raw)) continue;
        const IntensityFamily family = familyOf(entry);
        if (!SourceEstimator::usableIntensity(family, entry.raw)) continue;
        SourceRegion stationRegion = SourceRegion::China;
        if (!sourceRegionForStation(family, entry.lat, entry.lon, &stationRegion)) continue;
        RegionState& region = regionState(stationRegion);
        // 该区没启用：整区跳过，既不记账也不参与任何计算。
        if (!region.enabled) continue;
        // 区域主导制式与测站制式不同时（例如台湾将来接入 CWA 之外的源）用预换算的阈值，
        // 保证「触发」在同一个物理量级上。
        const double threshold =
            thresholds[regionIndex(stationRegion)][static_cast<std::size_t>(family)];
        const bool isTriggered = entry.raw >= threshold;
        const qint64 key = stationKey(family, entry.lat, entry.lon);
        auto it = region.states.find(key);
        if (isTriggered) {
            ++region.triggeredCount;
            if (it == region.states.end()) {
                StationState state;
                state.firstTriggerMs = now;
                state.lastTriggerMs = now;
                state.lastSeenMs = now;
                state.lastIntensity = entry.raw;
                state.latitude = entry.lat;
                state.longitude = entry.lon;
                region.states.insert(key, state);
            } else {
                // 已经回落又再次越过阈值：距上次触发超过 kRetriggerGapMs 才算新的一次触发，
                // 否则「首次触发于 N 秒前」会被上一场地震永久固定住。
                if (it->firstTriggerMs <= 0
                    || (it->lastTriggerMs > 0 && now - it->lastTriggerMs > kRetriggerGapMs)) {
                    it->firstTriggerMs = now;
                }
                it->lastTriggerMs = now;
                it->lastSeenMs = now;
                it->lastIntensity = entry.raw;
                it->latitude = entry.lat;
                it->longitude = entry.lon;
            }
            continue;
        }
        if (it != region.states.end()) {
            it->lastSeenMs = now;
            it->lastIntensity = entry.raw;
            it->latitude = entry.lat;
            it->longitude = entry.lon;
        }
    }
    // 证据在流动：记录各区最近一次「有足够触发站」的时刻，用于保持窗口。
    for (RegionState& region : regions_) {
        if (!region.enabled) continue;
        if (region.triggeredCount >= std::max(1, region.minStations)) region.lastActiveMs = now;
        // 清扫长期未见的站，避免哈希无限增长（全球测站网约 2500 站，1 小时累计也有限）。
        for (auto it = region.states.begin(); it != region.states.end();) {
            if (now - it->lastSeenMs > kStateTtlMs) it = region.states.erase(it);
            else ++it;
        }
    }
}

void SourceEstimationService::tick() {
    if (!enabled_) return;
    const long long now = nowProvider_();
    bool expired = false;
    for (RegionState& region : regions_) {
        if (!region.estimate.valid) continue;
        // 保持窗口到期：证据早就散了、也没人再越阈值 → 清空该区。
        // 同时把「收集中/无解」的区也标成 expired：汇总状态靠它区分「刚被清空」与
        // 「本来就没在算」，否则过期后会被重新报成 collecting。
        if (now - std::max(region.lastActiveMs, region.updatedMs)
            > static_cast<long long>(holdSeconds_) * 1000LL) {
            clearRegion(region, QStringLiteral("expired"));
            expired = true;
        }
    }
    if (expired) {
        // 证据早就散了：先让旧解消失，本轮不再起算（与旧行为一致）。
        // 否则刚清空就会被下一次 runEstimation 重新填成「收集中」，过期状态一闪而过。
        publish();
        return;
    }
    // 每秒刷新卡片里的计时字段（触发站数、状态），不重新估算。
    runEstimation();
    publish();
}

void SourceEstimationService::runEstimation() {
    if (!enabled_) return;
    if (watcher_.isRunning()) {
        pending_ = true;
        return;
    }
    const long long now = nowProvider_();
    if (now - lastRunMs_ < kMinRunIntervalMs) return;
    if (!latestEntries_ || latestEntries_->empty()) return;
    // 全部区域都没到发布门槛时不必起线程：直接维护「收集中」状态。
    // 注意：这里**不**改 region.state —— 过期后的区应当保持 expired，直到又有站越阈值。
    bool anyReady = false;
    for (const RegionState& region : regions_) {
        if (!region.enabled) continue;
        if (region.triggeredCount >= std::max(1, region.minStations)) anyReady = true;
    }
    if (!anyReady) {
        // 没有区到门槛：仍然刷一次载荷（触发计数、区域启用态都在变），
        // 否则「全部区域关闭」时信息面板会一直停在上一次的旧内容上。
        publish();
        return;
    }

    // 按「区域 × 制式」分桶：区域的参数各自带着，制式的数值不可混算。
    std::array<std::vector<StationObservation>, 4> buckets;
    std::array<SourceEstimatorConfig, 4> configs;
    for (std::size_t i = 0; i < regions_.size(); ++i) {
        const RegionState& region = regions_[i];
        SourceEstimatorConfig config;
        config.family = region.family;
        config.region = region.region;
        config.triggerLevel = region.trigger;
        config.minStations = std::max(1, region.minStations);
        config.useQuietStations = region.quietEvidence;
        config.clusterRadiusKm = static_cast<double>(region.clusterRadiusKm);
        configs[i] = config;
    }
    // 日本区额外收集**到时观测**：首次越阈时刻当作 P 波到时，交给到时反演。
    // 其余区域只有强度信息，继续走强度场路径。
    std::vector<ArrivalObservation> arrivals;
    // 同一帧里**没越过阈值**的日本区站：着未着证据，只用来否决错误候选。
    std::vector<std::pair<double, double>> quietArrivals;
    const RegionState& japan = regionState(SourceRegion::Japan);
    for (const StationSnapshot::Entry& entry : *latestEntries_) {
        const IntensityFamily family = familyOf(entry);
        if (!SourceEstimator::usableIntensity(family, entry.raw)) continue;
        SourceRegion stationRegion = SourceRegion::China;
        if (!sourceRegionForStation(family, entry.lat, entry.lon, &stationRegion)) continue;
        const std::size_t index = regionIndex(stationRegion);
        // 该区没启用：这条观测完全不参与推算。
        if (!regions_[index].enabled) continue;
        // 观测制式与该区主导制式不同：丢掉这条观测，而不是混进制式不同的桶里。
        // （现实里不会发生：区域的制式由台网决定，见 sourceRegionForStation。）
        if (family != configs[index].family) continue;
        StationObservation observation;
        observation.latitude = entry.lat;
        observation.longitude = entry.lon;
        observation.intensity = entry.raw;
        observation.family = family;
        buckets[index].push_back(observation);

        if (stationRegion != SourceRegion::Japan || !japan.enabled) continue;
        const auto state = japan.states.constFind(stationKey(family, entry.lat, entry.lon));
        if (state == japan.states.constEnd() || state->firstTriggerMs <= 0) {
            // 没有触发（或从未记录到越阈）→ 未触发站，进着未着列表。
            quietArrivals.emplace_back(entry.lat, entry.lon);
            continue;
        }
        ArrivalObservation arrival;
        arrival.latitude = entry.lat;
        arrival.longitude = entry.lon;
        arrival.onsetEpochMs = state->firstTriggerMs;
        arrival.intensity = entry.raw;
        arrival.maxIntensity = std::max(state->lastIntensity, entry.raw);
        arrivals.push_back(arrival);
    }

    ArrivalEstimatorConfig arrivalConfig;
    arrivalConfig.region = SourceRegion::Japan;
    // 反演下限是参考实现的 5 站；发布门槛跟随用户在设置里给的该区门槛
    // （默认也是 5，见 `sourceRegionDefaults(kJapan)`）。
    arrivalConfig.minStations = std::min(5, std::max(1, japan.minStations));
    arrivalConfig.publishMinStations = std::max(1, japan.minStations);
    arrivalConfig.clusterRadiusKm = static_cast<double>(japan.clusterRadiusKm);

    lastRunMs_ = now;
    watcher_.setFuture(QtConcurrent::run([buckets, configs, arrivals, quietArrivals,
                                          arrivalConfig, now]() {
        EstimateBatch batch;
        for (std::size_t i = 0; i < configs.size(); ++i) {
            batch.estimates[i] = SourceEstimator::estimate(buckets[i], configs[i]);
        }
        // 日本区：到时反演优先。它给得出**发震时刻**，而且比强度场更能定住深度；
        // 只有到时解不成立（站太少/走时表缺失）时才回落到强度解。
        if (arrivals.size() >= static_cast<std::size_t>(std::max(1, arrivalConfig.minStations))) {
            const ServiceTravelTimes travelTimes;
            batch.arrival = ArrivalEstimator::estimate(arrivals, arrivalConfig, travelTimes,
                                                       quietArrivals, now);
        }
        return batch;
    }));
}

void SourceEstimationService::onEstimated() {
    if (!enabled_) return;
    applyBatch(watcher_.result());
    if (pending_) {
        pending_ = false;
        runEstimation();
    }
}

void SourceEstimationService::applyBatch(const EstimateBatch& batch) {
    const long long now = nowProvider_();
    for (std::size_t i = 0; i < regions_.size(); ++i) {
        RegionState& region = regions_[i];
        if (!region.enabled) continue;   // 关掉的区不发布任何解
        SourceEstimate estimate = batch.estimates[i];
        // 日本区：到时解优先。它带发震时刻与 P/S 相位数，且深度是可辨识的
        // （强度场里「深一点的近震」和「浅一点的远震」几乎同解）。
        // 只在到时解成立且不弱于强度解时替换，避免早期只有一两个到时站反而更差。
        if (region.region == SourceRegion::Japan && batch.arrival.valid) {
            SourceEstimate arrivalEstimate = ArrivalEstimator::toSourceEstimate(batch.arrival);
            if (!estimate.valid || arrivalEstimate.usedStationCount >= 3) {
                estimate = arrivalEstimate;
            }
        }
        if (!estimate.valid) {
            // 有足够的触发站但解不出来（例如强度一片相同、没有梯度）：保留上一次的有效
            // 结果直到保持窗口到期，避免地图上的推定震中闪断。
            if (region.estimate.valid
                && now - std::max(region.lastActiveMs, region.updatedMs)
                       <= static_cast<long long>(holdSeconds_) * 1000LL) {
                continue;
            }
            if (region.triggeredCount < std::max(1, region.minStations)) {
                // 没有新证据：保持 expired 等既有状态，只在确实有触发站时转成 collecting。
                if (!region.estimate.valid && region.triggeredCount > 0) {
                    region.state = QStringLiteral("collecting");
                }
                continue;
            }
            clearRegion(region, QStringLiteral("insufficient_gradient"));
            continue;
        }
        const SourceEstimate& best = estimate;
        const bool fresh = !region.estimate.valid
                           || std::fabs(best.latitude - region.estimate.latitude) > 1e-6
                           || std::fabs(best.longitude - region.estimate.longitude) > 1e-6;
        if (fresh && region.sessionStartedMs <= 0) region.sessionStartedMs = now;
        // 结果位置大幅跳变（> 60 km）时视为新的一次事件，重置「首次给出」计时与报次。
        if (region.estimate.valid
            && QuakeCalculator::haversineDistance(region.estimate.latitude,
                                                  region.estimate.longitude, best.latitude,
                                                  best.longitude) > 60.0) {
            region.sessionStartedMs = now;
            region.reportNum = 0;
            region.reportBase = SourceEstimate{};
            region.originEpochMs = 0;
        }
        // 报次：解变了才加一（首报为 1）。判据见 sameReport()：落在显示精度以内的
        // 抖动（1 Hz 重算的常态）不算新报，否则「第 N 报」会每秒跳一次。
        if (region.reportNum <= 0 || !sameReport(region.reportBase, best)) {
            region.reportNum = std::max(1, region.reportNum + 1);
            region.reportBase = best;
        }
        region.estimate = best;
        region.updatedMs = now;
        region.state = QStringLiteral("ready");
        region.originEpochMs = estimateOriginMs(region, best);
        if (!fresh) region.sessionStartedMs = region.sessionStartedMs > 0 ? region.sessionStartedMs : now;
    }
    publish();
}

void SourceEstimationService::clearRegion(RegionState& region, const QString& state) {
    region.estimate = SourceEstimate{};
    region.estimate.region = region.region;
    region.estimate.family = region.family;
    region.state = state;
    region.sessionStartedMs = 0;
    region.updatedMs = 0;
    // 报次与发震时刻跟着解一起清：下一场地震必须从「第 1 报」重新开始，
    // 不能让上一场地震的报数或发震时刻漏到新的解上。
    region.reportNum = 0;
    region.reportBase = SourceEstimate{};
    region.originEpochMs = 0;
}

void SourceEstimationService::publish() {
    const long long now = nowProvider_();
    QVariantMap next;
    QVariantList regionList;
    QVariantList markers;
    bool anyVisible = false;
    bool anyTriggered = false;
    int totalTriggered = 0;
    const RegionState* primary = nullptr;

    for (const RegionState& region : regions_) {
        const QString id = QString::fromLatin1(sourceRegionId(region.region));
        const QString label = QString::fromUtf8(sourceRegionLabel(region.region));
        const QString familyId = QString::fromLatin1(intensityFamilyId(region.family));
        QVariantMap row;
        row["id"] = id;
        row["label"] = label;
        row["family"] = familyId;
        row["familyLabel"] = QString::fromUtf8(intensityFamilyLabel(region.family));
        // 逐区开关：关闭的区卡片不显示（`enabled=false` 且 `state=disabled`）。
        row["enabled"] = enabled_ && region.enabled;
        row["regionEnabled"] = region.enabled;
        row["triggeredStations"] = region.triggeredCount;
        row["requiredStations"] = std::max(1, region.minStations);
        row["trigger"] = region.trigger;
        row["quietEvidence"] = region.quietEvidence;
        row["clusterRadiusKm"] = region.clusterRadiusKm;
        row["state"] = region.state;
        row["visible"] = region.estimate.valid;
        totalTriggered += region.triggeredCount;
        if (region.triggeredCount > 0) anyTriggered = true;

        if (region.estimate.valid) {
            const SourceEstimate& estimate = region.estimate;
            anyVisible = true;
            row["latitude"] = estimate.latitude;
            row["longitude"] = estimate.longitude;
            row["depthKm"] = estimate.depthKm;
            row["magnitude"] = estimate.magnitude;
            row["magnitudeText"] = QString::number(estimate.magnitude, 'f', 1);
            row["stationCount"] = estimate.stationCount;
            row["usedStations"] = estimate.usedStationCount;
            row["quietStations"] = estimate.quietCount;
            row["activeStations"] = estimate.stationCount;
            row["rms"] = estimate.rmsIntensity;
            row["azimuthGap"] = estimate.azimuthGapDeg;
            row["uncertaintyKm"] = estimate.uncertaintyKm;
            row["confidence"] = estimate.confidence;
            row["confidencePercent"] = static_cast<int>(std::lround(estimate.confidence * 100.0));
            row["provisional"] = estimate.provisional;
            row["grade"] = QString::fromStdString(estimate.grade);
            row["boundaryHit"] = estimate.boundaryHit;
            row["clusterCount"] = estimate.clusterCount;
            row["otherClusters"] = estimate.otherClusterCount;
            row["nearestStationKm"] = estimate.nearestStationKm;
            row["method"] = QString::fromStdString(estimate.method);
            // 卡片按「普通地震 HUD」展示所需的三个量：
            //   · location    —— 震中地名（参考实现的 `getFEName` 1° 网格，见 RegionNameTable）；
            //   · reportNum   —— 第几报（解变了才加一，见 RegionState::reportNum）；
            //   · originText  —— 发震时刻全文（UTC+8）。
            // 时刻优先用到时反演给的，其余区在 applyBatch 里用「首触发 − P 走时」反推。
            row["location"] = regionNameFor(estimate.latitude, estimate.longitude);
            row["reportNum"] = std::max(1, region.reportNum);
            row["originEpochMs"] = static_cast<qint64>(region.originEpochMs);
            row["originUtc8"] = originText(region.originEpochMs);
            row["originText"] = timestampText(region.originEpochMs);
            // 右下角「精度」= 质量等级 S/A/B/C/D（判据见 §17.2 第 6 条的站数 + 质量分双门槛）。
            row["precision"] = QString::fromStdString(estimate.grade);
            row["precisionText"] = QStringLiteral("精度 ") + QString::fromStdString(estimate.grade);
            // 到时路径（日本区）额外给 P/S 相位数与到时残差。强度路径这些字段留空。
            row["pPhases"] = estimate.pPhaseCount;
            row["sPhases"] = estimate.sPhaseCount;
            row["arrivalRmsSeconds"] = estimate.rmsIntensity;
            row["usesArrivalTimes"] = estimate.method.rfind("arrival_time", 0) == 0;
            row["unresponsiveStations"] = estimate.quietCount;
            row["ageSeconds"] = (now - region.updatedMs) / 1000;
            row["elapsedSeconds"] =
                region.sessionStartedMs > 0 ? (now - region.sessionStartedMs) / 1000 : 0;
            row["latitudeText"] = QString::number(estimate.latitude, 'f', 3);
            row["longitudeText"] = QString::number(estimate.longitude, 'f', 3);
            row["depthText"] = QString::number(estimate.depthKm, 'f', 0);
            row["epicentralIntensity"] = SourceEstimator::predictedIntensity(
                estimate.family, estimate.magnitude, 0.0, estimate.depthKm);
            row["epicentralText"] =
                intensityText(estimate.family, row["epicentralIntensity"].toDouble());

            QVariantMap mark;
            mark["id"] = id;
            mark["label"] = label;
            mark["visible"] = true;
            mark["latitude"] = estimate.latitude;
            mark["longitude"] = estimate.longitude;
            mark["uncertaintyKm"] = estimate.uncertaintyKm;
            mark["family"] = familyId;
            mark["grade"] = QString::fromStdString(estimate.grade);
            mark["magnitude"] = estimate.magnitude;
            mark["depthKm"] = estimate.depthKm;
            mark["confidence"] = estimate.confidence;
            mark["provisional"] = estimate.provisional;
            mark["clusterCount"] = estimate.clusterCount;
            mark["otherClusters"] = estimate.otherClusterCount;
            mark["epicentralIntensity"] = row["epicentralIntensity"];
            markers.push_back(mark);

            if (!primary || estimate.confidence > primary->estimate.confidence) primary = &region;
        } else if (region.triggeredCount > 0) {
            row["ageSeconds"] = 0;
        }
        regionList.push_back(row);
    }

    // 顶层汇总：与旧版单结果字段同形，取「证据最强的一区」，供仍只读单值的地方使用。
    next["enabled"] = enabled_;
    // 全局总开关开着、但一个区域都没启用时，卡片应当提示「先选区域」而不是空白。
    {
        bool anyRegion = false;
        for (const RegionState& region : regions_) {
            if (region.enabled) anyRegion = true;
        }
        next["anyRegionEnabled"] = anyRegion;
    }
    next["regions"] = regionList;
    next["markers"] = markers;
    next["visible"] = anyVisible;
    next["triggeredStations"] = totalTriggered;
    next["holdSeconds"] = holdSeconds_;
    next["anyTriggered"] = anyTriggered;
    if (primary && primary->estimate.valid) {
        const SourceEstimate& estimate = primary->estimate;
        const QString id = QString::fromLatin1(sourceRegionId(primary->region));
        next["state"] = QStringLiteral("ready");
        next["id"] = id;
        next["label"] = QString::fromUtf8(sourceRegionLabel(primary->region));
        next["family"] = QString::fromLatin1(intensityFamilyId(estimate.family));
        next["familyLabel"] = QString::fromUtf8(intensityFamilyLabel(estimate.family));
        next["latitude"] = estimate.latitude;
        next["longitude"] = estimate.longitude;
        next["depthKm"] = estimate.depthKm;
        next["magnitude"] = estimate.magnitude;
        next["magnitudeText"] = QString::number(estimate.magnitude, 'f', 1);
        next["stationCount"] = estimate.stationCount;
        next["usedStations"] = estimate.usedStationCount;
        next["quietStations"] = estimate.quietCount;
        next["activeStations"] = estimate.stationCount;
        next["requiredStations"] = std::max(1, primary->minStations);
        next["rms"] = estimate.rmsIntensity;
        next["azimuthGap"] = estimate.azimuthGapDeg;
        next["uncertaintyKm"] = estimate.uncertaintyKm;
        next["confidence"] = estimate.confidence;
        next["confidencePercent"] = static_cast<int>(std::lround(estimate.confidence * 100.0));
        next["provisional"] = estimate.provisional;
        next["grade"] = QString::fromStdString(estimate.grade);
        next["boundaryHit"] = estimate.boundaryHit;
        next["clusterCount"] = estimate.clusterCount;
        next["otherClusters"] = estimate.otherClusterCount;
        next["nearestStationKm"] = estimate.nearestStationKm;
        next["method"] = QString::fromStdString(estimate.method);
        next["location"] = regionNameFor(estimate.latitude, estimate.longitude);
        next["reportNum"] = std::max(1, primary->reportNum);
        next["originEpochMs"] = static_cast<qint64>(primary->originEpochMs);
        next["originUtc8"] = originText(primary->originEpochMs);
        next["originText"] = timestampText(primary->originEpochMs);
        next["precision"] = QString::fromStdString(estimate.grade);
        next["precisionText"] = QStringLiteral("精度 ") + QString::fromStdString(estimate.grade);
        next["pPhases"] = estimate.pPhaseCount;
        next["sPhases"] = estimate.sPhaseCount;
        next["arrivalRmsSeconds"] = estimate.rmsIntensity;
        next["usesArrivalTimes"] = estimate.method.rfind("arrival_time", 0) == 0;
        next["unresponsiveStations"] = estimate.quietCount;
        next["trigger"] = primary->trigger;
        next["quietEvidence"] = primary->quietEvidence;
        next["clusterRadiusKm"] = primary->clusterRadiusKm;
        next["ageSeconds"] = (now - primary->updatedMs) / 1000;
        next["elapsedSeconds"] =
            primary->sessionStartedMs > 0 ? (now - primary->sessionStartedMs) / 1000 : 0;
        next["latitudeText"] = QString::number(estimate.latitude, 'f', 3);
        next["longitudeText"] = QString::number(estimate.longitude, 'f', 3);
        next["depthText"] = QString::number(estimate.depthKm, 'f', 0);
        next["epicentralIntensity"] = SourceEstimator::predictedIntensity(
            estimate.family, estimate.magnitude, 0.0, estimate.depthKm);
        next["epicentralText"] =
            intensityText(estimate.family, next["epicentralIntensity"].toDouble());
    } else {
        // 汇总状态要反映「各区各自处于哪一步」，而不是只看有没有触发站：
        // 证据刚散掉时各区是 expired，不应当被重新报成 collecting。
        // 汇总状态：全局关 → disabled；全局开但一个区都没启用 → disabled
        // （「打开了但没选台网」和「关掉了」对用户是同一件事：什么都不算）。
        bool anyRegion = false;
        for (const RegionState& region : regions_) {
            if (region.enabled) anyRegion = true;
        }
        QString state = (enabled_ && anyRegion) ? QStringLiteral("idle")
                                                : QStringLiteral("disabled");
        if (enabled_ && anyRegion) {
            bool collecting = false;
            bool expired = false;
            for (const RegionState& region : regions_) {
                if (!region.enabled) continue;
                if (region.state == QStringLiteral("collecting")) collecting = true;
                if (region.state == QStringLiteral("expired")) expired = true;
            }
            if (collecting) state = QStringLiteral("collecting");
            else if (expired) state = QStringLiteral("expired");
            else if (!anyTriggered) state = QStringLiteral("idle");
        }
        next["state"] = state;
        next["trigger"] = regions_[0].trigger;
        next["quietEvidence"] = regions_[0].quietEvidence;
        next["clusterRadiusKm"] = regions_[0].clusterRadiusKm;
    }

    QVariantMap nextMarker;
    if (primary && primary->estimate.valid && !markers.isEmpty()) {
        // 顶层标记 = 最强的一区（兼容只读单标记的调用方）。完整列表见 markers。
        for (const QVariant& entry : markers) {
            const QVariantMap candidate = entry.toMap();
            if (candidate.value("id").toString() == next.value("id").toString()) {
                nextMarker = candidate;
                break;
            }
        }
    }
    if (nextMarker.isEmpty()) nextMarker["visible"] = false;
    nextMarker["markers"] = markers;

    if (next != payload_ || nextMarker != marker_) {
        payload_ = next;
        marker_ = nextMarker;
        emit changed();
    }
}

QVariantMap SourceEstimationService::info() const { return payload_; }

QVariantMap SourceEstimationService::marker() const { return marker_; }

} // namespace komira
