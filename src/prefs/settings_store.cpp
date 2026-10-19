#include "prefs/settings_store.h"

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

SettingsStore::SettingsStore(QObject* parent) : QObject(parent) { migrateLegacyGlobal(s_); }

void SettingsStore::resetToDefaults() {
    s_.clear();
    emit changed();
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
    if (touched) emit changed();
}

} // namespace komira
