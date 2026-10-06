#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include "core/intensity_calculator.h"

namespace komira {

/// Wolfx 协议常量。《NATIVE_PORT_SPEC》 §2。
struct WolfxProtocol {
    static const QStringList& wsUrls() {
        static const QStringList urls = {
            QStringLiteral("wss://ws-api.wolfx.jp/all_eew"),
            QStringLiteral("wss://api.wolfx.jp/all_eew"),
        };
        return urls;
    }
    static QString eqListUrl() { return QStringLiteral("https://api.wolfx.jp/cenc_eqlist.json"); }
    static QString jmaEqListUrl() { return QStringLiteral("https://api.wolfx.jp/jma_eqlist.json"); }

    static const QStringList& queries() {
        static const QStringList q = {
            QStringLiteral("query_cenceew"),
            QStringLiteral("query_sceew"),
            QStringLiteral("query_jmaeew"),
            QStringLiteral("query_cwaeew"),
            QStringLiteral("query_fjeew"),
            QStringLiteral("query_cqeew"),
        };
        return q;
    }

    static bool isEewType(const QString& type) {
        static const QStringList types = {
            QStringLiteral("cenc_eew"), QStringLiteral("sc_eew"), QStringLiteral("jma_eew"),
            QStringLiteral("cwa_eew"),  QStringLiteral("fj_eew"), QStringLiteral("cq_eew"),
        };
        return types.contains(type);
    }

    /// 报文展示名（HUD 标题）。对齐 Wolfx Open API 文档的接口名。
    static QString titleFor(const QString& type) {
        if (type == "cenc_eew") return QStringLiteral("中国地震预警网 地震预警");
        if (type == "sc_eew") return QStringLiteral("四川省地震局 地震预警");
        if (type == "jma_eew") return QStringLiteral("JMA 紧急地震速报");
        if (type == "cwa_eew") return QStringLiteral("CWA 地震预警");
        if (type == "fj_eew") return QStringLiteral("福建省地震局 地震预警");
        if (type == "cq_eew") return QStringLiteral("重庆市地震局 地震预警");
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
        return type;
    }

    /// HTTP 目录（cenc_eqlist）统一由中国地震台网发布。
    static QString directoryAgency() { return QStringLiteral("CENC"); }

    /// HTTP 目录（jma_eqlist）由日本气象厅发布。
    static QString jmaDirectoryAgency() { return QStringLiteral("JMA"); }

    static constexpr int kQueryIntervalMs = 15000;
    static constexpr int kPollIntervalMs = 90000;
    /// EEW 新鲜度窗口：连接成功后 Wolfx 会立刻回放「最近一次」EEW，
    /// 发震时刻超过此窗口的电文不算"正在发生"，只丢弃、不告警。
    static constexpr long long kEewLiveWindowMs = 30LL * 60LL * 1000LL;
};

} // namespace komira
