#include "model/event_list_model.h"

#include <QDateTime>

namespace komira {

int EventListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : events_.size();
}

QVariant EventListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= events_.size()) return {};
    if (role == EventRole) return events_.at(index.row());
    return {};
}

QHash<int, QByteArray> EventListModel::roleNames() const {
    return {{EventRole, QByteArrayLiteral("event")}};
}

void EventListModel::setEvents(const QVariantList& events) {
    QList<QVariantMap> next;
    next.reserve(events.size());
    for (const QVariant& value : events) next.push_back(value.toMap());

    // Keep row identities stable where possible. This is deliberately incremental:
    // a changed magnitude updates one delegate, while a new directory row inserts one.
    for (int row = 0; row < next.size(); ++row) {
        const QString wantedId = next.at(row).value(QStringLiteral("id")).toString();
        if (row < events_.size()
            && events_.at(row).value(QStringLiteral("id")).toString() == wantedId) {
            if (events_.at(row) != next.at(row)) {
                events_[row] = next.at(row);
                emit dataChanged(index(row), index(row), {EventRole});
            }
            continue;
        }

        int existing = -1;
        for (int candidate = row + 1; candidate < events_.size(); ++candidate) {
            if (events_.at(candidate).value(QStringLiteral("id")).toString() == wantedId) {
                existing = candidate;
                break;
            }
        }
        if (existing >= 0) {
            beginMoveRows({}, existing, existing, {}, row);
            events_.move(existing, row);
            endMoveRows();
            if (events_.at(row) != next.at(row)) {
                events_[row] = next.at(row);
                emit dataChanged(index(row), index(row), {EventRole});
            }
        } else {
            beginInsertRows({}, row, row);
            events_.insert(row, next.at(row));
            endInsertRows();
        }
    }

    while (events_.size() > next.size()) {
        const int last = events_.size() - 1;
        beginRemoveRows({}, last, last);
        events_.removeLast();
        endRemoveRows();
    }
}

EventFilterModel::EventFilterModel(QObject* parent) : QSortFilterProxyModel(parent) {
    setDynamicSortFilter(true);
}

void EventFilterModel::setFilter(const QString& filter, const QString& searchQuery) {
    const QString normalizedFilter = filter.isEmpty() ? QStringLiteral("ALL") : filter;
    const QString normalizedQuery = searchQuery.trimmed().toLower();
    if (filter_ == normalizedFilter && searchQuery_ == normalizedQuery) return;
    filter_ = normalizedFilter;
    searchQuery_ = normalizedQuery;
    invalidateFilter();
}

bool EventFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const {
    const QModelIndex sourceIndex = sourceModel()->index(sourceRow, 0, sourceParent);
    const QVariantMap event = sourceModel()->data(sourceIndex, EventListModel::EventRole).toMap();
    if (filter_ == QLatin1String("WITHIN_500")
        && !(event.value(QStringLiteral("hasDistance")).toBool()
             && event.value(QStringLiteral("distance")).toDouble() <= 500.0))
        return false;
    if (filter_ == QLatin1String("M4_PLUS")
        && event.value(QStringLiteral("magnitude")).toDouble() < 4.0)
        return false;
    if (filter_ == QLatin1String("RECENT_24H")
        && QDateTime::currentMSecsSinceEpoch() - event.value(QStringLiteral("timestamp")).toLongLong()
               > 24LL * 3600LL * 1000LL)
        return false;
    if (!searchQuery_.isEmpty()) {
        const QString location = event.value(QStringLiteral("location")).toString().toLower();
        const QString source = event.value(QStringLiteral("source")).toString().toLower();
        if (!location.contains(searchQuery_) && !source.contains(searchQuery_)) return false;
    }
    return true;
}

} // namespace komira
