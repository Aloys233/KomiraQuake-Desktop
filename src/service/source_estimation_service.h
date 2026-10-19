#pragma once

#include <QFutureWatcher>
#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <array>
#include <functional>
#include <vector>

#include "core/source_estimator.h"
#include "model/station_snapshot.h"

namespace komira {

class SettingsStore;

/// 桌面端「本地震源推算（Beta）」。
///
/// **只吃实时测站显示流的强度值**（NIED Kmoni / S-Net / PancakesSeis / WolfxSeisJS /
/// KMA PEWS），不依赖任何 EEW 事件，也不进入数据源注册表、不产生 `EarthquakeEvent`、
/// 不参与告警/去重/历史 —— 与 §16 的测站显示同一条链路，只是把「每站当前强度」再往
/// 前推一步，用强度场反推震源。
///
/// 流程：
///   1. 1 Hz 合并一帧测站观测（AppController 在重建 StationSnapshot 时投喂）；
///   2. 逐站按坐标/制式归入**四个区域**之一（中国 / 日本 / 韩国 / 台湾）；
///   3. 每个区域用自己的参数（触发阈值、发布门槛、合并半径、反证开关）独立反演，
///      区域内仍按烈度制式分桶（不同制式的数值不能混算）；
///   4. 每区各自发布自己的解，证据消散超过保持时长后清空 —— 区域之间互不覆盖。
///
/// 计算跑在 worker 线程（QtConcurrent），主线程只做 1 Hz 的簿记与信号发射。
class SourceEstimationService : public QObject {
    Q_OBJECT
public:
    explicit SourceEstimationService(SettingsStore* settings, QObject* parent = nullptr);
    ~SourceEstimationService() override;

    /// 时间基准（与倒计时/告警同一只校时时钟）。
    void setNowProvider(std::function<long long()> provider);
    /// 注入一帧测站观测（容器的 entries 顺序与来源无关，内部按制式归类）。
    void ingestObservations(const std::vector<StationSnapshot::Entry>& entries);
    /// 设置变更：重置触发簿记，让新阈值立即生效。
    void reloadSettings();
    /// 清空全部状态（测站源全部关闭 / 应用退出）。
    void reset();

    /// 卡片载荷（见 .cpp 顶部字段说明）；无数据时 `state == "idle"`。
    ///
    /// 顶层字段是**汇总**（兼容旧调用：`state` / `visible` / `triggeredStations` 等取
    /// 「证据最强的一区」），逐区明细在 `regions` 数组里，顺序固定为中国 / 日本 / 韩国 / 台湾。
    QVariantMap info() const;
    /// 地图叠加层载荷。四个区域各自一个标记，`markers` 为数组；顶层保留最强的那个
    /// （兼容只读单标记的旧调用）。
    QVariantMap marker() const;

signals:
    /// 载荷有任何变化（结果、状态、每秒的计时字段）。
    void changed();

private:
    /// 单站触发簿记。日本区要拿「首次越阈时刻」做 P 波到时反演，其余区用它反推发震时刻。
    /// 两者共用同一份记录，避免两套时间戳各偏几毫秒。
    struct StationState {
        long long firstTriggerMs = 0;
        long long lastTriggerMs = 0;
        long long lastSeenMs = 0;
        double lastIntensity = -999.0;
        /// 测站坐标。强度路径要靠「首次越阈时刻 − P 走时」反推发震时刻，必须知道
        /// 每个站离推定震中多远；`stationKey()` 是打包后的整数，解不出坐标。
        double latitude = 0.0;
        double longitude = 0.0;
    };

    /// 单个区域的运行态。区域之间**完全独立**：参数、触发簿记、首次触发时刻、解与
    /// 保持窗口都各算各的，互不覆盖 —— 日本的一场地震不会挤掉中国区的解。
    struct RegionState {
        SourceRegion region = SourceRegion::China;
        /// 该区主导制式（决定设置里触发阈值的口径与卡片的大号数字）。
        IntensityFamily family = IntensityFamily::Csis;
        // 设置快照。
        double trigger = 3.0;
        int minStations = 6;
        bool quietEvidence = true;
        int clusterRadiusKm = 250;
        /// 该区是否启用（逐区开关）。关闭时整区跳过：不记账、不算、不发布。
        /// 默认 **false**，与设置层一致（未显式启用就不参与）。
        bool enabled = false;
        // 触发簿记。
        QHash<qint64, StationState> states;
        int triggeredCount = 0;
        // 发布态。
        SourceEstimate estimate;
        QString state = QStringLiteral("idle");
        long long sessionStartedMs = 0;
        long long lastActiveMs = 0;
        long long updatedMs = 0;
        /// 第几报（1 基，首报 = 1）。对齐参考实现的 `cluster.reportNum`：
        /// **解变了才算新的一报**，不是每秒都加。
        ///
        /// 参考的 `sameHypocenterThreshold` 是 `1e-9`（近乎逐位相等），因为它只在测站
        /// pick 变化时才重算；本服务 1 Hz 持续重算，用逐位相等会让报次每秒跳一次。
        /// 故判据改为**用户看得见的量**：坐标 / 深度 / 震级 / 等级任一变到显示精度以外
        /// 才算新的一报（阈值见 kReport* 常量）。
        int reportNum = 0;
        /// 上一次记入报次的解，仅供 `applyBatch` 判断「变了没有」。
        SourceEstimate reportBase;
        /// 反推出来的发震时刻（epoch ms，0 = 未知）。日本区到时路径直接给；
        /// 其余区在 `applyBatch` 里用「触发站首次越阈时刻 − P 走时」的中位数反推。
        long long originEpochMs = 0;
    };

