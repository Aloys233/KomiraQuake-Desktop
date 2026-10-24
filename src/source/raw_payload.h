#pragma once

#include <QJsonDocument>
#include <QJsonObject>

#include <string>

#include "model/earthquake_event.h"

namespace komira {

/// 把收到的原始报文挂到事件上：详情页展示「原始 JSON」用。
///
/// 只做一次紧凑序列化（源侧本来就已经把同一个 object 解析成事件，多这一次
/// `toJson` 的代价远小于在详情页反推字段）。不写入 events 表，避免把历史库
/// 撑大；报次记录单独落在 event_reports 表。
inline void attachRawPayload(EarthquakeEvent& event, const QJsonObject& payload) {
    if (payload.isEmpty()) return;
    event.rawJson = QJsonDocument(payload).toJson(QJsonDocument::Compact).toStdString();
}

} // namespace komira
