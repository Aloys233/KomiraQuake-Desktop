#include "source/jian_protocol.h"

namespace komira {

namespace {

/// 46 路地震类频道；顺序即设置页顺序（先实时预警，后地震情报）。
///
/// **eventNs / agency 必须与 Whews 的同源频道逐字一致**：合并键是
/// `EarthquakeEvent::identity() = "<agency>|<eventNs>:<上游 id>"`，两处写法不一致时
/// 同一场地震会落进两个键，列表里出现两条「同一地震」的重复条目。
const std::vector<JianChannel>& channelsImpl() {
    static const std::vector<JianChannel> channels = {
        // ── 实时预警（告警链路，7 路）────────────────────────────────
        // cea 与 cea-pr（省级分中心）都并入 cenc_eew：两者是同一场地震的两种发文，
        // 与 Whews 的 cea / cea-pr → cenc_eew 同口径。
        {QStringLiteral("cea"), QStringLiteral("CEA"), QStringLiteral("cenc_eew"), SourceEventKind::Live},
        {QStringLiteral("cea-pr"), QStringLiteral("CEA"), QStringLiteral("cenc_eew"), SourceEventKind::Live},
        {QStringLiteral("cwa-eew"), QStringLiteral("CWA"), QStringLiteral("cwa_eew"), SourceEventKind::Live},
        {QStringLiteral("jma-eew"), QStringLiteral("JMA"), QStringLiteral("jma_eew"), SourceEventKind::Live},
        {QStringLiteral("sa"), QStringLiteral("SA"), QStringLiteral("sa_eew"), SourceEventKind::Live},
        {QStringLiteral("kma-eew"), QStringLiteral("KMA"), QStringLiteral("kma_eew"), SourceEventKind::Live},
        // early-est（INGV 全球快速定位）：上游文档把它写作「快速定位」，Whews 的同源频道
        // `early_est` 走的是告警链路。同一场地震在两条聚合链路里必须落同一种 kind ——
        // 否则一端进列表、另一端进 HUD，用户会看到同一次地震出现两次。故跟随 Whews 取 Live。
        {QStringLiteral("early-est"), QStringLiteral("Early-est"), QStringLiteral("early_est"),
         SourceEventKind::Live},

        // ── 中国 ──────────────────────────────────────────────────
        // cenc 用上游 id 原文（去掉 _M/_A 后缀），与 Wolfx cenc_eqlist / Whews cenc 对齐。
        {QStringLiteral("cenc"), QStringLiteral("CENC"), QString(), SourceEventKind::Directory},
        {QStringLiteral("ningxia"), QString::fromUtf8("宁夏地震局"), QStringLiteral("ningxia"),
         SourceEventKind::Directory},
        {QStringLiteral("yunnan"), QString::fromUtf8("云南地震局"), QStringLiteral("yunnan"),
         SourceEventKind::Directory},
        {QStringLiteral("shanxi"), QString::fromUtf8("山西地震局"), QStringLiteral("shanxi"),
         SourceEventKind::Directory},
        {QStringLiteral("beijing"), QString::fromUtf8("北京地震局"), QStringLiteral("beijing"),
         SourceEventKind::Directory},

        // ── 东亚 / 东南亚 ─────────────────────────────────────────
        {QStringLiteral("cwa"), QStringLiteral("CWA"), QStringLiteral("cwa_eqlist"), SourceEventKind::Directory},
        {QStringLiteral("jma"), QStringLiteral("JMA"), QStringLiteral("jma_eqlist"), SourceEventKind::Directory},
        {QStringLiteral("hko"), QStringLiteral("HKO"), QStringLiteral("hko_eqlist"), SourceEventKind::Directory},
        {QStringLiteral("kma"), QStringLiteral("KMA"), QStringLiteral("kma_eqlist"), SourceEventKind::Directory},
        {QStringLiteral("tmd"), QStringLiteral("TMD"), QStringLiteral("tmd"), SourceEventKind::Directory},
        {QStringLiteral("mmd"), QStringLiteral("MMD"), QStringLiteral("mmd"), SourceEventKind::Directory},
        {QStringLiteral("bmkg"), QStringLiteral("BMKG"), QStringLiteral("bmkg"), SourceEventKind::Directory},
        {QStringLiteral("phivolcs"), QStringLiteral("PHIVOLCS"), QStringLiteral("phivolcs"),
         SourceEventKind::Directory},

        // ── 大洋洲 ────────────────────────────────────────────────
        {QStringLiteral("geonet"), QStringLiteral("GeoNet"), QStringLiteral("geonet"), SourceEventKind::Directory},
        {QStringLiteral("ga"), QStringLiteral("GA"), QStringLiteral("ga"), SourceEventKind::Directory},

        // ── 美洲 ──────────────────────────────────────────────────
        {QStringLiteral("usgs"), QStringLiteral("USGS"), QStringLiteral("usgs"), SourceEventKind::Directory},
        {QStringLiteral("nrcan"), QStringLiteral("NRCan"), QStringLiteral("nrcan"), SourceEventKind::Directory},
        {QStringLiteral("usp"), QStringLiteral("USP"), QStringLiteral("usp"), SourceEventKind::Directory},
        {QStringLiteral("scsn"), QStringLiteral("SCSN"), QStringLiteral("scsn"), SourceEventKind::Directory},
        {QStringLiteral("ncedc"), QStringLiteral("NCEDC"), QStringLiteral("ncedc"), SourceEventKind::Directory},
        {QStringLiteral("csn"), QStringLiteral("CSN"), QStringLiteral("csn"), SourceEventKind::Directory},
        {QStringLiteral("ssn"), QStringLiteral("SSN"), QStringLiteral("ssn"), SourceEventKind::Directory},
        // 秘鲁 IGP：eventNs 用 `igp`（而非 `peru`）与 Whews 的同源频道对齐。
        {QStringLiteral("peru"), QStringLiteral("IGP"), QStringLiteral("igp"), SourceEventKind::Directory},
        {QStringLiteral("igepn"), QStringLiteral("IGEPN"), QStringLiteral("igepn"), SourceEventKind::Directory},

        // ── 欧洲 ──────────────────────────────────────────────────
        {QStringLiteral("emsc"), QStringLiteral("EMSC"), QStringLiteral("emsc"), SourceEventKind::Directory},
        {QStringLiteral("gfz"), QStringLiteral("GFZ"), QStringLiteral("gfz"), SourceEventKind::Directory},
        {QStringLiteral("bcsf"), QStringLiteral("BCSF"), QStringLiteral("bcsf"), SourceEventKind::Directory},
        {QStringLiteral("ingv"), QStringLiteral("INGV"), QStringLiteral("ingv"), SourceEventKind::Directory},
        {QStringLiteral("ipgp"), QStringLiteral("IPGP"), QStringLiteral("ipgp"), SourceEventKind::Directory},
        {QStringLiteral("infp"), QStringLiteral("INFP"), QStringLiteral("infp"), SourceEventKind::Directory},
        {QStringLiteral("sed"), QStringLiteral("SED"), QStringLiteral("sed"), SourceEventKind::Directory},
        {QStringLiteral("knmi"), QStringLiteral("KNMI"), QStringLiteral("knmi"), SourceEventKind::Directory},
        {QStringLiteral("noa"), QStringLiteral("NOA"), QStringLiteral("noa"), SourceEventKind::Directory},
        {QStringLiteral("ipma"), QStringLiteral("IPMA"), QStringLiteral("ipma"), SourceEventKind::Directory},
        {QStringLiteral("lmu"), QStringLiteral("LMU"), QStringLiteral("lmu"), SourceEventKind::Directory},
        {QStringLiteral("afad"), QStringLiteral("AFAD"), QStringLiteral("afad"), SourceEventKind::Directory},
        {QStringLiteral("koeri"), QStringLiteral("KOERI"), QStringLiteral("koeri"), SourceEventKind::Directory},

        // ── 其他 / 全球 ───────────────────────────────────────────
        {QStringLiteral("isc"), QStringLiteral("ISC"), QStringLiteral("isc"), SourceEventKind::Directory},
        {QStringLiteral("gsras"), QStringLiteral("GSRAS"), QStringLiteral("gsras"), SourceEventKind::Directory},
    };
    return channels;
}

} // namespace

const std::vector<JianChannel>& jianChannels() { return channelsImpl(); }

const JianChannel* jianChannelFor(const QString& type) {
    for (const auto& channel : channelsImpl())
        if (channel.type == type) return &channel;
    return nullptr;
}

QString jianTitleFor(const JianChannel& channel) {
    const QString& type = channel.type;
    // 少数频道有官方全名：HUD 标题用它们，其余退化为「<机构> 地震预警 / 地震情报」，
    // 与 Wolfx / Pancakes / Whews 的标题风格保持一致。
    if (type == QLatin1String("cea") || type == QLatin1String("cea-pr"))
        return QStringLiteral("中国地震预警网 地震预警");
    if (type == QLatin1String("jma-eew")) return QStringLiteral("JMA 紧急地震速报");
    if (type == QLatin1String("cwa-eew")) return QStringLiteral("CWA 地震预警");
    if (type == QLatin1String("kma-eew")) return QStringLiteral("KMA 地震预警");
    if (type == QLatin1String("sa")) return QStringLiteral("ShakeAlert 地震预警");
    if (type == QLatin1String("jma")) return QStringLiteral("JMA 地震情报");
    return channel.agency
        + (channel.kind == SourceEventKind::Live ? QStringLiteral(" 地震预警")
                                                 : QStringLiteral(" 地震情报"));
}

} // namespace komira
