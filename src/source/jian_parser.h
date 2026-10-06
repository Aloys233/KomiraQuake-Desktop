#pragma once

#include <QJsonObject>
#include <QString>
#include <optional>

#include "core/intensity_calculator.h"
#include "model/earthquake_event.h"
#include "source/eew_parser.h"
#include "source/jian_protocol.h"

namespace komira {

/// Jian 业务帧解析。字段约定见 https://api.sismotide.top/api/ §5/§6。
class JianParser {
public:
    /// 单个业务记录（Data 对象）→ 事件。必填字段缺失返回 nullopt。
    static std::optional<EarthquakeEvent> parseRecord(const JianChannel& channel,
                                                      const QJsonObject& data,
                                                      const EewParser::UserLocation& user,
                                                      IntensityStandard standard);

    /// 认证响应 {ok:true, token:"…"} → token；否则 nullopt。
    static std::optional<QString> parseAuthToken(const QByteArray& body);
};

} // namespace komira
