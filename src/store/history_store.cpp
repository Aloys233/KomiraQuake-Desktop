#include "store/history_store.h"

#include <QDebug>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QSet>
#include <QUuid>
#include <QVariant>

#include <utility>

#include "core/quake_key.h"

namespace komira {

HistoryStore::~HistoryStore() {
    if (db_.isOpen()) db_.close();
    if (!connectionName_.isEmpty()) {
        db_ = QSqlDatabase();
        QSqlDatabase::removeDatabase(connectionName_);
    }
}

bool HistoryStore::init(const QString& path) {
    ready_ = false;
    if (!connectionName_.isEmpty()) {
        db_.close();
        db_ = QSqlDatabase();
        QSqlDatabase::removeDatabase(connectionName_);
    }
    connectionName_ = QStringLiteral("komira_history_") + QUuid::createUuid().toString();
    db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName_);
    db_.setDatabaseName(path);
    if (!db_.open()) {
        ready_ = false;
        return false;
    }
    QSqlQuery pragma(db_);
    pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    pragma.finish();
    ready_ = createTable();
    return ready_;
}

bool HistoryStore::createTable() {
    if (!db_.transaction()) return false;
    QSqlQuery q(db_);
    if (!q.exec(QStringLiteral(R"SQL(
        CREATE TABLE IF NOT EXISTS events (
            id TEXT PRIMARY KEY,
            source TEXT NOT NULL,
            magnitude REAL NOT NULL,
            latitude REAL NOT NULL,
            longitude REAL NOT NULL,
            depth REAL NOT NULL,
            location TEXT NOT NULL,
            timestamp INTEGER NOT NULL,
            distance_km REAL NOT NULL,
            estimated_intensity TEXT NOT NULL,
            raw_intensity REAL NOT NULL,
            p_wave_arrival INTEGER,
            s_wave_arrival INTEGER,
            warning_level INTEGER NOT NULL,
            report_num INTEGER NOT NULL,
            is_final INTEGER NOT NULL,
            is_canceled INTEGER NOT NULL
        )
    )SQL"))) {
        db_.rollback();
        return false;
    }
    // Inspect columns first: genuine migration failures must not be mistaken for
    // an already-applied migration. ALTER preserves all existing history rows.
    if (!q.exec(QStringLiteral("PRAGMA table_info(events)"))) {
        db_.rollback();
        return false;
    }
    QSet<QString> columns;
    while (q.next()) columns.insert(q.value(1).toString());
    q.finish();
    const std::pair<QString, QString> additions[] = {
        {QStringLiteral("source_provider"), QStringLiteral("TEXT NOT NULL DEFAULT ''")},
        {QStringLiteral("source_agency"), QStringLiteral("TEXT NOT NULL DEFAULT ''")},
        {QStringLiteral("event_id"), QStringLiteral("TEXT NOT NULL DEFAULT ''")},
        {QStringLiteral("report_time"), QStringLiteral("INTEGER NOT NULL DEFAULT 0")},
        {QStringLiteral("max_intensity_raw"), QStringLiteral("REAL NOT NULL DEFAULT 0")},
        {QStringLiteral("max_intensity_text"), QStringLiteral("TEXT NOT NULL DEFAULT ''")},
        {QStringLiteral("source_id"), QStringLiteral("TEXT NOT NULL DEFAULT ''")},
        // 情报标记与测定状态：重启后详情页仍要区分「情报」与「第 N 报」。
        {QStringLiteral("is_info"), QStringLiteral("INTEGER NOT NULL DEFAULT 0")},
        {QStringLiteral("determination"), QStringLiteral("TEXT NOT NULL DEFAULT ''")},
        // 送达过这条上游报文的全部聚合源标签（换行分隔）：`history_` 按 id 归并时是
        // 并集，只留最后一轮的来源会让另一路在重启后彻底消失。
        {QStringLiteral("source_tags"), QStringLiteral("TEXT NOT NULL DEFAULT ''")}
    };
    for (const auto& column : additions) {
        if (!columns.contains(column.first)
            && !q.exec(QStringLiteral("ALTER TABLE events ADD COLUMN %1 %2")
                           .arg(column.first, column.second))) {
            db_.rollback();
            return false;
        }
    }
    if (!q.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS warning_tombstones ("
            "identity TEXT PRIMARY KEY, ended_at INTEGER NOT NULL)"))) {
        db_.rollback();
        return false;
    }
    // 报次归档：同一场地震的每一报、每一路聚合源各一行。quake_key 由控制器按
    // 「机构 + 发震时刻分钟桶 + 震中网格」生成，实时预警与情报因此能落进同一组。
    //
    // 主键用 report_key 而不是 report_num：同一场地震的预警与情报各自从第 1 报开始
    // 编号（情报甚至没有报数），只用 (quake_key, report_num) 会让「情报」覆盖掉「第 1 报」。
    // 旧库的主键是 (quake_key, report_num, source_id)，SQLite 不能原地改主键，只能重建。
    if (!q.exec(QStringLiteral("PRAGMA table_info(event_reports)"))) {
        db_.rollback();
        return false;
    }
    QSet<QString> reportColumns;
    while (q.next()) reportColumns.insert(q.value(1).toString());
    q.finish();
    const bool reportsTableExists = !reportColumns.isEmpty();
    const bool legacyReports = reportsTableExists
                               && !reportColumns.contains(QStringLiteral("report_key"));
    if (legacyReports) {
        // 升级前的归档：quake_key 是 `<机构>|<发震时刻毫秒>`，报次只按 report_num 分。
        // 新键要「分钟桶 + 震中网格」，两者都能从行内已有数据算出来，所以原地搬家，
        // 不丢用户的报次历史（见下方 C++ 迁移）。
        if (!q.exec(QStringLiteral("ALTER TABLE event_reports RENAME TO event_reports_legacy"))) {
            db_.rollback();
            return false;
        }
    }
    if (!q.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS event_reports ("
            "quake_key TEXT NOT NULL, report_key TEXT NOT NULL, report_num INTEGER NOT NULL, "
            "source_id TEXT NOT NULL, source_tag TEXT NOT NULL, received_at INTEGER NOT NULL, "
            "raw_json TEXT NOT NULL DEFAULT '', timestamp INTEGER NOT NULL, "
            "report_time INTEGER NOT NULL, magnitude REAL NOT NULL, depth REAL NOT NULL, "
            "latitude REAL NOT NULL, longitude REAL NOT NULL, location TEXT NOT NULL, "
            "max_intensity_text TEXT NOT NULL DEFAULT '', is_final INTEGER NOT NULL, "
            "is_canceled INTEGER NOT NULL, is_info INTEGER NOT NULL DEFAULT 0, "
            "determination TEXT NOT NULL DEFAULT '', "
            "PRIMARY KEY (quake_key, report_key, source_id))"))) {
        db_.rollback();
        return false;
    }
    if (legacyReports && !migrateLegacyReports()) {
        db_.rollback();
        return false;
    }
    if (!migrateInfoReportSlots()) {
        db_.rollback();
        return false;
    }
    q.finish();
    if (db_.commit()) return true;
    db_.rollback();
    return false;
}

