#include "source/whews_protocol.h"

namespace komira {

const std::vector<WhewsChannel>& whewsChannels() {
    static const std::vector<WhewsChannel> channels = {
        // EEW：告警链路。eventNs 与 Wolfx / Pancakes 同名频道对齐，便于跨源合并。
        {QStringLiteral("cea"), QStringLiteral("CEA"), QStringLiteral("cenc_eew"), SourceEventKind::Live, WhewsTimeZone::Utc8},
        {QStringLiteral("cea-pr"), QStringLiteral("CEA"), QStringLiteral("cenc_eew"), SourceEventKind::Live, WhewsTimeZone::Utc8},
        {QStringLiteral("jma_eew"), QStringLiteral("JMA"), QStringLiteral("jma_eew"), SourceEventKind::Live, WhewsTimeZone::Jst},
        {QStringLiteral("cwa_eew"), QStringLiteral("CWA"), QStringLiteral("cwa_eew"), SourceEventKind::Live, WhewsTimeZone::Utc8},
        {QStringLiteral("sa_eew"), QStringLiteral("SA"), QStringLiteral("sa_eew"), SourceEventKind::Live, WhewsTimeZone::Utc8},
        {QStringLiteral("kma_eew"), QStringLiteral("KMA"), QStringLiteral("kma_eew"), SourceEventKind::Live, WhewsTimeZone::Utc8},
        {QStringLiteral("early_est"), QStringLiteral("Early-est"), QStringLiteral("early_est"), SourceEventKind::Live, WhewsTimeZone::Utc8},
        // 速报/情报：只进列表与历史。cenc 用上游 id 原文，与 Wolfx cenc_eqlist 对齐。
        {QStringLiteral("cenc"), QStringLiteral("CENC"), QString(), SourceEventKind::Directory, WhewsTimeZone::Utc8},
        {QStringLiteral("jma"), QStringLiteral("JMA"), QStringLiteral("jma_eqlist"), SourceEventKind::Directory, WhewsTimeZone::Jst},
        {QStringLiteral("cwa"), QStringLiteral("CWA"), QStringLiteral("cwa_eqlist"), SourceEventKind::Directory, WhewsTimeZone::Utc8},
        {QStringLiteral("kma"), QStringLiteral("KMA"), QStringLiteral("kma_eqlist"), SourceEventKind::Directory, WhewsTimeZone::Utc8},
        {QStringLiteral("usgs"), QStringLiteral("USGS"), QStringLiteral("usgs"), SourceEventKind::Directory, WhewsTimeZone::Utc8},
        {QStringLiteral("emsc"), QStringLiteral("EMSC"), QStringLiteral("emsc"), SourceEventKind::Directory, WhewsTimeZone::Utc8},
        {QStringLiteral("hko"), QStringLiteral("HKO"), QStringLiteral("hko_eqlist"), SourceEventKind::Directory, WhewsTimeZone::Utc8},
        {QStringLiteral("bcsf"), QStringLiteral("BCSF"), QStringLiteral("bcsf"), SourceEventKind::Directory, WhewsTimeZone::Utc8},
        {QStringLiteral("bmkg"), QStringLiteral("BMKG"), QStringLiteral("bmkg"), SourceEventKind::Directory, WhewsTimeZone::Utc8},
        {QStringLiteral("geonet"), QStringLiteral("GeoNet"), QStringLiteral("geonet"), SourceEventKind::Directory, WhewsTimeZone::Utc8},
        {QStringLiteral("nrcan"), QStringLiteral("NRCan"), QStringLiteral("nrcan"), SourceEventKind::Directory, WhewsTimeZone::Utc8},
        {QStringLiteral("tmd"), QStringLiteral("TMD"), QStringLiteral("tmd"), SourceEventKind::Directory, WhewsTimeZone::Utc8},
        {QStringLiteral("usp"), QStringLiteral("USP"), QStringLiteral("usp"), SourceEventKind::Directory, WhewsTimeZone::Utc8},
        {QStringLiteral("gfz"), QStringLiteral("GFZ"), QStringLiteral("gfz"), SourceEventKind::Directory, WhewsTimeZone::Utc8},
        {QStringLiteral("ingv"), QStringLiteral("INGV"), QStringLiteral("ingv"), SourceEventKind::Directory, WhewsTimeZone::Utc8},
        {QStringLiteral("bgs"), QStringLiteral("BGS"), QStringLiteral("bgs"), SourceEventKind::Directory, WhewsTimeZone::Utc8},
        {QStringLiteral("mmd"), QStringLiteral("MMD"), QStringLiteral("mmd"), SourceEventKind::Directory, WhewsTimeZone::Utc8},
        {QStringLiteral("phivolcs"), QStringLiteral("PHIVOLCS"), QStringLiteral("phivolcs"), SourceEventKind::Directory, WhewsTimeZone::Utc8},
        {QStringLiteral("ipma"), QStringLiteral("IPMA"), QStringLiteral("ipma"), SourceEventKind::Directory, WhewsTimeZone::Utc8},
        {QStringLiteral("afad"), QStringLiteral("AFAD"), QStringLiteral("afad"), SourceEventKind::Directory, WhewsTimeZone::Utc8},
        {QStringLiteral("sed"), QStringLiteral("SED"), QStringLiteral("sed"), SourceEventKind::Directory, WhewsTimeZone::Utc8},
        {QStringLiteral("cenais"), QStringLiteral("CENAIS"), QStringLiteral("cenais"), SourceEventKind::Directory, WhewsTimeZone::Utc8},
        {QStringLiteral("gsras"), QStringLiteral("GSRAS"), QStringLiteral("gsras"), SourceEventKind::Directory, WhewsTimeZone::Utc8},
        {QStringLiteral("ga"), QStringLiteral("GA"), QStringLiteral("ga"), SourceEventKind::Directory, WhewsTimeZone::Utc8},
        {QStringLiteral("ssn"), QStringLiteral("SSN"), QStringLiteral("ssn"), SourceEventKind::Directory, WhewsTimeZone::Utc8},
        {QStringLiteral("noa"), QStringLiteral("NOA"), QStringLiteral("noa"), SourceEventKind::Directory, WhewsTimeZone::Utc8},
        {QStringLiteral("scsn"), QStringLiteral("SCSN"), QStringLiteral("scsn"), SourceEventKind::Directory, WhewsTimeZone::Utc8},
        {QStringLiteral("iag"), QStringLiteral("IAG"), QStringLiteral("iag"), SourceEventKind::Directory, WhewsTimeZone::Utc8},
        {QStringLiteral("igp"), QStringLiteral("IGP"), QStringLiteral("igp"), SourceEventKind::Directory, WhewsTimeZone::Utc8},
        {QStringLiteral("nepal"), QStringLiteral("NEPAL"), QStringLiteral("nepal"), SourceEventKind::Directory, WhewsTimeZone::Utc8},
        {QStringLiteral("funvisis"), QStringLiteral("FUNVISIS"), QStringLiteral("funvisis"), SourceEventKind::Directory, WhewsTimeZone::Utc8},
        {QStringLiteral("beijing"), QString::fromUtf8("北京地震局"), QStringLiteral("beijing"), SourceEventKind::Directory, WhewsTimeZone::Utc8},
        {QStringLiteral("yunnan"), QString::fromUtf8("云南地震局"), QStringLiteral("yunnan"), SourceEventKind::Directory, WhewsTimeZone::Utc8},
        {QStringLiteral("ningxia"), QString::fromUtf8("宁夏地震局"), QStringLiteral("ningxia"), SourceEventKind::Directory, WhewsTimeZone::Utc8},
    };
    return channels;
}

const WhewsChannel* whewsChannelFor(const QString& source) {
    for (const auto& channel : whewsChannels())
        if (channel.source == source) return &channel;
    return nullptr;
}

QString whewsTitleFor(const WhewsChannel& channel) {
    return channel.kind == SourceEventKind::Live
        ? channel.agency + QStringLiteral(" 地震预警")
        : channel.agency + QStringLiteral(" 地震情报");
}

} // namespace komira