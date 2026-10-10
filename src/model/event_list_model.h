#pragma once

#include <QAbstractListModel>
#include <QSortFilterProxyModel>
#include <QVariantList>

namespace komira {

/// Stable model used by the sidebar. Keeping the event rows in a Qt model avoids
/// replacing a JavaScript array (and all delegate contexts) on every directory item.
class EventListModel final : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role {
        EventRole = Qt::UserRole + 1,
    };

    explicit EventListModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setEvents(const QVariantList& events);

private:
    QList<QVariantMap> events_;
};

/// Filter proxy for the sidebar. Filtering changes proxy rows, not the lifetime of
/// the source model, so normal directory refreshes only update affected delegates.
class EventFilterModel final : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit EventFilterModel(QObject* parent = nullptr);

    Q_INVOKABLE void setFilter(const QString& filter, const QString& searchQuery);

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    QString filter_ = QStringLiteral("ALL");
    QString searchQuery_;
};

} // namespace komira
