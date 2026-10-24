#pragma once

#include <QString>
#include <QVector>

#include "source/source_event_kind.h"

namespace komira {

/// 聚合源内部可单独启停的频道（子数据源）。
///
/// 子源 id 只在所属聚合源内唯一（例如 Wolfx 与 Whews 都有 jma_eew），
/// 持久化时用 `sourceId:subId` 复合键区分。
struct SubSourceInfo {
    QString id;
    QString name;
    SourceEventKind kind = SourceEventKind::Live;
};

/// 某聚合源的可配置子源；空列表表示该源没有子源（如模拟源）。
/// 顺序即设置页展示顺序：先实时预警、后地震情报。
const QVector<SubSourceInfo>& subSourcesFor(const QString& sourceId);

} // namespace komira
