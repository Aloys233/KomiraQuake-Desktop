#include "model/event_list_model.h"

#include <QDateTime>
#include <QSet>

namespace komira {

bool EventListModel::Row::operator==(const Row& other) const {
    // 逐字段比较：只要 delegate 读到的内容一致就不发 dataChanged。
    // focused 单独走 setFocusedId()，这里也参与比较是安全的（它确实影响绘制）。
    return id == other.id && location == other.location && source == other.source
           && magnitudeText == other.magnitudeText && depthText == other.depthText
           && timeText == other.timeText && sourceTag == other.sourceTag
           && sourceTags == other.sourceTags
           && intensity == other.intensity && intensityLabel == other.intensityLabel
           && intensityColor == other.intensityColor && levelTag == other.levelTag
           && timestamp == other.timestamp && magnitude == other.magnitude
           && distance == other.distance && distanceText == other.distanceText
           && hasDistance == other.hasDistance
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
    case EventSourceTagsRole: return row->sourceTags;
    case EventIntensityRole: return row->intensity;
    case EventIntensityLabelRole: return row->intensityLabel;
    case EventIntensityColorRole: return row->intensityColor;
    case EventActiveRole: return row->isActive;
    case EventFocusedRole: return row->focused;
    case EventDistanceTextRole: return row->distanceText;
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
        {EventSourceTagsRole, QByteArrayLiteral("eventSourceTags")},
        {EventIntensityRole, QByteArrayLiteral("eventIntensity")},
        {EventIntensityLabelRole, QByteArrayLiteral("eventIntensityLabel")},
        {EventIntensityColorRole, QByteArrayLiteral("eventIntensityColor")},
        {EventActiveRole, QByteArrayLiteral("eventActive")},
        {EventFocusedRole, QByteArrayLiteral("eventFocused")},
        {EventDistanceTextRole, QByteArrayLiteral("eventDistanceText")},
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

    // 目录合并会让同一场地震的「最佳来源」在 Wolfx / Pancakes 之间切换，行的 id 随之
    // 变化，于是大量已有行需要挪位。逐行 beginMoveRows 在 QML 里每次都要重排 delegate：
    // 实测一次启动刷新 51 次 move 就卡住 GUI 261 ms（窗口刚出来时表现为「点不动」）。
    // 少量挪位仍走增量（保住滚动位置）；挪位很多时改成一次 reset：QML 只重建一次，
    // 代价远低于几十次 move。
    constexpr int kMaxIncrementalMoves = 8;
    int lastIndex = -1;
    int backwards = 0;
    for (const Row& wanted : rows) {
        const auto found = indexById.constFind(wanted.id);
        if (found == indexById.constEnd()) continue;
        if (found.value() < lastIndex) ++backwards;
        else lastIndex = found.value();
    }
    if (backwards > kMaxIncrementalMoves) {
        beginResetModel();
        rows_ = std::move(rows);
        endResetModel();
        return;
    }

    // 先删掉新列表里已经不存在的行，再按位置匹配/插入。旧实现只在末尾裁剪，
    // 一旦中间少了 1–2 行（换源导致 id 变化），后面每一行都会「找到更后面的同 id 行
    // 并上移」，把 2 次删除放大成上百次 move —— 启动刷新的主要卡顿就来自这里。
    QSet<QString> desiredIds;
    desiredIds.reserve(static_cast<qsizetype>(rows.size()));
    for (const Row& row : rows) desiredIds.insert(row.id);
    for (int i = static_cast<int>(rows_.size()) - 1; i >= 0; --i) {
        if (desiredIds.contains(rows_[static_cast<std::size_t>(i)].id)) continue;
        beginRemoveRows({}, i, i);
        rows_.erase(rows_.begin() + i);
        endRemoveRows();
    }
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
    // 「近 24h」随时间滑走：只有该筛选生效时才需要重算（200 行，代价可忽略）。
    // 行内容本身不变，所以不能指望 setRows / dataChanged 来触发这次过期。
    recencyTimer_.setInterval(60'000);
    connect(&recencyTimer_, &QTimer::timeout, this, [this]() {
        if (filter_ != QLatin1String("RECENT_24H")) return;
        beginFilterChange();
        endFilterChange();
    });
    recencyTimer_.start();
}

long long EventFilterModel::nowMs() const {
    return nowProvider_ ? nowProvider_() : QDateTime::currentMSecsSinceEpoch();
}

void EventFilterModel::setFilter(const QString& filter, const QString& searchQuery) {
    const QString normalizedFilter = filter.isEmpty() ? QStringLiteral("ALL") : filter;
    const QString normalizedQuery = searchQuery.trimmed().toLower();
    if (filter_ == normalizedFilter && searchQuery_ == normalizedQuery) return;
    filter_ = normalizedFilter;
    searchQuery_ = normalizedQuery;
    beginFilterChange();
    endFilterChange();
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
    if (filter_ == QLatin1String("RECENT_24H")) {
        // 区间必须**双向**判定：只判 `now - ts > 24h` 会把未来时间戳（校时漂移、
        // 源侧时间错误）当成「最近」，它们会永远钉在列表最前面。留 60 s 容差，
        // 与 EarthquakeEvent::expired() 对「未来」的容忍度一致。
        const long long now = nowMs();
        constexpr long long kFutureToleranceMs = 60'000;
        constexpr long long kWindowMs = 24LL * 3600LL * 1000LL;
        if (row->timestamp > now + kFutureToleranceMs) return false;
        if (now - row->timestamp > kWindowMs) return false;
    }
    if (!searchQuery_.isEmpty()) {
        // 只在需要时做一次大小写折叠；预转换放进行构建期（见 EventListModel::Row::source）。
        bool sourceMatched = row->source.contains(searchQuery_, Qt::CaseInsensitive);
        if (!sourceMatched) {
            for (const QString& tag : row->sourceTags) {
                if (tag.contains(searchQuery_, Qt::CaseInsensitive)) {
                    sourceMatched = true;
                    break;
                }
            }
        }
        if (!row->location.contains(searchQuery_, Qt::CaseInsensitive) && !sourceMatched) {
            return false;
        }
    }
    return true;
}

} // namespace komira