/// 情报的报次槽从「发布时刻」改成「测定状态」（见 AppController::reportSlotKey）。
///
/// 旧行的槽是 `i:<毫秒>`，同一份情报经 Wolfx（有 ReportTime）与 Jian（没有）会落成两个
/// 槽，详情页把一报显示成两报。这里把旧槽按行内的 determination 重算，让它们与今后
/// 写入的槽对齐；同一槽位上多路聚合源本来就应该并存（主键含 source_id），`INSERT OR
/// REPLACE` 天然完成合并。幂等：只有 `i:` 后接纯数字的槽会被改。
bool HistoryStore::migrateInfoReportSlots() {
    QSqlQuery read(db_);
    if (!read.exec(QStringLiteral(
            "SELECT quake_key, report_key, report_num, source_id, source_tag, received_at, "
            "raw_json, timestamp, report_time, magnitude, depth, latitude, longitude, location, "
            "max_intensity_text, is_final, is_canceled, is_info, determination "
            "FROM event_reports WHERE report_key GLOB 'i:[0-9]*'"))) {
        return false;
    }
    QVector<StoredReport> rewritten;
    while (read.next()) {
        StoredReport r;
        r.quakeKey = read.value(0).toString();
        const QString oldKey = read.value(1).toString();
        r.reportNum = read.value(2).toInt();
        r.sourceId = read.value(3).toString();
        r.sourceTag = read.value(4).toString();
        r.receivedAt = read.value(5).toLongLong();
        r.rawJson = read.value(6).toString();
        r.timestamp = read.value(7).toLongLong();
        r.reportTime = read.value(8).toLongLong();
        r.magnitude = read.value(9).toDouble();
        r.depth = read.value(10).toDouble();
        r.latitude = read.value(11).toDouble();
        r.longitude = read.value(12).toDouble();
        r.location = read.value(13).toString();
        r.maxIntensityText = read.value(14).toString();
        r.isFinal = read.value(15).toInt() != 0;
        r.isCanceled = read.value(16).toInt() != 0;
        r.isInfo = read.value(17).toInt() != 0;
        r.determination = read.value(18).toString();
        // 与 AppController::reportSlotKey 逐字一致：有测定状态按它分槽，没有则退回
        // 发布时刻（`i:t<ms>`），这样不同波的两次定稿仍然各占一行。
        const long long stamp = r.reportTime > 0 ? r.reportTime : r.timestamp;
        r.reportKey = r.determination.isEmpty() ? QStringLiteral("i:t") + QString::number(stamp)
                                                : QStringLiteral("i:") + r.determination;
        if (r.reportKey == oldKey) continue;
        rewritten.push_back(r);
    }
    read.finish();
    if (rewritten.isEmpty()) return true;
    // 先写入新槽、再删旧行：中途失败时旧行还在，下次启动用同一条查询重试，不会丢报次。
    if (!writeReportsBlocking(rewritten)) return false;
    QSqlQuery remove(db_);
    remove.prepare(QStringLiteral(
        "DELETE FROM event_reports WHERE quake_key = ? AND source_id = ? "
        "AND report_key GLOB 'i:[0-9]*'"));
    for (const StoredReport& r : std::as_const(rewritten)) {
        remove.addBindValue(r.quakeKey);
        remove.addBindValue(r.sourceId);
        if (!remove.exec()) return false;
    }
    return true;
}

