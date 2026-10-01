#include "store/history_store.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QSet>
#include <QUuid>
#include <QVariant>

#include <utility>

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
        {QStringLiteral("max_intensity_text"), QStringLiteral("TEXT NOT NULL DEFAULT ''")}
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
    q.finish();
    if (db_.commit()) return true;
    db_.rollback();
    return false;
}

QVector<EarthquakeEvent> HistoryStore::loadRecent(int limit) const {
    QVector<EarthquakeEvent> out;
    if (!ready_) return out;
    QSqlQuery q(db_);
    q.prepare(QStringLiteral(
        "SELECT id, source, magnitude, latitude, longitude, depth, location, timestamp, "
        "distance_km, estimated_intensity, raw_intensity, p_wave_arrival, s_wave_arrival, "
        "warning_level, report_num, is_final, is_canceled, source_provider, source_agency, "
        "event_id, report_time, max_intensity_raw, max_intensity_text "
        "FROM events ORDER BY timestamp DESC LIMIT ?"));
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
        "event_id, report_time, max_intensity_raw, max_intensity_text) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
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
        if (!q.exec()) {
            db_.rollback();
            return;
        }
    }
    if (!db_.commit()) db_.rollback();
}

void HistoryStore::prune(int keep) {
    if (!ready_) return;
    QSqlQuery q(db_);
    q.prepare(QStringLiteral(
        "DELETE FROM events WHERE id NOT IN "
        "(SELECT id FROM events ORDER BY timestamp DESC LIMIT ?)"));
    q.addBindValue(keep);
    q.exec();
}

void HistoryStore::clear() {
    if (!ready_) return;
    QSqlQuery q(db_);
    q.exec(QStringLiteral("DELETE FROM events"));
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
