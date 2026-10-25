#pragma once

#include <QJsonObject>
#include <QString>
#include <optional>

#include "core/intensity_calculator.h"
#include "model/earthquake_event.h"
#include "source/eew_parser.h"
#include "source/jian_protocol.h"

namespace komira {

/// Jian 业务帧解析。字段约定见 https://api.sismotide.top/api/
///
/// 46 路频道的字段并不统一，这里按**取值族**收口，而不是每个频道写一条分支：
/// - 发震时刻：`originTime` 可以是 epoch 毫秒（中国/台湾/美国等）或 ISO 8601 字符串
///   （JMA 带 `+09:00`；KMA/EMSC 等带别处偏移）。字符串一律走 `EewParser::parseTime`，
///   它按报文自带的偏移解析——**不按设备本地时区**，否则非 UTC+8 设备整体偏移。
/// - 烈度：JMA 系是震度文本（`1` / `5弱` / `5強`），CWA/CENC 是数值，KMA 是罗马数字
///   （`Ⅵ`）外加数值 `maxMMI`。三种都要能被 `maxIntensityRaw` 承载，见 parseIntensity。
/// - 地名：JMA 保留日文原文；KMA / 秘鲁等有 `placename_zh` 优先用它；其余英文地名
///   按坐标查「震源区」中文名（与 GQ / USGS 同一张表），查不到保留原文。
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
