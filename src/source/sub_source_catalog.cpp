#include "source/sub_source_catalog.h"

#include <QHash>

#include <utility>

#include "model/data_source_info.h"
#include "source/jian_protocol.h"
#include "source/pancakes_protocol.h"
#include "source/whews_protocol.h"
#include "source/wolfx_protocol.h"

namespace komira {

namespace {

QString channelName(const QString& agency, SourceEventKind kind) {
    return agency
        + (kind == SourceEventKind::Live ? QStringLiteral(" 地震预警")
                                         : QStringLiteral(" 地震情报"));
}

/// 同一聚合源内可能出现同名子源（Whews 的 cea / cea-pr 都是 CEA 预警）。
/// 重名时补上子源 id，避免设置页出现两行一模一样的开关。
QVector<SubSourceInfo> disambiguate(QVector<SubSourceInfo> items) {
    QHash<QString, int> counts;
    for (const SubSourceInfo& item : std::as_const(items)) ++counts[item.name];
    for (SubSourceInfo& item : items) {
        if (counts.value(item.name) > 1) item.name += QStringLiteral(" · ") + item.id;
    }
    return items;
}

QHash<QString, QVector<SubSourceInfo>> buildCatalog() {
    QHash<QString, QVector<SubSourceInfo>> catalog;

    QVector<SubSourceInfo> wolfx;
    for (const QString& type : WolfxProtocol::eewTypes()) {
        wolfx.push_back({type, channelName(WolfxProtocol::agencyFor(type), SourceEventKind::Live),
                         SourceEventKind::Live});
    }
    wolfx.push_back({QStringLiteral("cenc_eqlist"),
                     channelName(WolfxProtocol::directoryAgency(), SourceEventKind::Directory),
                     SourceEventKind::Directory});
    wolfx.push_back({QStringLiteral("jma_eqlist"),
                     channelName(WolfxProtocol::jmaDirectoryAgency(), SourceEventKind::Directory),
                     SourceEventKind::Directory});
    catalog.insert(SourceIds::kWolfx, wolfx);

    QVector<SubSourceInfo> pancakes;
    for (const QString& source : PancakesProtocol::quakeSources()) {
        const SourceEventKind kind = source == PancakesProtocol::sourceJmaEqlist()
            ? SourceEventKind::Directory : SourceEventKind::Live;
        pancakes.push_back({source, PancakesProtocol::titleFor(source), kind});
    }
    catalog.insert(SourceIds::kPancakes, pancakes);

    QVector<SubSourceInfo> jian;
    for (const JianChannel& channel : jianChannels()) {
        // 与 HUD 标题同源：设置页的开关名与事件卡上的来源名必须是同一句话，
        // 否则用户勾了「中国地震预警网 地震预警」却在列表里看到「CEA 地震预警」。
        jian.push_back({channel.type, jianTitleFor(channel), channel.kind});
    }
    catalog.insert(SourceIds::kJian, disambiguate(jian));

    QVector<SubSourceInfo> whews;
    for (const WhewsChannel& channel : whewsChannels()) {
        whews.push_back({channel.source, whewsTitleFor(channel), channel.kind});
    }
    catalog.insert(SourceIds::kWhews, disambiguate(whews));

    return catalog;
}

} // namespace

const QVector<SubSourceInfo>& subSourcesFor(const QString& sourceId) {
    static const QHash<QString, QVector<SubSourceInfo>> catalog = buildCatalog();
    static const QVector<SubSourceInfo> empty;
    const auto it = catalog.constFind(sourceId);
    return it == catalog.constEnd() ? empty : it.value();
}

} // namespace komira
