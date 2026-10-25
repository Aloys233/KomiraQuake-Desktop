#include "prefs/settings_store.h"

#include <QList>
#include <QStringList>

#include <string>

#include "core/source_estimator.h"

namespace komira {

namespace {

/// 区域设置键：`sourceEstimation.<region>.<field>`。按区域拆开后各区互不影响，
/// 恢复默认也只需删掉该区的前缀。
QString regionKey(const char* regionId, const char* field) {
    return QStringLiteral("sourceEstimation.%1.%2")
        .arg(QString::fromLatin1(regionId), QString::fromLatin1(field));
}

bool resolveRegion(const QString& region, SourceRegion* out) {
    return sourceRegionFromId(region.toStdString(), out);
}

/// 旧版（区域拆分之前）的四个全局键。迁移规则：**只在中国区还没有任何键时**把旧值
/// 抄进中国区，然后删掉旧键 —— 否则「用户改过中国区、又升级一次」会被旧值覆盖。
struct LegacyGlobalKey {
    const char* legacy;
    const char* field;
};

constexpr LegacyGlobalKey kLegacyGlobals[] = {
    {"sourceEstimationTrigger", "trigger"},
    {"sourceEstimationMinStations", "minStations"},
    {"sourceEstimationQuietEvidence", "quietEvidence"},
    {"sourceEstimationClusterRadius", "clusterRadius"},
};

void migrateLegacyGlobal(QSettings& settings) {
    bool hasLegacy = false;
    for (const LegacyGlobalKey& entry : kLegacyGlobals) {
        if (settings.contains(QString::fromLatin1(entry.legacy))) {
            hasLegacy = true;
            break;
        }
    }
    if (!hasLegacy) return;
    const char* chinaId = sourceRegionId(SourceRegion::China);
    const bool chinaTouched = settings.contains(regionKey(chinaId, "trigger"))
                              || settings.contains(regionKey(chinaId, "minStations"));
    if (!chinaTouched) {
        for (const LegacyGlobalKey& entry : kLegacyGlobals) {
            const QString legacy = QString::fromLatin1(entry.legacy);
            if (!settings.contains(legacy)) continue;
            settings.setValue(regionKey(chinaId, entry.field), settings.value(legacy));
        }
    }
    for (const LegacyGlobalKey& entry : kLegacyGlobals) {
        settings.remove(QString::fromLatin1(entry.legacy));
    }
}

} // namespace

SettingsStore::SettingsStore(QObject* parent) : QObject(parent) {
    migrateLegacyGlobal(s_);
    // 200 ms：比一次拖动明显长，又短到用户切页/关程序前一定落盘。
    syncTimer_.setSingleShot(true);
    syncTimer_.setInterval(200);
    connect(&syncTimer_, &QTimer::timeout, this, &SettingsStore::flushSync);
}

void SettingsStore::flushSync() {
    syncTimer_.stop();
    s_.sync();
}

SettingsStore::~SettingsStore() {
    // 析构时兜底：把合并窗口内还没落盘的值写出去。
    if (syncTimer_.isActive()) flushSync();
}

void SettingsStore::resetToDefaults() {
    s_.clear();
    emit changed();
    // 恢复默认会一次性改掉**所有**设置项，逐项信号必须全部发出去：
    // QML 只监听具体某一项（不再依赖聚合的 changed()），漏发就会停在旧值上
    // —— 例如主题按钮仍显示「深色」而实际已回到「跟随系统」。
    emitAllChanged();
}

bool SettingsStore::hasSourceEstimationRegion(const QString& region) const {
    SourceRegion ignored;
    return resolveRegion(region, &ignored);
}

QString SettingsStore::sourceEstimationRegionLabel(const QString& region) const {
    SourceRegion resolved;
    if (!resolveRegion(region, &resolved)) return QString();
    return QString::fromUtf8(sourceRegionLabel(resolved));
}

bool SettingsStore::isSourceEstimationRegionAvailable(const QString& region) const {
    SourceRegion resolved;
    if (!resolveRegion(region, &resolved)) return false;
    // 只有日本区的算法在真实台网上标定过（§17.6 到时反演）；中国 / 韩国 / 台湾 的
    // 强度场路径仍是待标定状态，默认不开放，开发者模式才放出来。
    return resolved == SourceRegion::Japan || developerMode();
}

QVariantMap SettingsStore::sourceEstimationRegionParams() const {
    QVariantMap out;
    for (const SourceRegion region : sourceRegions()) {
        const QString id = QString::fromLatin1(sourceRegionId(region));
        QVariantMap row;
        row["label"] = sourceEstimationRegionLabel(id);
        row["trigger"] = sourceEstimationTriggerFor(id);
        row["minStations"] = sourceEstimationMinStationsFor(id);
        row["quietEvidence"] = sourceEstimationQuietEvidenceFor(id);
        row["clusterRadius"] = sourceEstimationClusterRadiusFor(id);
        // 生效开关：不可用区域（非开发者模式下的中国 / 韩国 / 台湾）恒为关，
        // 与签名、服务层口径一致。
        row["enabled"] = isSourceEstimationRegionEnabled(id)
                         && isSourceEstimationRegionAvailable(id);
        row["available"] = isSourceEstimationRegionAvailable(id);
        out[id] = row;
    }
    return out;
}

double SettingsStore::sourceEstimationTriggerFor(const QString& region) const {
    SourceRegion resolved;
    if (!resolveRegion(region, &resolved)) return sourceRegionDefaults(SourceRegion::China).trigger;
    const SourceRegionDefaults& defaults = sourceRegionDefaults(resolved);
    return s_.value(regionKey(sourceRegionId(resolved), "trigger"), defaults.trigger).toDouble();
}

void SettingsStore::setSourceEstimationTrigger(const QString& region, double v) {
    SourceRegion resolved;
    if (!resolveRegion(region, &resolved)) return;
    set(regionKey(sourceRegionId(resolved), "trigger"), v);
}

int SettingsStore::sourceEstimationMinStationsFor(const QString& region) const {
    SourceRegion resolved;
    if (!resolveRegion(region, &resolved)) {
        return sourceRegionDefaults(SourceRegion::China).minStations;
    }
    const SourceRegionDefaults& defaults = sourceRegionDefaults(resolved);
    return s_.value(regionKey(sourceRegionId(resolved), "minStations"),
                    defaults.minStations).toInt();
}

void SettingsStore::setSourceEstimationMinStations(const QString& region, int v) {
    SourceRegion resolved;
    if (!resolveRegion(region, &resolved)) return;
    set(regionKey(sourceRegionId(resolved), "minStations"), v);
}

bool SettingsStore::sourceEstimationQuietEvidenceFor(const QString& region) const {
    SourceRegion resolved;
    if (!resolveRegion(region, &resolved)) {
        return sourceRegionDefaults(SourceRegion::China).quietEvidence;
    }
    const SourceRegionDefaults& defaults = sourceRegionDefaults(resolved);
    return s_.value(regionKey(sourceRegionId(resolved), "quietEvidence"),
                    defaults.quietEvidence).toBool();
}

void SettingsStore::setSourceEstimationQuietEvidence(const QString& region, bool v) {
    SourceRegion resolved;
    if (!resolveRegion(region, &resolved)) return;
    set(regionKey(sourceRegionId(resolved), "quietEvidence"), v);
}

int SettingsStore::sourceEstimationClusterRadiusFor(const QString& region) const {
    SourceRegion resolved;
    if (!resolveRegion(region, &resolved)) {
        return sourceRegionDefaults(SourceRegion::China).clusterRadiusKm;
    }
    const SourceRegionDefaults& defaults = sourceRegionDefaults(resolved);
    return s_.value(regionKey(sourceRegionId(resolved), "clusterRadius"),
                    defaults.clusterRadiusKm)
        .toInt();
}

void SettingsStore::setSourceEstimationClusterRadius(const QString& region, int v) {
    SourceRegion resolved;
    if (!resolveRegion(region, &resolved)) return;
    set(regionKey(sourceRegionId(resolved), "clusterRadius"), v);
}

bool SettingsStore::isSourceEstimationRegionEnabled(const QString& region) const {
    SourceRegion resolved;
    if (!resolveRegion(region, &resolved)) return false;
    // 未在启用集合里 = 关闭（默认关闭）。
    return enabledSourceEstimationRegions().contains(
        QString::fromLatin1(sourceRegionId(resolved)));
}

void SettingsStore::setSourceEstimationRegionEnabled(const QString& region, bool enabled) {
    SourceRegion resolved;
    if (!resolveRegion(region, &resolved)) return;
    const QString id = QString::fromLatin1(sourceRegionId(resolved));
    QStringList enabledList = enabledSourceEstimationRegions();
    if (enabledList.contains(id) == enabled) return;   // 已经是目标状态，不发 changed
    if (enabled) enabledList << id;
    else enabledList.removeAll(id);
    set("enabledSourceEstimationRegions", enabledList);
}

void SettingsStore::resetSourceEstimationRegion(const QString& region) {
    SourceRegion resolved;
    if (!resolveRegion(region, &resolved)) return;
    const char* id = sourceRegionId(resolved);
    bool touched = false;
    for (const char* field : {"trigger", "minStations", "quietEvidence", "clusterRadius"}) {
        const QString key = regionKey(id, field);
        if (!s_.contains(key)) continue;
        s_.remove(key);
        touched = true;
    }
    if (touched) {
        emit changed();
        emit sourceEstimationRegionParamsChanged();
        emit sourceEstimationRegionStateChanged();
    }
}

const QList<SettingsStore::KeySignal>& SettingsStore::keySignals() {
    static const QList<KeySignal> kEntries = {
        {"alertVolume", &SettingsStore::alertVolumeChanged},
        {"announceCountdown", &SettingsStore::announceCountdownChanged},
        {"announceEvent", &SettingsStore::announceEventChanged},
        {"announceUpdate", &SettingsStore::announceUpdateChanged},
        {"autoCheckUpdates", &SettingsStore::autoCheckUpdatesChanged},
        {"backgroundBlur", &SettingsStore::backgroundBlurChanged},
        {"basemapId", &SettingsStore::basemapIdChanged},
        {"closeToTray", &SettingsStore::closeToTrayChanged},
        {"customBasemapDatum", &SettingsStore::customBasemapDatumChanged},
        {"customBasemapUrl", &SettingsStore::customBasemapUrlChanged},
        {"customNtpServer", &SettingsStore::customNtpServerChanged},
        {"developerMode", &SettingsStore::developerModeChanged},
        {"disabledSources", &SettingsStore::disabledSourcesChanged},
        {"disabledStationSources", &SettingsStore::disabledStationSourcesChanged},
        {"disabledSubSources", &SettingsStore::disabledSubSourcesChanged},
        {"enableAlarmTone", &SettingsStore::enableAlarmToneChanged},
        {"enableNtpSync", &SettingsStore::enableNtpSyncChanged},
        {"enableSoundAlert", &SettingsStore::enableSoundAlertChanged},
        {"enableWarnings", &SettingsStore::enableWarningsChanged},
        {"enabledSourceEstimationRegions", &SettingsStore::enabledSourceEstimationRegionsChanged},
        {"intensityStandard", &SettingsStore::intensityStandardChanged},
        {"isMuted", &SettingsStore::isMutedChanged},
        {"jianRefreshToken", &SettingsStore::jianRefreshTokenChanged},
        {"localIntensityFilter", &SettingsStore::localIntensityFilterChanged},
        {"notifyAlert", &SettingsStore::notifyAlertChanged},
        {"notifyEew", &SettingsStore::notifyEewChanged},
        {"notifyInfo", &SettingsStore::notifyInfoChanged},
        {"raiseAlert", &SettingsStore::raiseAlertChanged},
        {"raiseEew", &SettingsStore::raiseEewChanged},
        {"raiseInfo", &SettingsStore::raiseInfoChanged},
        {"reduceMotion", &SettingsStore::reduceMotionChanged},
        {"showEstimatedEpicenter", &SettingsStore::showEstimatedEpicenterChanged},
        {"showEstimateLabel", &SettingsStore::showEstimateLabelChanged},
        {"showProvinceBorders", &SettingsStore::showProvinceBordersChanged},
        {"showStationLayer", &SettingsStore::showStationLayerChanged},
        {"stackStationCards", &SettingsStore::stackStationCardsChanged},
        {"showWavefront", &SettingsStore::showWavefrontChanged},
        {"waveFadeMode", &SettingsStore::waveFadeModeChanged},
        {"waveFadeIntensity", &SettingsStore::waveFadeIntensityChanged},
        {"waveFadeDistanceKm", &SettingsStore::waveFadeDistanceKmChanged},
        {"waveFadeSeconds", &SettingsStore::waveFadeSecondsChanged},
        {"silentStart", &SettingsStore::silentStartChanged},
        {"simulatedUrl", &SettingsStore::simulatedUrlChanged},
        {"sourceEstimationEnabled", &SettingsStore::sourceEstimationEnabledChanged},
        {"sourceEstimationHoldSeconds", &SettingsStore::sourceEstimationHoldSecondsChanged},
        {"themeMode", &SettingsStore::themeModeChanged},
        {"updateChannel", &SettingsStore::updateChannelChanged},
        {"updateCustomProxy", &SettingsStore::updateCustomProxyChanged},
        {"vectorOverlay", &SettingsStore::vectorOverlayChanged},
        {"whewsToken", &SettingsStore::whewsTokenChanged},
    };
    return kEntries;
}

bool SettingsStore::isRegionParamKey(const QString& key) {
    static const QStringList kRegionFields = {QStringLiteral(".trigger"),
                                              QStringLiteral(".minStations"),
                                              QStringLiteral(".quietEvidence"),
                                              QStringLiteral(".clusterRadius")};
    if (!key.startsWith(QStringLiteral("sourceEstimation."))) return false;
    for (const QString& field : kRegionFields) {
        if (key.endsWith(field)) return true;
    }
    return false;
}

void SettingsStore::emitKeyChanged(const QString& key) {
    // 区域参数是复合键（`sourceEstimation.<region>.<field>`），统一走一个信号。
    if (isRegionParamKey(key)) {
        emit sourceEstimationRegionParamsChanged();
        emit sourceEstimationRegionStateChanged();
        return;
    }
    // 逐源震级过滤也是复合键（`minMagnitude.<sourceId>`）：一个信号覆盖全部源。
    if (key.startsWith(QLatin1String("minMagnitude."))) {
        emit infoMagnitudeChanged();
        return;
    }
    const auto& entries = keySignals();
    for (const auto& entry : entries) {
        if (key == QLatin1String(entry.key)) {
            (this->*entry.signal)();
            // 区域状态快照同时覆盖区域参数、启用集合与可用性（开发者模式），
            // 三者的变化都要让 QML 的逐区绑定重算。
            if (key == QLatin1String("enabledSourceEstimationRegions")
                || key == QLatin1String("developerMode")) {
                emit sourceEstimationRegionStateChanged();
            }
            return;
        }
    }
    // 未登记的键：没有逐项信号，但 changed() 已经发出，C++ 侧的复核不受影响。
}

void SettingsStore::emitAllChanged() {
    for (const auto& entry : keySignals()) (this->*entry.signal)();
    emit sourceEstimationRegionParamsChanged();
    emit sourceEstimationRegionStateChanged();
}
} // namespace komira
