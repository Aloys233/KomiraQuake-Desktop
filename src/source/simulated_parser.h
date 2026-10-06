#pragma once

#include <QJsonObject>
#include <QString>

#include <optional>

#include "core/intensity_calculator.h"
#include "model/earthquake_event.h"
#include "source/eew_parser.h"
#include "source/source_event_kind.h"

namespace komira {

/// 模拟源报文解析（协议 sim-eew/1，见《NATIVE_PORT_SPEC》 §2.7）。
///
/// 服务端只发场景可表达的字段；距离、烈度、到时、预警等级一律由
/// [EewParser::recompute] 依用户定位与烈度制式本地派生。
class SimulatedParser {
public:
    /// 解析一帧 `report` / `directory`。必填项缺失返回 nullopt。
    ///
    /// 必填：`eventId`、`originTime`、`latitude`、`longitude`。
    static std::optional<EarthquakeEvent> parseReport(const QJsonObject& frame,
                                                      SourceEventKind kind,
                                                      const EewParser::UserLocation& user,
                                                      IntensityStandard standard,
                                                      long long nowMs);
};

} // namespace komira