bool HistoryStore::migrateLegacyReports() {
    // 旧行没有报数机构：quake_key 形如 `<小写机构>|<发震时刻毫秒>`，机构段可直接取回。
    // identity 兜底键（`机构|eventId`）的后半段不是纯数字，靠这一点筛掉。
    QSqlQuery read(db_);
    if (!read.exec(QStringLiteral(
            "SELECT quake_key, report_num, source_id, source_tag, received_at, raw_json, "
            "timestamp, report_time, magnitude, depth, latitude, longitude, location, "
            "max_intensity_text, is_final, is_canceled FROM event_reports_legacy"))) {
        return false;
    }
    QVector<StoredReport> migrated;
    const QRegularExpression numericSuffix(QStringLiteral("^\\d+$"));
    while (read.next()) {
        const QString legacyKey = read.value(0).toString();
        const int split = legacyKey.lastIndexOf(QLatin1Char('|'));
        if (split <= 0) continue;
        const QString agency = legacyKey.left(split);
        const QString suffix = legacyKey.mid(split + 1);
        if (!numericSuffix.match(suffix).hasMatch()) continue;   // identity 兜底键，跳过
        bool ok = false;
        const long long timestamp = suffix.toLongLong(&ok);
        if (!ok || timestamp <= 0) continue;
        StoredReport r;
        r.quakeKey = quakePrimaryGroupKey(agency.toStdString(), timestamp,
                                          read.value(10).toDouble(), read.value(11).toDouble());
        if (r.quakeKey.isEmpty()) continue;
        r.reportNum = read.value(1).toInt();
        r.reportKey = QStringLiteral("n:") + QString::number(r.reportNum);
        r.sourceId = read.value(2).toString();
        r.sourceTag = read.value(3).toString();
        r.receivedAt = read.value(4).toLongLong();
        r.rawJson = read.value(5).toString();
        r.timestamp = read.value(6).toLongLong();
        r.reportTime = read.value(7).toLongLong();
        r.magnitude = read.value(8).toDouble();
        r.depth = read.value(9).toDouble();
        r.latitude = read.value(10).toDouble();
        r.longitude = read.value(11).toDouble();
        r.location = read.value(12).toString();
        r.maxIntensityText = read.value(13).toString();
        r.isFinal = read.value(14).toInt() != 0;
        r.isCanceled = read.value(15).toInt() != 0;
        migrated.push_back(r);
        if (migrated.size() >= 2000) {
            writeReportsBlocking(migrated);
            migrated.clear();
        }
    }
    read.finish();
    if (!migrated.isEmpty()) writeReportsBlocking(migrated);
    QSqlQuery drop(db_);
    return drop.exec(QStringLiteral("DROP TABLE event_reports_legacy"));
}

