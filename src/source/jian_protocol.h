#pragma once

#include <QString>
#include <vector>

#include "source/source_event_kind.h"

namespace komira {

/// Jian 数据源（api.sismotide.top，Jian Project）的频道映射。
/// 文档：https://api.sismotide.top/api/
///
/// 只接入「中国 + 日本 + 台湾」地震类频道；EEW 走告警链路，地震速报/目录只进列表。
/// eventNs 为频道化 eventId 前缀，用于与 Wolfx / Pancakes 的同名频道对齐合并（互为备份）；
/// 留空表示直接使用上游 id 原文（cenc 与 Wolfx cenc_eqlist 对齐）。
struct JianChannel {
    QString type;       ///< /all 的 source key 与推送 type
    QString agency;     ///< sourceAgency
    QString eventNs;    ///< eventId 频道前缀（空 = 用上游 id 原文）
    SourceEventKind kind;
};

/// 已接入的频道集合；顺序即状态展示顺序。
const std::vector<JianChannel>& jianChannels();

/// 按推送 type 查频道；未接入返回 nullptr。
const JianChannel* jianChannelFor(const QString& type);

struct JianProtocol {
    /// 聚合通道：一条连接覆盖全部已接入频道（服务端连接数上限 3）。
    static QString wsUrl() { return QStringLiteral("wss://api.sismotide.top/all"); }
    /// 登录密钥 → 刷新令牌。
    static QString refreshUrl() { return QStringLiteral("https://auth.sismotide.top/api/refresh"); }
    /// 刷新令牌 → 访问令牌。
    static QString accessUrl() { return QStringLiteral("https://auth.sismotide.top/api/access"); }

    static QString providerName() { return QStringLiteral("Jian"); }

    /// 历史列表命令：/all 会依次推送 cenc/cwa/jma/hko 的 *list_response。
    static QString listCommand() { return QStringLiteral("alllist"); }

    static constexpr int kHeartbeatTimeoutMs = 90 * 1000;
    static constexpr long long kEewLiveWindowMs = 30LL * 60LL * 1000LL;
};

} // namespace komira
