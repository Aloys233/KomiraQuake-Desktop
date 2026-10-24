#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QSortFilterProxyModel>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <functional>
#include <vector>

namespace komira {

/// 侧栏事件行：**纯值**结构，刻意不经过 QVariantMap。
///
/// 早期实现把每条事件装进一张 QVariantMap：一次目录刷新要为 200 条事件各建一张约 40 项
/// 的 map（含两次 QDateTime::toString），delegate 取 `model.event` 时还要整张复制一次，
/// 而 QML 侧再把它转成 JS 对象 —— 三层全量转换。现在每行只保留 delegate 真正用到的
/// 十几个标量字段：`data()` 直接返回标量，筛选代理直接读字段，全程无 QVariant 容器。
///
/// 行的身份由 `id` 决定：刷新时按 id 原地对齐，新增/移动/更新只影响对应的 delegate。
class EventListModel final : public QAbstractListModel {
    Q_OBJECT
public:
    /// QML 角色名统一加 `event` 前缀：`id` / `source` / `focused` 这类名字容易与
    /// Item/控件的内置属性撞名，`required property` 一旦撞名就会静默绑定失败。
    enum Role {
        EventIdRole = Qt::UserRole + 1,
        EventLocationRole,
        EventMagnitudeTextRole,
        EventDepthTextRole,
        EventTimeTextRole,
        EventSourceTagRole,
        EventSourceTagsRole,
        EventIntensityRole,
        EventIntensityLabelRole,
        EventIntensityColorRole,
        EventActiveRole,
        EventFocusedRole,
        /// 距用户的距离（km，整数文本）；无定位时为空串。
        EventDistanceTextRole,
    };

    struct Row {
        QString id;
        QString location;
        /// 原始数据源展示名（搜索用；与 sourceTag 不同，后者是「提供方·机构」）。
        QString source;
        QString magnitudeText;
        QString depthText;
        QString timeText;
        QString sourceTag;
        /// 同一物理事件可能同时被多个聚合源收录；保留完整来源集合供 UI 排版。
        QStringList sourceTags;
        QString intensity;
        QString intensityLabel;
        QString intensityColor;
        QString levelTag;
        long long timestamp = 0;
        double magnitude = 0.0;
        double distance = -1.0;
        QString distanceText;
        bool hasDistance = false;
        bool isActive = false;
        bool focused = false;

        bool operator==(const Row& other) const;
        bool operator!=(const Row& other) const { return !(*this == other); }
    };

    explicit EventListModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    /// 整表对齐（按 id 复用行身份）：只有真正变化的行才发 dataChanged。
    void setRows(std::vector<Row> rows);
    /// 焦点变化：只对真正改变的两行发 dataChanged，不重建整表。
    void setFocusedId(const QString& id);
    /// 供筛选代理直接读字段，避免 data() -> toMap() 的 QVariant 往返与堆分配。
    const Row* rowAt(int row) const;

private:
    std::vector<Row> rows_;
};

/// 侧栏筛选代理。筛选项只改变代理行，源模型与 delegate 生命周期不受影响。
///
/// 「近 24 小时」是**时间相关**条件：行本身不变，但随着时间推移会自然失效。
/// 因此代理自带一个 60 s 定时器，在选中该筛选时重新求值；否则列表会停在首次
/// 筛选结果上，过了 24 h 的条目永远不下线。
class EventFilterModel final : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit EventFilterModel(QObject* parent = nullptr);

    Q_INVOKABLE void setFilter(const QString& filter, const QString& searchQuery);
    /// 供时效筛选使用的应用时钟（校时后的 now）。未注入时退回系统时钟。
    void setNowProvider(std::function<long long()> provider) { nowProvider_ = std::move(provider); }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    long long nowMs() const;

    QString filter_ = QStringLiteral("ALL");
    QString searchQuery_;
    std::function<long long()> nowProvider_;
    QTimer recencyTimer_;
};

} // namespace komira
