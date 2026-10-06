#pragma once

#include <QMetaType>

namespace komira {

/// 数据源事件类别。Live 走告警链路（实时预警：告警音、倒计时、HUD）；
/// Directory 只进列表与历史库，不触发告警。《NATIVE_PORT_SPEC》 §2.2。
enum class SourceEventKind { Live, Directory };

} // namespace komira

Q_DECLARE_METATYPE(komira::SourceEventKind)
