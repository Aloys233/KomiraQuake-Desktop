#pragma once

#include <QHash>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <QVector>

#include <string>
#include <vector>

#include "model/earthquake_event.h"

namespace komira {

/// `EarthquakeEvent::sourceTags` ↔ events 表的 `source_tags` 列。
///
/// 用换行分隔而不是逗号：标签本身是 `<provider>·<agency>`（如 `Wolfx·CENC`），机构段
/// 可能是中文机构名（`北京地震局`），若将来出现带标点的上游名，逗号/分号都会歧义；
/// 换行不可能出现在单个标签里，解析退化为一次 split。
inline QString sourceTagsToText(const std::vector<std::string>& tags) {
    QStringList parts;
    parts.reserve(static_cast<qsizetype>(tags.size()));
    for (const std::string& tag : tags) {
        const QString text = QString::fromStdString(tag).trimmed();
        if (!text.isEmpty()) parts.push_back(text);
    }
    return parts.join(QLatin1Char('\n'));
}

inline std::vector<std::string> sourceTagsFromText(const QString& text) {
    std::vector<std::string> tags;
    if (text.isEmpty()) return tags;
    const QStringList parts = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    tags.reserve(static_cast<std::size_t>(parts.size()));
    for (const QString& part : parts) {
        const QString trimmed = part.trimmed();
        if (!trimmed.isEmpty()) tags.push_back(trimmed.toStdString());
    }
    return tags;
}

/// 一条报次记录（某个源收到的某一报）。详情页的「数据源 / 报数」两段都靠它。
/// 与 events 表分开存：同一场地震会收到多报、每报又可能经多个聚合源送达，
/// 全部塞进 events 行会反复覆盖，只留得下最后一报。
struct StoredReport {
    QString quakeKey;
    /// 组内的报次槽：实时预警为 `n:<报数>`，情报为 `i:<发布时刻>`。同一场地震的
    /// 预警与情报是两种报文，报数都可能从 1 开始，不能只按 report_num 区分。
    QString reportKey;
    int reportNum = 1;
    QString sourceId;
    QString sourceTag;
    long long receivedAt = 0;
    QString rawJson;
    long long timestamp = 0;
    long long reportTime = 0;
    double magnitude = 0.0;
    double depth = 0.0;
    double latitude = 0.0;
    double longitude = 0.0;
    QString location;
    QString maxIntensityText;
    bool isFinal = false;
    bool isCanceled = false;
    /// 情报/速报行（相对实时预警的一报）：没有「第 N 报」语义，详情页标「情报」。
    bool isInfo = false;
    /// 测定状态：`reviewed`（正式测定）/ `automatic`（自动测定）/ 空。
    QString determination;
};

/// SQLite 历史存储。表结构与《NATIVE_PORT_SPEC》 §6 一致。
class HistoryStore {
public:
    HistoryStore() = default;
    ~HistoryStore();

    bool init(const QString& path);
    bool isReady() const { return ready_; }

    QVector<EarthquakeEvent> loadRecent(int limit = 200) const;
    void upsertAll(const QVector<EarthquakeEvent>& events);
    /// 逐条 upsert 报次记录（主键：quakeKey + reportKey + sourceId）。
    void upsertReports(const QVector<StoredReport>& reports);
    /// 读出某场地震的全部报次（按报数、接收时间升序）。详情页打开时按需查询。
    QVector<StoredReport> loadReports(const QString& quakeKey) const;
    void prune(int keep = 400);
    void clear();
    // Separate from visible history: clearing history must not revive alerts.
    void saveTombstone(const QString& key, long long endedAt);
    QHash<QString, long long> loadTombstones(long long now) const;

private:
    bool createTable();
    /// 把升级前的 event_reports 行搬到新主键（quake_key 重算、report_key = `n:<num>`）。
    bool migrateLegacyReports();
    /// 情报的报次槽从 `i:<发布时刻>` 改成 `i:<测定状态>`（跨链路口径对齐，见 reportSlotKey）。
    bool migrateInfoReportSlots();
    bool writeReportsBlocking(const QVector<StoredReport>& reports);

    QSqlDatabase db_;
    bool ready_ = false;
    QString connectionName_;
};

} // namespace komira
