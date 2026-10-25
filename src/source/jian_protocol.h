#pragma once

#include <QString>
#include <vector>

#include "source/source_event_kind.h"

namespace komira {

/// Jian 数据源（api.sismotide.top，Jian Project）的频道映射。
/// 文档：https://api.sismotide.top/api/
///
/// **覆盖文档里全部地震类频道**：6 路 EEW（告警链路）+ 40 路速报/情报（只进列表与历史），
/// 共 46 路，与参考实现 `参考/flutterRhythmQuake/lib/models/jian_sources.dart` 的
/// `jianEarthquakeSources` 逐项对齐。**刻意不接**：
/// - 海啸 / 气象 / 火山（`nmefc-tsunami`、`cwa-tsunami`、`jma-tsunami`、`weather`、
///   `jma-volcano`、`usgs-volcano`、`usgs-tsunami`）：本项目没有对应数据模型
///   （《NATIVE_PORT_SPEC》§14 非目标），硬塞进 `EarthquakeEvent` 会产出「M0 的假地震」。
/// - `/cenc-ir` 烈度速报：不进 `/all`，需要第二条鉴权连接，且产物是等值线产品而非事件。
/// - `/kmoni`、`/s-net`、`/kma-station` 测站流：见 §16（独立源，免鉴权）。
///
/// eventNs 为频道化 eventId 前缀，用于与 Wolfx / Pancakes / Whews 的同名频道对齐合并
/// （互为备份）；留空表示直接使用上游 id 原文（cenc 与 Wolfx cenc_eqlist 对齐）。
/// **每项的 eventNs 与 agency 都必须与 Whews 的同源频道取同一个串**——`EarthquakeEvent::identity()`
/// 是 `<agency>|<eventId>`，两处写法不一致就不会合并成同一场地震。
struct JianChannel {
    QString type;       ///< /all 的 source key 与推送 type
    QString agency;     ///< sourceAgency（跨聚合商一致的机构缩写）
    QString eventNs;    ///< eventId 频道前缀（空 = 用上游 id 原文）
    SourceEventKind kind;
};

/// 已接入的频道集合；顺序即设置页展示顺序（先实时预警、后地震情报）。
const std::vector<JianChannel>& jianChannels();

/// 按推送 type 查频道；未接入返回 nullptr。
const JianChannel* jianChannelFor(const QString& type);

/// HUD 标题：有官方全名的频道（中国地震预警网 / JMA 紧急地震速报 / ShakeAlert…）用全名，
/// 其余退化为「<机构> 地震信息」，与 Wolfx / Pancakes / Whews 的标题风格一致。
QString jianTitleFor(const JianChannel& channel);

struct JianProtocol {
    /// 聚合通道：一条连接覆盖全部已接入频道（服务端连接数上限 3）。
    static QString wsUrl() { return QStringLiteral("wss://api.sismotide.top/all"); }
    /// 登录密钥 → 刷新令牌。
    static QString refreshUrl() { return QStringLiteral("https://auth.sismotide.top/api/refresh"); }
    /// 刷新令牌 → 访问令牌。
    static QString accessUrl() { return QStringLiteral("https://auth.sismotide.top/api/access"); }

    static QString providerName() { return QStringLiteral("Jian"); }

    /// 历史列表命令：/all 会依次推送 cenc/cwa/jma/hko 的 *list_response。
    /// **只有这四源支持历史列表**，其余 42 路只有「最新一报 + 变化推送」——所以启动时
    /// 列表里能看到的历史深度取决于这四条，不要把「只有一条」当成解析丢失。
    static QString listCommand() { return QStringLiteral("alllist"); }

    static constexpr int kHeartbeatTimeoutMs = 90 * 1000;
    static constexpr long long kEewLiveWindowMs = 30LL * 60LL * 1000LL;
};

} // namespace komira
