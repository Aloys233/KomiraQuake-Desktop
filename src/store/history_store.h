#pragma once

#include <QHash>
#include <QSqlDatabase>
#include <QString>
#include <QVector>

#include "model/earthquake_event.h"

namespace komira {

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
    bool writeReportsBlocking(const QVector<StoredReport>& reports);

    QSqlDatabase db_;
    bool ready_ = false;
    QString connectionName_;
};

} // namespace komira
