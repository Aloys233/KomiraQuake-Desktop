#include "model/event_list_model.h"

#include <QDateTime>

namespace komira {

bool EventListModel::Row::operator==(const Row& other) const {
    // 逐字段比较：只要 delegate 读到的内容一致就不发 dataChanged。
    // focused 单独走 setFocusedId()，这里也参与比较是安全的（它确实影响绘制）。
    return id == other.id && location == other.location && source == other.source
           && magnitudeText == other.magnitudeText && depthText == other.depthText
           && timeText == other.timeText && sourceTag == other.sourceTag
           && intensity == other.intensity && intensityLabel == other.intensityLabel
           && intensityColor == other.intensityColor && levelTag == other.levelTag
           && timestamp == other.timestamp && magnitude == other.magnitude
           && distance == other.distance && hasDistance == other.hasDistance
           && isActive == other.isActive && focused == other.focused;
}

int EventListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

const EventListModel::Row* EventListModel::rowAt(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return nullptr;
    return &rows_[static_cast<std::size_t>(row)];
}

QVariant EventListModel::data(const QModelIndex& index, int role) const {
    const Row* row = rowAt(index.row());
    if (!index.isValid() || !row) return {};
    switch (role) {
    case EventIdRole: return row->id;
    case EventLocationRole: return row->location;
    case EventMagnitudeTextRole: return row->magnitudeText;
    case EventDepthTextRole: return row->depthText;
    case EventTimeTextRole: return row->timeText;
    case EventSourceTagRole: return row->sourceTag;
    case EventIntensityRole: return row->intensity;
    case EventIntensityLabelRole: return row->intensityLabel;
    case EventIntensityColorRole: return row->intensityColor;
    case EventActiveRole: return row->isActive;
    case EventFocusedRole: return row->focused;
    default: return {};
    }
}

QHash<int, QByteArray> EventListModel::roleNames() const {
    return {
        {EventIdRole, QByteArrayLiteral("eventId")},
        {EventLocationRole, QByteArrayLiteral("eventLocation")},
        {EventMagnitudeTextRole, QByteArrayLiteral("eventMagnitudeText")},
        {EventDepthTextRole, QByteArrayLiteral("eventDepthText")},
        {EventTimeTextRole, QByteArrayLiteral("eventTimeText")},
        {EventSourceTagRole, QByteArrayLiteral("eventSourceTag")},
        {EventIntensityRole, QByteArrayLiteral("eventIntensity")},
        {EventIntensityLabelRole, QByteArrayLiteral("eventIntensityLabel")},
        {EventIntensityColorRole, QByteArrayLiteral("eventIntensityColor")},
        {EventActiveRole, QByteArrayLiteral("eventActive")},
        {EventFocusedRole, QByteArrayLiteral("eventFocused")},
    };
}

void EventListModel::setRows(std::vector<Row> rows) {
    // 按 id 做最小增量：一次目录刷新里绝大多数行只是字段微调，
    // 只有真正新增/删除/移动才走 beginInsertRows / beginRemoveRows / beginMoveRows。
    // 行身份稳定，ListView 的 delegate 才能被复用（含滚动位置与悬停态）。
    //
    // 旧实现为每一行都向后线性扫一遍找同 id 行，200 条目录更新就是 4 万次 QString 比较；
    // 这里先建一次 id -> 行号索引，查找退化成哈希，整体 O(n)。
    QHash<QString, int> indexById;
    indexById.reserve(static_cast<qsizetype>(rows_.size()));
    const auto rebuildIndex = [this, &indexById]() {
        indexById.clear();
        for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
            indexById.insert(rows_[static_cast<std::size_t>(i)].id, i);
        }
    };
    rebuildIndex();

    for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
        const QString& wantedId = rows[static_cast<std::size_t>(row)].id;
        if (row < static_cast<int>(rows_.size())
            && rows_.at(static_cast<std::size_t>(row)).id == wantedId) {
            if (rows_.at(static_cast<std::size_t>(row)) != rows[static_cast<std::size_t>(row)]) {
                rows_[static_cast<std::size_t>(row)] = rows[static_cast<std::size_t>(row)];
                emit dataChanged(index(row), index(row), {});
            }
            continue;
        }

        const auto found = indexById.constFind(wantedId);
        const int existing = (found != indexById.constEnd() && found.value() > row) ? found.value() : -1;
        if (existing >= 0) {
            beginMoveRows({}, existing, existing, {}, row);
            rows_.erase(rows_.begin() + existing);
            rows_.insert(rows_.begin() + row, rows[static_cast<std::size_t>(row)]);
            endMoveRows();
        } else {
            beginInsertRows({}, row, row);
            rows_.insert(rows_.begin() + row, rows[static_cast<std::size_t>(row)]);
            endInsertRows();
        }
        // 结构性变更（移动/插入）是低频路径：与其增量维护偏移，不如整体重建索引，
        // 避免漏改导致某行被误判为「已存在」而静默丢失。
        rebuildIndex();
    }

    if (static_cast<int>(rows_.size()) > static_cast<int>(rows.size())) {
        const int first = static_cast<int>(rows.size());
        const int last = static_cast<int>(rows_.size()) - 1;
        beginRemoveRows({}, first, last);
        rows_.resize(rows.size());
        endRemoveRows();
    }
}

void EventListModel::setFocusedId(const QString& id) {
    for (int row = 0; row < static_cast<int>(rows_.size()); ++row) {
        Row& current = rows_[static_cast<std::size_t>(row)];
        const bool wanted = !id.isEmpty() && current.id == id;
        if (current.focused == wanted) continue;
        current.focused = wanted;
        emit dataChanged(index(row), index(row), {EventFocusedRole});
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
    const auto* model = qobject_cast<const EventListModel*>(sourceModel());
    if (!model) return true;
    const EventListModel::Row* row = model->rowAt(sourceRow);
    if (!row) return false;
    (void)sourceParent;

    if (filter_ == QLatin1String("WITHIN_500")
        && !(row->hasDistance && row->distance <= 500.0))
        return false;
    if (filter_ == QLatin1String("M4_PLUS") && row->magnitude < 4.0) return false;
    if (filter_ == QLatin1String("RECENT_24H")
        && QDateTime::currentMSecsSinceEpoch() - row->timestamp > 24LL * 3600LL * 1000LL)
        return false;
    if (!searchQuery_.isEmpty()) {
        // 只在需要时做一次大小写折叠；预转换放进行构建期（见 EventListModel::Row::source）。
        if (!row->location.contains(searchQuery_, Qt::CaseInsensitive)
            && !row->source.contains(searchQuery_, Qt::CaseInsensitive)) {
            return false;
        }
    }
    return true;
}

} // namespace komira
