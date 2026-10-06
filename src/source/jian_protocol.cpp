#include "source/jian_protocol.h"

namespace komira {

const std::vector<JianChannel>& jianChannels() {
    static const std::vector<JianChannel> channels = {
        // EEW：告警链路。eventId 频道前缀与 Wolfx / Pancakes 同名频道对齐，便于跨源合并。
        {QStringLiteral("cea"), QStringLiteral("CEA"), QStringLiteral("cenc_eew"), SourceEventKind::Live},
        {QStringLiteral("cwa-eew"), QStringLiteral("CWA"), QStringLiteral("cwa_eew"), SourceEventKind::Live},
        {QStringLiteral("jma-eew"), QStringLiteral("JMA"), QStringLiteral("jma_eew"), SourceEventKind::Live},
        // 速报/目录：只进列表与历史。cenc 用上游 id 原文，与 Wolfx cenc_eqlist 对齐。
        {QStringLiteral("cenc"), QStringLiteral("CENC"), QString(), SourceEventKind::Directory},
        {QStringLiteral("cwa"), QStringLiteral("CWA"), QStringLiteral("cwa_eqlist"), SourceEventKind::Directory},
        {QStringLiteral("jma"), QStringLiteral("JMA"), QStringLiteral("jma_eqlist"), SourceEventKind::Directory},
        {QStringLiteral("hko"), QStringLiteral("HKO"), QStringLiteral("hko_eqlist"), SourceEventKind::Directory},
    };
    return channels;
}

const JianChannel* jianChannelFor(const QString& type) {
    for (const auto& channel : jianChannels())
        if (channel.type == type) return &channel;
    return nullptr;
}

} // namespace komira
