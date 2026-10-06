#pragma once

#include <QJsonObject>
#include <QString>
#include <optional>

#include "core/intensity_calculator.h"
#include "model/earthquake_event.h"
#include "source/eew_parser.h"
#include "source/source_event_kind.h"

namespace komira {

struct PancakesParsed {
    EarthquakeEvent event;
    SourceEventKind kind = SourceEventKind::Live;
    QString source;
};

/// PancakesAPI 地震事件解析。
///
/// 聚合通道外层为 `{source, type, action, timestampMs, payload}`；payload 结构与该子源 HTTP
/// 列表项字段一致。`eventId` 以 `<source>:` 前缀区分，保证四个子源之间即使上游 id 相同
/// （如 JMA 的 EEW 与速报共用 EventID）也不会在合并/去重时互相污染。
class PancakesParser {
public:
    static std::optional<PancakesParsed> parseRealtime(const QJsonObject& envelope,
                                                       const EewParser::UserLocation& user,
                                                       IntensityStandard standard,
                                                       long long nowMs);

    /// HTTP 列表项（GET /api/v1/alert/quake/{source}）→ 目录条目。
    static std::optional<EarthquakeEvent> parseListItem(const QJsonObject& item,
                                                        const EewParser::UserLocation& user,
                                                        IntensityStandard standard,
                                                        long long nowMs);
};

} // namespace komira