bool HistoryStore::writeReportsBlocking(const QVector<StoredReport>& reports) {
    if (reports.isEmpty()) return true;
    QSqlQuery q(db_);
    q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO event_reports (quake_key, report_key, report_num, source_id, "
        "source_tag, received_at, raw_json, timestamp, report_time, magnitude, depth, latitude, "
        "longitude, location, max_intensity_text, is_final, is_canceled, is_info, determination) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    for (const StoredReport& r : reports) {
        q.addBindValue(r.quakeKey);
        q.addBindValue(r.reportKey);
        q.addBindValue(r.reportNum);
        q.addBindValue(r.sourceId);
        q.addBindValue(r.sourceTag);
        q.addBindValue(r.receivedAt);
        q.addBindValue(r.rawJson);
        q.addBindValue(r.timestamp);
        q.addBindValue(r.reportTime);
        q.addBindValue(r.magnitude);
        q.addBindValue(r.depth);
        q.addBindValue(r.latitude);
        q.addBindValue(r.longitude);
        q.addBindValue(r.location);
        q.addBindValue(r.maxIntensityText);
        q.addBindValue(r.isFinal ? 1 : 0);
        q.addBindValue(r.isCanceled ? 1 : 0);
        q.addBindValue(r.isInfo ? 1 : 0);
        q.addBindValue(r.determination);
        if (!q.exec()) {
            qWarning() << "[history] migrate report failed:" << q.lastError().text();
            return false;
        }
    }
    return true;
}

QVector<EarthquakeEvent> HistoryStore::loadRecent(int limit) const {
    QVector<EarthquakeEvent> out;
    if (!ready_) return out;
    QSqlQuery q(db_);
    q.prepare(QStringLiteral(
        "SELECT id, source, magnitude, latitude, longitude, depth, location, timestamp, "
        "distance_km, estimated_intensity, raw_intensity, p_wave_arrival, s_wave_arrival, "
        "warning_level, report_num, is_final, is_canceled, source_provider, source_agency, "
        "event_id, report_time, max_intensity_raw, max_intensity_text, source_id, "
        "is_info, determination, source_tags FROM events ORDER BY timestamp DESC LIMIT ?"));
    q.addBindValue(limit);
    if (!q.exec()) return out;

    while (q.next()) {
        EarthquakeEvent e;
        e.id = q.value(0).toString().toStdString();
        e.source = q.value(1).toString().toStdString();
        e.magnitude = q.value(2).toDouble();
        e.latitude = q.value(3).toDouble();
        e.longitude = q.value(4).toDouble();
        e.depth = q.value(5).toDouble();
        e.location = q.value(6).toString().toStdString();
        e.timestamp = q.value(7).toLongLong();
        e.distanceKm = q.value(8).toDouble();
        e.estimatedIntensity = q.value(9).toString().toStdString();
        e.rawIntensity = q.value(10).toDouble();
        if (!q.value(11).isNull()) e.pWaveArrival = q.value(11).toLongLong();
        if (!q.value(12).isNull()) e.sWaveArrival = q.value(12).toLongLong();
        e.warningLevel = static_cast<WarningLevel>(q.value(13).toInt());
        e.reportNum = q.value(14).toInt();
        e.isFinal = q.value(15).toInt() != 0;
        e.isCanceled = q.value(16).toInt() != 0;
        e.sourceProvider = q.value(17).toString().toStdString();
        e.sourceAgency = q.value(18).toString().toStdString();
        e.eventId = q.value(19).toString().toStdString();
        e.reportTime = q.value(20).toLongLong();
        e.maxIntensityRaw = q.value(21).toDouble();
        e.maxIntensityText = q.value(22).toString().toStdString();
        e.sourceId = q.value(23).toString().toStdString();
        e.isInfo = q.value(24).toInt() != 0;
        e.determination = q.value(25).toString().toStdString();
        e.sourceTags = sourceTagsFromText(q.value(26).toString());
        // 升级前入库的行没有标签列内容：至少退回它自己的来源，别让列表标签空着。
        if (e.sourceTags.empty()) {
            const QString provider = QString::fromStdString(e.sourceProvider).trimmed();
            const QString agency = QString::fromStdString(e.sourceAgency).trimmed();
            const QString tag = provider.isEmpty()
                                    ? agency
                                    : (agency.isEmpty() ? provider
                                                        : provider + QStringLiteral("·") + agency);
            if (!tag.isEmpty()) e.sourceTags.push_back(tag.toStdString());
        }
        out.push_back(e);
    }
    return out;
}

