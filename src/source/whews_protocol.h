#pragma once

#include <QString>
#include <QStringList>
#include <vector>

#include "source/source_event_kind.h"

namespace komira {

/// Whews 报文时刻的时区：JMA 系端点为 UTC+9，其余为 UTC+8（报文时刻均无时区）。
enum class WhewsTimeZone { Utc8, Jst };

/// Whews 数据源（备用站国内 api.2v8.cn / 主站 api.beecld.com）的频道映射。
/// 文档：https://api.2v8.cn/docs
///
/// 单条 `/ws/all` 聚合连接覆盖下列全部频道；帧格式 `{"Data":{…},"md5":…,"source":"<短名>"}`，
/// 首连为 JSON 数组（各源最新一条），之后为单条对象。
/// eventNs 为频道化 eventId 前缀，用于与 Wolfx / Pancakes / Jian 的同名频道对齐合并；
/// 留空表示直接使用上游 id 原文（cenc 与 Wolfx cenc_eqlist 对齐）。
struct WhewsChannel {
    QString source;    ///< /ws/all 帧内的 source 短名
    QString agency;    ///< sourceAgency
    QString eventNs;   ///< eventId 频道前缀（空 = 用上游 id 原文）
    SourceEventKind kind;
    WhewsTimeZone tz;
};

/// 已接入的频道集合；顺序即状态展示顺序。
const std::vector<WhewsChannel>& whewsChannels();

/// 按帧内 source 短名查频道；未接入返回 nullptr。
const WhewsChannel* whewsChannelFor(const QString& source);

/// 报文展示名（HUD 标题）：按机构 + 预警/情报区分，对齐 Wolfx / Pancakes 的标题风格。
QString whewsTitleFor(const WhewsChannel& channel);

struct WhewsProtocol {
    /// 站点地址，**备用站（国内）在前**：优先直连国内站，不可用时才轮换到主站。
    /// 索引只在连接失败时前进，连通后不回落（见 WhewsSource::urlIndex_）。
    static QStringList wsHosts() {
        return {QStringLiteral("wss://api.2v8.cn"), QStringLiteral("wss://api.beecld.com")};
    }

    /// 聚合端点路径；令牌以 `?token=wat_…` 附加在握手。
    static QString wsPath() { return QStringLiteral("/ws/all"); }

    static QString providerName() { return QStringLiteral("Whews"); }

    static constexpr int kHeartbeatTimeoutMs = 90 * 1000;
    static constexpr long long kEewLiveWindowMs = 30LL * 60LL * 1000LL;

    /// 服务端关闭码（握手升级成功后）。文档要求 `4401` 令牌无效、`4403` 被封禁时
    /// **停止盲目重连**（重连过频会被智能封禁）；`4503` 鉴权服务不可用、`4008`
    /// 单令牌连接数超限则退避后重连。
    struct CloseCode {
        static constexpr int kUnauthorized = 4401;
        static constexpr int kBanned = 4403;
    };
};

} // namespace komira