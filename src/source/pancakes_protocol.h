#pragma once

#include <QString>
#include <QStringList>

namespace komira {

/// PancakesAPI 协议常量。
///
/// 统一主域 `api.aloys23.link`：聚合通道 `/api/v1/alert/ws/all` 透传全部突发事件，外层统一为
/// `{source, type, action, timestampMs, payload}`；本项目只接入地震类子源
/// （gq / usgs / jma_eew / jma_eqlist），其余气象、海洋与火山事件一律忽略。
/// 列表/历史另走各子源的 HTTP 接口 `GET /api/v1/alert/quake/{source}`。
struct PancakesProtocol {
    static QString wsUrl() { return QStringLiteral("wss://api.aloys23.link/api/v1/alert/ws/all"); }
    static QString baseUrl() { return QStringLiteral("https://api.aloys23.link"); }

    static QString sourceGq() { return QStringLiteral("gq"); }
    static QString sourceUsgs() { return QStringLiteral("usgs"); }
    static QString sourceJmaEew() { return QStringLiteral("jma_eew"); }
    static QString sourceJmaEqlist() { return QStringLiteral("jma_eqlist"); }

    static const QStringList& quakeSources() {
        static const QStringList sources = {
            QStringLiteral("gq"), QStringLiteral("usgs"),
            QStringLiteral("jma_eew"), QStringLiteral("jma_eqlist"),
        };
        return sources;
    }

    static QString listUrl(const QString& source) {
        return baseUrl() + QStringLiteral("/api/v1/alert/quake/") + source;
    }

    /// 数据源提供方，列表卡片以 `<provider>·<agency>` 标明来源。
    static QString providerName() { return QStringLiteral("Pancakes"); }

    /// 报数机构缩写。JMA 的 EEW 与速报同属气象厅，合并展示为 JMA。
    static QString agencyFor(const QString& source) {
        if (source == "gq") return QStringLiteral("GQ");
        if (source == "usgs") return QStringLiteral("USGS");
        if (source == "jma_eew") return QStringLiteral("JMA");
        if (source == "jma_eqlist") return QStringLiteral("JMA");
        return source.toUpper();
    }

    /// 报文展示名（HUD 标题）。
    static QString titleFor(const QString& source) {
        if (source == "gq") return QStringLiteral("GlobalQuake地震信息");
        if (source == "usgs") return QStringLiteral("USGS 地震信息");
        if (source == "jma_eew") return QStringLiteral("JMA 紧急地震速报");
        if (source == "jma_eqlist") return QStringLiteral("JMA 地震情报");
        return source;
    }

    /// 实时告警新鲜度窗口：上游连接建立后不回放历史，此窗口仅用于防御迟到/重放的报文。
    static constexpr long long kLiveWindowMs = 30LL * 60LL * 1000LL;

    static constexpr int kPollIntervalMs = 90'000;

    /// 取消报没有递增报数，用哨兵报数保证能覆盖旧报次并终止生命周期。
    static constexpr int kCancelReportNum = 999'999;
};

} // namespace komira