void HistoryStore::upsertAll(const QVector<EarthquakeEvent>& events) {
    if (!ready_ || events.isEmpty()) return;
    if (!db_.transaction()) return;
    QSqlQuery q(db_);
    q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO events (id, source, magnitude, latitude, longitude, depth, location, "
        "timestamp, distance_km, estimated_intensity, raw_intensity, p_wave_arrival, s_wave_arrival, "
        "warning_level, report_num, is_final, is_canceled, source_provider, source_agency, "
        "event_id, report_time, max_intensity_raw, max_intensity_text, source_id, "
        "is_info, determination, source_tags) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    for (const EarthquakeEvent& e : events) {
        q.addBindValue(QString::fromStdString(e.id));
        q.addBindValue(QString::fromStdString(e.source));
        q.addBindValue(e.magnitude);
        q.addBindValue(e.latitude);
        q.addBindValue(e.longitude);
        q.addBindValue(e.depth);
        q.addBindValue(QString::fromStdString(e.location));
        q.addBindValue(e.timestamp);
        q.addBindValue(e.distanceKm);
        q.addBindValue(QString::fromStdString(e.estimatedIntensity));
        q.addBindValue(e.rawIntensity);
        q.addBindValue(e.pWaveArrival ? QVariant(*e.pWaveArrival) : QVariant());
        q.addBindValue(e.sWaveArrival ? QVariant(*e.sWaveArrival) : QVariant());
        q.addBindValue(warningCode(e.warningLevel));
        q.addBindValue(e.reportNum);
        q.addBindValue(e.isFinal ? 1 : 0);
        q.addBindValue(e.isCanceled ? 1 : 0);
        q.addBindValue(QString::fromStdString(e.sourceProvider));
        q.addBindValue(QString::fromStdString(e.sourceAgency));
        q.addBindValue(QString::fromStdString(e.eventId));
        q.addBindValue(e.reportTime);
        q.addBindValue(e.maxIntensityRaw);
        q.addBindValue(QString::fromStdString(e.maxIntensityText));
        q.addBindValue(QString::fromStdString(e.sourceId));
        q.addBindValue(e.isInfo ? 1 : 0);
        q.addBindValue(QString::fromStdString(e.determination));
        q.addBindValue(sourceTagsToText(e.sourceTags));
        if (!q.exec()) {
            db_.rollback();
            return;
        }
    }
    if (!db_.commit()) db_.rollback();
}

void HistoryStore::upsertReports(const QVector<StoredReport>& reports) {
    if (!ready_ || reports.isEmpty()) return;
    if (!db_.transaction()) return;
    writeReportsBlocking(reports);
    if (!db_.commit()) db_.rollback();
}

