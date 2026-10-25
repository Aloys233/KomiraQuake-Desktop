#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include "core/intensity_calculator.h"

namespace komira {

/// Wolfx 协议常量。《NATIVE_PORT_SPEC》 §2。
///
/// 端点事实（2026-10-25 线上抓包核实，与文档有出入处以此为准）：
/// - **只有 `ws-api.wolfx.jp` 能完成握手**。文档与旧代码里的备用站
///   `wss://api.wolfx.jp/all_eew` 实测**每次都握手失败**（不是偶发），
///   故不再作为故障转移目标。
/// - 专用频道（如 `wss://ws-api.wolfx.jp/cenc_eew`）连上后**只发心跳不推数据**，
///   必须发对应的 `query_*` 才会回一帧；所以不能把专用频道当作「少发查询」的替代方案。
/// - `all_eew` 的查询**不能并发**：实测同时发 8 条只有 1 条回包，
///   按 ~1.2 s 间隔串行发则 8 条全回。故查询必须串行节流（见 `kQueryStaggerMs`）。
/// - 目录（`cenc_eqlist` / `jma_eqlist`）在 WS 上**只回「查询的那一刻」的全量**：
///   实测发完 `query_cenceqlist` 后被动等 5 分钟，服务端**一帧都没再推**（期间 md5 未变）。
///   也就是说这个端点**不是订阅式推送**，必须**周期重发查询**才能拿到更新 ——
///   否则目录会一直停在连接时那一份，比原来的 90 s HTTP 轮询还慢。
///   故目录查询按 `kDirectoryQueryIntervalMs`（30 s）单独重发，HTTP 仍保留为断线兜底。
struct WolfxProtocol {
    /// 聚合端点。**只用 ws-api 这一台**：`api.wolfx.jp` 的 WSS 入口实测不可用。
    static const QStringList& wsUrls() {
        static const QStringList urls = {
            QStringLiteral("wss://ws-api.wolfx.jp/all_eew"),
        };
        return urls;
    }

    static QString eqListUrl() { return QStringLiteral("https://api.wolfx.jp/cenc_eqlist.json"); }
    static QString jmaEqListUrl() { return QStringLiteral("https://api.wolfx.jp/jma_eqlist.json"); }

    /// 目录端点：HTTP 兜底 URL、报数机构、子源 id，以及对应的 WS 查询指令。
    struct DirectoryEndpointSpec {
        QString url;
        QString agency;
        QString subId;
        /// 发出去会立刻回一帧全量目录（历史坑：`query_cenceqlist` 只发一次就够，
        /// 但必须发 —— 只连专用频道既不回放也不推送）。
        QString query;
        bool jma;
    };

    /// 目录端点全集，按声明顺序在一个周期内串行拉取。
    static const QList<DirectoryEndpointSpec>& directoryEndpoints() {
        static const QList<DirectoryEndpointSpec> list = {
            {eqListUrl(), directoryAgency(), QStringLiteral("cenc_eqlist"),
             QStringLiteral("query_cenceqlist"), false},
            {jmaEqListUrl(), jmaDirectoryAgency(), QStringLiteral("jma_eqlist"),
             QStringLiteral("query_jmaeqlist"), true},
        };
        return list;
    }

    /// 订阅指令全集：6 条 EEW + 2 条目录。顺序即发送顺序（先 EEW，让告警链路先拿到数据）。
    static const QStringList& queries() {
        static const QStringList q = {
            QStringLiteral("query_cenceew"),
            QStringLiteral("query_sceew"),
            QStringLiteral("query_jmaeew"),
            QStringLiteral("query_cwaeew"),
            QStringLiteral("query_fjeew"),
            QStringLiteral("query_cqeew"),
            QStringLiteral("query_cenceqlist"),
            QStringLiteral("query_jmaeqlist"),
        };
        return q;
    }

    static bool isEewType(const QString& type) {
        return eewTypes().contains(type);
    }

    /// 已接入的 EEW 频道（查询报文与子数据源 id 同源）。
    static const QStringList& eewTypes() {
        static const QStringList types = {
            QStringLiteral("cenc_eew"), QStringLiteral("sc_eew"), QStringLiteral("jma_eew"),
            QStringLiteral("cwa_eew"),  QStringLiteral("fj_eew"), QStringLiteral("cq_eew"),
        };
        return types;
    }

    /// 已接入的目录频道（WS 帧 `type` 与子源 id 同源）。
    static const QStringList& directoryTypes() {
        static const QStringList types = {
            QStringLiteral("cenc_eqlist"), QStringLiteral("jma_eqlist"),
        };
        return types;
    }

    /// 该 WS 帧类型属于目录（整份列表帧），而不是单条 EEW。
    static bool isDirectoryType(const QString& type) { return directoryTypes().contains(type); }