    struct EstimateBatch {
        /// 与 `sourceRegions()` 同序。
        std::array<SourceEstimate, 4> estimates;
        /// 日本区的到时解（含发震时刻、P/S 相位数）。其余区域留空。
        ArrivalEstimate arrival;
    };

    void tick();
    void runEstimation();
    void onEstimated();
    void applyBatch(const EstimateBatch& batch);
    /// 读取设置；与缓存不同则返回 true 并更新缓存。
    bool syncConfig();
    /// 测站稳定键：坐标量化到 3 位小数（约 110 m）+ 制式，打包进一个 64 位整数。
    ///
    /// 各测站源没有统一台站 ID，只能按坐标认站；同一物理测站在相邻帧里坐标不会跳。
    /// 用整数而不是 QString 是**性能要求**：本函数每帧对全部测站（约 2500）调用一次，
    /// 10 Hz 下就是每秒 2.5 万次；QString 拼装 + 哈希在此量级上是可测的浪费。
    static qint64 stationKey(IntensityFamily family, double latitude, double longitude);
    /// 反推发震时刻：取「触发站首次越阈时刻 − 该站 P 走时」的中位数。
    ///
    /// 这是一条**有意保守**的估计：首次越阈比真正的 P 到时要晚（要等强度爬过阈值），
    /// 所以反推出来的时刻通常略晚于真实发震时刻，误差量级 = 各站爬升时间差（通常 1–4 s）。
    /// 取中位数而非均值：个别站晚了几十秒（阈值高、离得远）不该把整条拉走。
    /// 没有任何触发站时返回 0，卡片据此隐藏时刻行。
    long long estimateOriginMs(const RegionState& region, const SourceEstimate& estimate) const;
    /// 报次判据：两个解在**显示精度**上是否算同一报（见 RegionState::reportNum 的说明）。
    static bool sameReport(const SourceEstimate& a, const SourceEstimate& b);
    /// 区域名（参考实现的 `getFEName`）。表未加载时退回坐标文本。
    static QString regionNameFor(double latitude, double longitude);
    void clearRegion(RegionState& region, const QString& state);
    /// 重算整张载荷（汇总 + 逐区明细 + 标记）并（若有变化）发 changed。
    void publish();
    static IntensityFamily familyOf(const StationSnapshot::Entry& entry);
    RegionState& regionState(SourceRegion region);
    const RegionState& regionState(SourceRegion region) const;

    SettingsStore* settings_ = nullptr;
    std::function<long long()> nowProvider_;
    QTimer ticker_;
    QFutureWatcher<EstimateBatch> watcher_;
    bool pending_ = false;

    std::vector<StationSnapshot::Entry> latestEntries_;
    long long latestEntriesMs_ = 0;
    long long lastRunMs_ = 0;

    /// 四个区域的运行态，顺序与 `sourceRegions()` 一致。
    std::array<RegionState, 4> regions_;
    /// 上一次算出的 Batch 里的强度观测，供「本轮无需重算」时复用（未使用）。
    QVariantMap payload_;
    QVariantMap marker_;

    // 全局设置快照（变化即重算触发簿记）。
    bool enabled_ = true;
    int holdSeconds_ = 60;

    /// 同一测站回落再触发，间隔超过它才视为新的触发事件。
    static constexpr long long kRetriggerGapMs = 45'000;
    /// 重新估算的最小间隔：测站帧 1 Hz，估算本身几毫秒到几十毫秒。
    static constexpr long long kMinRunIntervalMs = 1200;
    /// 状态簿记的清扫阈值。
    static constexpr long long kStateTtlMs = 5LL * 60LL * 1000LL;
    /// 报次判据的显示精度：坐标 0.001°（卡片就印 3 位小数）、深度 1 km
    /// （卡片印整数）、震级 0.1（卡片印 1 位小数）。落在精度以内的抖动不算新报，
    /// 否则 1 Hz 重算会让「第 N 报」每秒跳一次。
    static constexpr double kReportLocationDeg = 1e-3;
    static constexpr double kReportDepthKm = 1.0;
    static constexpr double kReportMagnitude = 0.1;
};

} // namespace komira
