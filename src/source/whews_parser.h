#pragma once

#include <optional>

#include "core/intensity_calculator.h"
#include "model/earthquake_event.h"
#include "source/eew_parser.h"
#include "source/whews_protocol.h"

namespace komira {

/// Whews 业务帧解析（`/ws/all` 的 `Data` 对象）。字段约定见 https://api.2v8.cn/docs。
class WhewsParser {
public:
    /// 单个业务记录（Data 对象）→ 事件。必填字段缺失返回 nullopt。
    static std::optional<EarthquakeEvent> parseRecord(const WhewsChannel& channel,
                                                      const QJsonObject& data,
                                                      const EewParser::UserLocation& user,
                                                      IntensityStandard standard);
};

} // namespace komira