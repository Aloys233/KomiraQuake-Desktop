#pragma once

#include <QHash>
#include <QSqlDatabase>
#include <QString>
#include <QVector>

#include "model/earthquake_event.h"

namespace komira {

/// SQLite 历史存储。表结构与《NATIVE_PORT_SPEC》 §6 一致。
class HistoryStore {
public:
    HistoryStore() = default;
    ~HistoryStore();

    bool init(const QString& path);
    bool isReady() const { return ready_; }

    QVector<EarthquakeEvent> loadRecent(int limit = 200) const;
    void upsertAll(const QVector<EarthquakeEvent>& events);
    void prune(int keep = 400);
    void clear();
    // Separate from visible history: clearing history must not revive alerts.
    void saveTombstone(const QString& key, long long endedAt);
    QHash<QString, long long> loadTombstones(long long now) const;

private:
    bool createTable();

    QSqlDatabase db_;
    bool ready_ = false;
    QString connectionName_;
};

} // namespace komira