QVector<StoredReport> HistoryStore::loadReports(const QString& quakeKey) const {
    QVector<StoredReport> out;
    if (!ready_ || quakeKey.isEmpty()) return out;
    QSqlQuery q(db_);
    q.prepare(QStringLiteral(
        "SELECT quake_key, report_key, report_num, source_id, source_tag, received_at, raw_json, "
        "timestamp, report_time, magnitude, depth, latitude, longitude, location, "
        "max_intensity_text, is_final, is_canceled, is_info, determination "
        "FROM event_reports WHERE quake_key = ? ORDER BY report_num ASC, received_at ASC"));
    q.addBindValue(quakeKey);
    if (!q.exec()) return out;
    while (q.next()) {
        StoredReport r;
        r.quakeKey = q.value(0).toString();
        r.reportKey = q.value(1).toString();
        r.reportNum = q.value(2).toInt();
        r.sourceId = q.value(3).toString();
        r.sourceTag = q.value(4).toString();
        r.receivedAt = q.value(5).toLongLong();
        r.rawJson = q.value(6).toString();
        r.timestamp = q.value(7).toLongLong();
        r.reportTime = q.value(8).toLongLong();
        r.magnitude = q.value(9).toDouble();
        r.depth = q.value(10).toDouble();
        r.latitude = q.value(11).toDouble();
        r.longitude = q.value(12).toDouble();
        r.location = q.value(13).toString();
        r.maxIntensityText = q.value(14).toString();
        r.isFinal = q.value(15).toInt() != 0;
        r.isCanceled = q.value(16).toInt() != 0;
        r.isInfo = q.value(17).toInt() != 0;
        r.determination = q.value(18).toString();
        out.push_back(r);
    }
    return out;
}

void HistoryStore::prune(int keep) {
    if (!ready_) return;
    QSqlQuery q(db_);
    q.prepare(QStringLiteral(
        "DELETE FROM events WHERE id NOT IN "
        "(SELECT id FROM events ORDER BY timestamp DESC LIMIT ?)"));
    q.addBindValue(keep);
    q.exec();
    // 报次归档不按事件 id 关联（跨链路合并后会换 id），按接收时间保留最近一段：
    // 8000 行约等于 400 场地震各 20 报，足够覆盖列表里的历史条目。
    if (!q.exec(QStringLiteral(
            "DELETE FROM event_reports WHERE rowid NOT IN "
            "(SELECT rowid FROM event_reports ORDER BY received_at DESC LIMIT 8000)"))) {
        qWarning() << "[history] prune event_reports failed:" << q.lastError().text();
    }
}

void HistoryStore::clear() {
    if (!ready_) return;
    QSqlQuery q(db_);
    q.exec(QStringLiteral("DELETE FROM events"));
    q.exec(QStringLiteral("DELETE FROM event_reports"));
}

void HistoryStore::saveTombstone(const QString& key, long long endedAt) {
    if (!ready_ || key.isEmpty() || !db_.transaction()) return;
    QSqlQuery q(db_);
    q.prepare(QStringLiteral(
        "INSERT INTO warning_tombstones(identity, ended_at) VALUES (?, ?) "
        "ON CONFLICT(identity) DO UPDATE SET ended_at = MAX(ended_at, excluded.ended_at)"));
    q.addBindValue(key);
    q.addBindValue(endedAt);
    if (!q.exec()) {
        db_.rollback();
        return;
    }
    q.prepare(QStringLiteral("DELETE FROM warning_tombstones WHERE ended_at < ?"));
    q.addBindValue(endedAt - 24LL * 60 * 60 * 1000);
    if (!q.exec() || !q.exec(QStringLiteral(
            "DELETE FROM warning_tombstones WHERE identity NOT IN "
            "(SELECT identity FROM warning_tombstones "
            "ORDER BY ended_at DESC, identity ASC LIMIT 4096)"))) {
        db_.rollback();
        return;
    }
    q.finish();
    if (!db_.commit()) db_.rollback();
}

QHash<QString, long long> HistoryStore::loadTombstones(long long now) const {
    QHash<QString, long long> result;
    if (!ready_) return result;
    QSqlQuery q(db_);
    q.prepare(QStringLiteral(
        "SELECT identity, ended_at FROM warning_tombstones WHERE ended_at >= ? "
        "ORDER BY ended_at DESC, identity ASC LIMIT 4096"));
    q.addBindValue(now - 24LL * 60 * 60 * 1000);
    if (!q.exec()) return result;
    while (q.next()) result.insert(q.value(0).toString(), q.value(1).toLongLong());
    return result;
}

} // namespace komira
