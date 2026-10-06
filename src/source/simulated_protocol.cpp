#include "source/simulated_protocol.h"

namespace komira {

QString simulatedTitleFor(SourceEventKind kind) {
    return kind == SourceEventKind::Live ? QStringLiteral("模拟数据源 地震预警")
                                         : QStringLiteral("模拟数据源 地震情报");
}

} // namespace komira