    /// 订阅报文 → 频道 id；未知报文返回空串（发送端应跳过）。
    static QString typeForQuery(const QString& query) {
        if (query == "query_cenceew") return QStringLiteral("cenc_eew");
        if (query == "query_sceew") return QStringLiteral("sc_eew");
        if (query == "query_jmaeew") return QStringLiteral("jma_eew");
        if (query == "query_cwaeew") return QStringLiteral("cwa_eew");
        if (query == "query_fjeew") return QStringLiteral("fj_eew");
        if (query == "query_cqeew") return QStringLiteral("cq_eew");
        if (query == "query_cenceqlist") return QStringLiteral("cenc_eqlist");
        if (query == "query_jmaeqlist") return QStringLiteral("jma_eqlist");
        return QString();
    }

    /// 报文展示名（HUD 标题）。对齐 Wolfx Open API 文档的接口名。
    static QString titleFor(const QString& type) {
        if (type == "cenc_eew") return QStringLiteral("中国地震预警网 地震预警");
        if (type == "sc_eew") return QStringLiteral("四川省地震局 地震预警");
        if (type == "jma_eew") return QStringLiteral("JMA 紧急地震速报");
        if (type == "cwa_eew") return QStringLiteral("CWA 地震预警");
        if (type == "fj_eew") return QStringLiteral("福建省地震局 地震预警");
        if (type == "cq_eew") return QStringLiteral("重庆市地震局 地震预警");
        if (type == "cenc_eqlist") return QStringLiteral("中国地震台网 地震信息");
        if (type == "jma_eqlist") return QStringLiteral("JMA 地震情报");
        return type;
    }

    /// 数据源提供方，列表卡片左下角以 `<provider>·<agency>` 标明来源。
    static QString providerName() { return QStringLiteral("Wolfx"); }

    /// 报数机构缩写。EEW 的 cenc_eew 是中国地震局（CEA，中国地震预警网）；
    /// cenc_eqlist 才是中国地震台网（CENC）——两者不可混为一谈。
    static QString agencyFor(const QString& type) {
        if (type == "cenc_eew") return QStringLiteral("CEA");
        if (type == "sc_eew") return QStringLiteral("SC");
        if (type == "jma_eew") return QStringLiteral("JMA");
        if (type == "cwa_eew") return QStringLiteral("CWA");
        if (type == "fj_eew") return QStringLiteral("FJ");
        if (type == "cq_eew") return QStringLiteral("CQ");
        if (type == "cenc_eqlist") return directoryAgency();
        if (type == "jma_eqlist") return jmaDirectoryAgency();
        return type;
    }

    /// HTTP 目录（cenc_eqlist）统一由中国地震台网发布。
    static QString directoryAgency() { return QStringLiteral("CENC"); }

    /// HTTP 目录（jma_eqlist）由日本气象厅发布。
    static QString jmaDirectoryAgency() { return QStringLiteral("JMA"); }

    /// EEW 订阅的重复间隔。
    static constexpr int kQueryIntervalMs = 15000;
    /// 同轮内两条查询之间的间隔。**不可并发**：实测并发发送时上游只回第一条，
    /// 其余被丢弃（见结构体注释）。8 条 × 1.2 s ≈ 10 s，仍在 15 s 轮内。
    static constexpr int kQueryStaggerMs = 1200;
    /// 目录查询的重复间隔。**必须周期重发**：服务端不会主动推目录变化
    /// （实测被动等 5 分钟一帧未推），不重发就等于目录冻结在连接那一刻。
    /// 30 s 是本项目对目录实时性的口径，也把两路目录的带宽压到 ~48 KB/min。
    static constexpr int kDirectoryQueryIntervalMs = 30000;
    /// 目录 HTTP 兜底轮询间隔。WS 目录为主链路，这里只在 WS 长时间静默时补拉
    /// （判据见 `kDirectoryPushFreshMs`）。
    static constexpr int kPollIntervalMs = 300000;
    /// WS 目录帧的新鲜度窗口：窗口内收到过 WS 目录帧就跳过 HTTP 兜底。
    /// 取 2 分钟（> 30 s 重发周期），既能容忍偶发丢帧，又能在 WS 静默 ~2 min 后
    /// 果断回落到 HTTP。
    static constexpr long long kDirectoryPushFreshMs = 2LL * 60LL * 1000LL;
    /// EEW 新鲜度窗口：连接成功后 Wolfx 会立刻回放「最近一次」EEW，
    /// 发震时刻超过此窗口的电文不算"正在发生"，只丢弃、不告警。
    static constexpr long long kEewLiveWindowMs = 30LL * 60LL * 1000LL;
};

} // namespace komira
