#pragma once

#include <array>
#include <string>
#include <vector>

namespace komira {

/// 测站烈度制式。本地震源推算**在同一制式内**做拟合：不同源（JMA 計測震度 /
/// 中国 CSIS / 韩国 MMI）的数值不可混用，否则一次地动的强度梯度会被制式差淹没。
enum class IntensityFamily { Csis, Kma, Jma };

/// 制式 id（`csis` / `kma` / `jma`），与 QML 的色阶选择一致。
const char* intensityFamilyId(IntensityFamily family);
/// 制式展示名（`中国烈度` / `韩国 MMI` / `計測震度`）。
const char* intensityFamilyLabel(IntensityFamily family);

/// 推算区域。按**台网密度与制式**拆开，每区一套独立参数（触发阈值、发布门槛、
/// 合并半径、反证开关），解也各自发布、互不平均：
///
///   · 中国：PancakesSeis / Wolfx SeisJS 的 CSIS 台网，最稀疏；
///   · 日本：NIED Kmoni + S-Net，上千站、JMA 計測震度；
///   · 韩国：KMA PEWS，约 260 站、MMI；
///   · 台湾：目前只有 Wolfx SeisJS 个别站（CWA 台网尚未接入），单独成区，
///     这样接入前后都不会被大陆台网的参数拖住。
///
/// 区域只决定「用哪套参数、结果算谁的」，不改变拟合本身：同一区域内仍按制式分桶。
enum class SourceRegion { China, Japan, Korea, Taiwan };

/// 全部区域，顺序即 UI 顺序（中国 / 日本 / 韩国 / 台湾）。
const std::array<SourceRegion, 4>& sourceRegions();
/// 区域 id（`china` / `japan` / `korea` / `taiwan`），用于设置键与 QML。
const char* sourceRegionId(SourceRegion region);
/// 区域展示名（`中国` / `日本` / `韩国` / `台湾`）。
const char* sourceRegionLabel(SourceRegion region);
/// 区域主导强度制式：设置里该区的「触发烈度」按这个口径填写。
IntensityFamily sourceRegionFamily(SourceRegion region);
/// id → 区域；未知 id 返回 false。
bool sourceRegionFromId(const std::string& id, SourceRegion* out);
bool sourceRegionFromId(const char* id, SourceRegion* out);

/// 区域默认参数。按台网密度给：日本最密（可等更多站）、韩国次之、中国最疏、
/// 台湾目前只有零星测站故门槛最低（1 站即可给「单站粗解」，卡片会明确标注）。
struct SourceRegionDefaults {
    double trigger = 3.0;        ///< 该区主导制式下的触发烈度
    int minStations = 6;         ///< 发布门槛（触发站数）
    int clusterRadiusKm = 250;   ///< 触发区合并半径
    bool quietEvidence = true;   ///< 用未触发站作删失观测
};
const SourceRegionDefaults& sourceRegionDefaults(SourceRegion region);

/// 把某一制式的强度值折算成「中国烈度等效值」。
/// 用途只有一个：区域触发阈值按各自制式填写，跨区比较/给别的制式换算时走这条桥。
double csisEquivalent(IntensityFamily family, double value);
/// 反向：给定中国烈度等效值，求该制式的触发阈值（保留 1 位小数）。
double intensityFromCsis(IntensityFamily family, double csis);

/// 测站坐标 → 推算区域。`family` 只作为来源提示参与判定（KMA 一定在韩国、
/// Kmoni/S-Net 的 JMA 值一定在日本），台湾框优先于「JMA = 日本」，
/// 这样将来接入 CWA（震度制式与 JMA 同形）也不会被划进日本区。
/// 不属于任何区域返回 false（不参与推算，但仍可在测站层显示）。
bool sourceRegionForStation(IntensityFamily family, double latitude, double longitude,
                            SourceRegion* out);

/// 一条测站强度观测。坐标 WGS-84 度，强度为所属制式的原生值。
struct StationObservation {
    double latitude = 0.0;
    double longitude = 0.0;
    double intensity = 0.0;
    IntensityFamily family = IntensityFamily::Csis;
};

/// 推算参数。默认值对应「中国区域默认灵敏度」：触发烈度 3.0、最少 6 站发布。
/// 实际使用时由 `SourceEstimationService` 按区域填入（见 `sourceRegionDefaults`）。
struct SourceEstimatorConfig {
    IntensityFamily family = IntensityFamily::Csis;
    /// 所属推算区域。只写入结果供 UI 归行，不参与拟合。
    SourceRegion region = SourceRegion::China;
    /// 触发阈值：强度 ≥ 该值的测站进入正证据；低于它的测站作为**删失观测**（尚未触发）
    /// 参与负证据，前提是它们离候选震源足够近。
    double triggerLevel = 3.0;
    /// 发布门槛：触发测站数低于它时认为还不足以给出震源。可设到 1。
    int minStations = 6;
    /// 参与衰减拟合的最少触发站数。低于它的簇走「弱证据」路径（见 provisional 说明），
    /// 而不是直接放弃 —— 这条路径只在发布门槛被下调到 1–2 时才会被用到。
    int minSearchStations = 4;
    /// 触发站抽稀上限（按网格取每格最强站）；只影响搜索代价，不影响 stationCount。
    int maxStations = 96;
    /// 负证据（未触发站）抽稀上限。
    int maxQuietStations = 48;
    /// 是否使用未触发站作为删失观测。
    bool useQuietStations = true;
    /// Huber 残差阈值，单位与强度相同。
    double robustDelta = 1.0;
    /// 负证据权重（相对正证据均值残差）。
    double quietWeight = 0.6;
    /// 负证据留出的余量：预测强度必须超过「触发阈值 + 余量」才算矛盾。
    double quietMargin = 1.0;
    /// 负证据只取候选震源该半径内的测站（更远的站预测值必然很低，没有信息量）。
    double quietRadiusKm = 400.0;
    /// 地理分离半径（km）：触发站之间超过这个距离就不会被并进同一个震源候选。
    ///
    /// 同时发生两场地震时（例如四川与云南各一次），把两簇强度塞进同一个拟合，最优解会落在
    /// 两簇之间 —— 那是一个**不存在**的震中，而且残差可能仍然很小。分簇是保证「发布出来的
    /// 震源对应一场真实的地动」的前提。
    /// 取 250 km。单链接聚类只连接「彼此相距不超过半径」的触发站，因此：
    ///   · 一场地震的触发区是**连成一片**的（测站间距通常 20–50 km，即使稀疏台网也只有
    ///     百余公里），整片会被串成同一簇；
    ///   · 两场独立地震之间通常隔着上百公里**尚未触发**的测站，它们不参与聚类，
    ///     所以两片触发区不会串在一起。
    /// 半径再放大到几百公里就会开始把两场地震串成一簇（那正是要避免的）。
    double clusterRadiusKm = 250.0;
    /// 深度候选。只做**离散**深度：单凭强度场无法稳定反演连续深度，硬扫会过拟合。
    std::vector<double> depthCandidatesKm = {10.0, 30.0, 60.0, 100.0};
    /// 深度先验权重：证据不足时偏向浅源，避免搜索把深度推到边界。
    double depthPriorWeight = 0.02;
    /// 搜索框在触发站外包络外扩的度数。
    double searchPaddingDeg = 0.6;
    /// 搜索半径上下限（度）。触发站可能横跨上千公里，必须限幅。
    double minSearchRadiusDeg = 0.25;
    double maxSearchRadiusDeg = 6.0;
    /// 极坐标粗扫：环数、环向步长（度）、环向点数上限。
    int scanRings = 14;
    double scanStepDeg = 0.15;
    int maxScanPoints = 1400;
    /// 模式搜索（pattern search）的收敛下限与最大轮数。
    double refineFloorDeg = 0.004;
    int maxRefineSteps = 24;
    /// 多解迟滞：把「上一次发布的解」作为偏好位置，给附近的簇一个有限的加成。
    ///
    /// 同一地区同时出现两片独立触发区时（例如四川主震 + 云南一场小震），两簇都能过发布
    /// 门槛、置信度相差不到 0.01。此时**逐帧噪声**就足以让「最强簇」每次换人：地图上
    /// 震中在几百公里之间每秒来回跳，服务层再把每次跳变当成新事件把报次清零 ——
    /// 用户看到的就是「发了很多报，然后震中跳走，报次回到第 1 报并一直停在 1」。
    ///
    /// `preferRadiusKm <= 0`（默认）表示没有偏好，`estimate()` 保持纯函数语义
    /// （单测与一次性调用都是这种情形）。服务层每次重算时传入上一帧的解。
    double preferLatitude = 0.0;
    double preferLongitude = 0.0;
    double preferRadiusKm = 0.0;
    /// 迟滞加成（置信度单位）。0.15 大于逐帧噪声造成的差异（实测 0.01–0.03），
    /// 又小于相邻两个等级的门槛差（A→B 是 0.16），所以不会把明显更好的新簇锁死。
    double preferBonus = 0.15;
};

/// 推算结果。`valid == false` 时其余字段无意义。
struct SourceEstimate {
    bool valid = false;
    IntensityFamily family = IntensityFamily::Csis;
    SourceRegion region = SourceRegion::China;
    double latitude = 0.0;
    double longitude = 0.0;
    double depthKm = 10.0;
    /// 由强度场反演的等效震级（同一制式的衰减关系）；证据不足时可能偏低。
    double magnitude = 0.0;
    /// 触发测站总数（抽稀前）。
    int stationCount = 0;
    /// 实际参与搜索的触发站数（抽稀后）。
    int usedStationCount = 0;
    /// 参与负证据的未触发站数。
    int quietCount = 0;
    /// 触发站在最佳解下的强度残差 RMS（同制式单位）。
    double rmsIntensity = 0.0;
    /// 以最佳解为中心、触发站的方位角最大空隙（度）。360 表示只有一站。
    double azimuthGapDeg = 180.0;
    /// 最近触发站到震中的震中距（km）。
    double nearestStationKm = 0.0;
    /// 触发站平均震中距（km）。
    double meanDistanceKm = 0.0;
    /// 水平不确定度（km，启发式，非概率校准值）。
    double uncertaintyKm = 0.0;
    /// 置信度 0–1（启发式，不解释为概率）。
    double confidence = 0.0;
    /// 质量等级 S/A/B/C/D。
    std::string grade = "D";
    /// 最佳解是否贴住搜索边界（贴边通常意味着证据只覆盖一侧）。
    bool boundaryHit = false;
    /// 弱证据解：只有 1–2 个触发站时，强度场没有梯度，无法约束震源。
    /// 此时只给出「触发站附近」的粗略位置 + 很大的不确定度，并置本标志，
    /// 由 UI 明确标注为单站/双站推定，避免被当成与多点解同等的结论。
    bool provisional = false;
    /// 触发站被划分成的地理簇数量（含未达发布门槛的小簇）。
    int clusterCount = 0;
    /// 除发布的那一簇之外，还有几簇也达到了发布门槛。卡片据此提示「另有 N 处独立触发」，
    /// 让用户知道同时存在别的地动，而不是被悄悄合并成一个中间点。
    int otherClusterCount = 0;
    /// 方法标识，写入卡片/日志便于追溯。
    std::string method = "intensity_decay_polar_v1";
    /// 发震时刻（epoch ms）。只有到时路径（日本区）会填；强度路径留 0。
    long long originEpochMs = 0;
    /// P / S 相位数。只有到时路径会填，强度路径为 0。
    int pPhaseCount = 0;
    int sPhaseCount = 0;
};

/// 一条**到时观测**：测站在 `onsetEpochMs` 首次越过感知阈值（P 波到时近似）。
///
/// 日本区（NIED Kmoni / S-Net）之所以能比强度场更准，是因为 kanameishi 与 kotoho7 的
/// 做法都用**到时**：站与站之间的触发时刻差把震中距差固定住，再用 JMA2001 走时表反解
/// 发震时刻与震源位置。强度场只能约束「哪里更近」，到时能约束「近多少」。
struct ArrivalObservation {
    double latitude = 0.0;
    double longitude = 0.0;
    /// 首次越过阈值的时刻（epoch ms）。同一次地震里各站之间的**差值**才是信息。
    long long onsetEpochMs = 0;
    /// 该站的强度（JMA 計測震度），用来加权：强震站震相更可信。
    double intensity = 0.0;
    /// 该站自触发以来到达的最大强度（kanameishi 的 `maxAscend` 同义），用于权重。
    double maxIntensity = 0.0;
};

/// 邻域联动参数。两个参考实现都不是「数到 N 个互不相干的站越阈就发布」：
/// kanameishi 要求某站的 6 个邻居里过半被激活、且邻域 activity 之和过表
/// （`NiedNet.vue` 的 `chainActivate`）；kotoho7 用「最近 7 点」做 `通常許可`。
///
/// 这里取两者共同的内核：**发布前，触发站里至少要有一个半径内的邻居团**。
/// 远场单个强噪声站（或一次远处的孤立触发）凑不出邻居团，就不会被当成震源。
struct ArrivalNeighborhoodConfig {
    /// 邻域半径（km）。kanameishi 的邻接表是「30 km 内 + 按方位补到每方向 2 个」；
    /// 这里用固定半径，因为我们的台站表没有预计算的方位均衡。
    double radiusKm = 30.0;
    /// 一个站要算「有邻居」至少需要多少个邻站（含自己）。kanameishi 的
    /// `numThres = n/2 + 1`（n = 6 个邻站时要求 4 个活跃）。
    int minClusterStations = 3;
    /// 参与联动的触发站占全部触发站的最低比例。低于它说明触发点全是孤立的，
    /// 不构成任何一片有感区，此时宁可只报「收集中」。
    double minClusteredRatio = 0.5;
};

/// kotoho7 `HYP:誤差レベル計算` 里的未触发站（着未着）惩罚参数。
///
/// 参考实现的核心洞察：少数已触发站可以「完美」贴合走时曲线，而候选位置落在
/// 一片本该已经到波、却毫无反应的测站中间 —— 那个解看起来误差极低，实际是错的。
/// 未触发站的作用就是给这种解加罚。
struct ArrivalQuietPenaltyConfig {
    bool enabled = true;
    /// 参考震中距分位数（kanameishi `penaltyReferenceQuantile: 0.9`）：
    /// 取「参与惩罚参考的已触发站」震中距的第 90 百分位，
    /// 作为「比它更近的未触发站才算矛盾」的分界线。
    double referenceQuantile = 0.9;
    /// 每有一个「本该已到波却没触发」的站，加多少秒² 的罚（与残差同量纲）。
    double perStationSeconds2 = 1.0;
    /// 罚的上限系数：罚项最多是正证据残差项的这个倍数，避免未触发站主导搜索。
    double maxRatio = 3.0;
    /// 只在这个时间窗内启用。超过之后，要么震已经走完、要么这些站本来就不该响，
    /// 继续惩罚会把解往台网中心拉（kanameishi 的 inherited 分阶段过滤同义）。
    long long activeWindowMs = 60'000;
};

/// 日本区到时推算的配置。默认值对齐 kanameishi `NiedHypocenterProfile.parameters`
/// 与 kotoho7 的搜索步长，但**不照抄**其台站专属量（那是 NIED 台站表 + 邻接矩阵的副产物）。
struct ArrivalEstimatorConfig {
    SourceRegion region = SourceRegion::Japan;
    /// 参与反演的最少站数。kanameishi `minInferenceStationCount: 5`。
    int minStations = 5;
    /// 发布门槛：达到它才对外给解（低于 minStations 时根本不出解）。
    /// 对齐 kanameishi 的 5 站，而不是「按台网密度估一个数」。
    int publishMinStations = 5;
    /// 邻域联动门槛。
    ArrivalNeighborhoodConfig neighborhood;
    /// 未触发站（着未着）惩罚。
    ArrivalQuietPenaltyConfig quietPenalty;
    /// 质量等级门槛，**按有效站数 + 质量分双门槛**（kanameishi `qualityRankMinEffectiveCounts`
    /// 与 `qualityRankMinScores`）。有效站数取「P/S 计数」与「去重站数」的均值，与参考一致。
    ///
    /// 注意：这组数字是给 NIED 那种上千站台网准备的（S 级要 200 站）。台网稀疏时
    /// 自然只有 C/D —— 那正是参考实现的意图：站少就别标高质量。
    int rankMinStationsS = 200;
    int rankMinStationsA = 100;
    int rankMinStationsB = 30;
    int rankMinStationsC = 10;
    /// 质量分门槛。**质量分越大越好**（残差越小、站数越多，分越高），
    /// 与 kanameishi `qualityRankMinScores: { S: 3, A: 2, B: 1, C: 0 }` 同向。
    double rankMinScoreS = 3.0;
    double rankMinScoreA = 2.0;
    double rankMinScoreB = 1.0;
    double rankMinScoreC = 0.0;
    /// 低于这个质量分不显示（kanameishi `minDisplayedHypocenterQualityScore: -3`）。
    double minDisplayedScore = -3.0;
    /// 走时表名（`jma2001` / `jb`）。距离超过 `farFieldKm` 时改用远场表。
    const char* nearTable = "jma2001";
    const char* farTable = "jb";
    double farFieldKm = 2000.0;
    /// 深度候选（km）。kanameishi 从 10 km 起扫，范围 0–700；这里只取地震学上
    /// 强度/到时都还分得开的档位，避免在无约束方向过拟合。
    std::vector<double> depthCandidatesKm = {10.0, 30.0, 50.0, 70.0, 100.0};
    /// 网格搜索：以触发站加权质心为中心，逐级缩小步长。
    /// kanameishi 用 3° → 1° → 0.3° → 0.1°，深度同步 100→50→20→10 km。
    double initialStepDeg = 3.0;
    double finalStepDeg = 0.05;
    int maxIterations = 60;
    /// 初始搜索半径（度）：覆盖一次大地震的触发区。
    double initialRadiusDeg = 3.0;
    /// Huber 阈值（秒）：到时残差超过它的站降权。
    double robustDeltaSeconds = 2.0;
    /// P 波速度（km/s），仅用于「相邻站触发是否兼容」的关联门（kanameishi 3.5 km/s）。
    double associationVelocityKmps = 3.5;
    /// 关联余量（ms）：触发时刻差超过「距离/速度 + 余量」就不算同一场。
    long long associationPaddingMs = 2000;
    /// 地理分离半径（km）：与强度路径同一语义。
    double clusterRadiusKm = 200.0;
    /// 同一测站重复触发的合并窗口（ms）。
    long long retriggerGapMs = 45'000;
};

/// 到时推算结果。字段与 `SourceEstimate` 对齐，另加到时特有的诊断量。
struct ArrivalEstimate {
    bool valid = false;
    SourceRegion region = SourceRegion::Japan;
    IntensityFamily family = IntensityFamily::Jma;
    double latitude = 0.0;
    double longitude = 0.0;
    double depthKm = 10.0;
    /// 发震时刻（epoch ms）。到时反演的**主产物**，强度场给不出这个量。
    long long originEpochMs = 0;
    /// 由**位置已定**后的各站强度反推的震级（比强度路径稳：震中距不再是搜索变量）。
    double magnitude = 0.0;
    /// 参与反演的发震时刻与 P/S 相位一致程度（秒，RMS）。
    double rmsSeconds = 0.0;
    /// 参与反演的站数（去重后）。
    int stationCount = 0;
    /// 拟合中贡献了正权重的站数。
    int usedStationCount = 0;
    /// 被判定为 P 的站数 / 被判定为 S 的站数（kanameishi 的 P/S/O/L 分类）。
    int pCount = 0;
    int sCount = 0;
    /// 至少有 P 与 S 两相被同时解释时的相位覆盖（0–1）。
    double phaseCoverage = 0.0;
    /// 纬度网格的 1σ 半宽（度，启发式）。
    double uncertaintyDeg = 0.0;
    /// 水平不确定度（km，启发式）。
    double uncertaintyKm = 0.0;
    double confidence = 0.0;
    std::string grade = "D";
    bool provisional = false;
    int clusterCount = 0;
    int otherClusterCount = 0;
    /// 被判为「本该已到波却没反应」的未触发站数（着未着惩罚的强度）。
    int penalizedQuietCount = 0;
    /// 质量分（越小越好；kanameishi `qualityScore` 同义）。低于 `minDisplayedScore` 不显示。
    double qualityScore = 0.0;
    /// 解是否贴住搜索边界。
    bool boundaryHit = false;
    std::string method = "arrival_time_jma2001_v1";
};

/// 日本区到时推算内核：纯函数、无 Qt、无全局状态。
///
/// 思路（对齐 kanameishi `FindHypocenter` 与 kotoho7 `点-震源 距離計算`）：
///   1. 每站给出**首次越阈时刻**，视作 P 波到时的观测；
///   2. 对候选震源 (lat, lon, depth, origin) 用 JMA2001 走时表预测每站 P 到时，
///      残差 `|onset − (origin + tP)|` 按 Huber 加权求 RMS；
///   3. 对每个候选位置，origin 有闭式解（加权中位数附近的一维最优），故只搜位置 + 深度；
///   4. 位置用「粗网格（3°）→ 逐级缩步」的坐标下降，深度在离散候选里取最优；
///   5. 支持 **P/S 双相**：若某站既是 P 又是 S 的候选（同一个 onset 可以解释为
///      「近处的 P」或「远处的 S」），取残差更小的那个相位，并按 kanameishi 的
///      `calcWaveCountPenalty` 对 S 明显多于 P 的场景降权（否则所有站都会被当成 S）。
///
/// 明确不做：震相拾取（没有波形）、有限断层、矩张量、NIED 台站专属的邻接矩阵。
class ArrivalEstimator {
public:
    /// 走时表访问接口。默认实现读 `TravelTimeService` 的全局表；
    /// 单测可注入一个假表（纯 C++，不依赖 Qt）。
    class TravelTimes {
    public:
        virtual ~TravelTimes() = default;
        /// 给定深度与震中距，返回 P 走时（秒）。
        virtual double pSeconds(double depthKm, double distanceKm) const = 0;
        /// 给定深度与震中距，返回 S 走时（秒）。
        virtual double sSeconds(double depthKm, double distanceKm) const = 0;
        /// 该距离应使用哪张表（`near` / `far`）。
        virtual bool farField(double distanceKm) const { return distanceKm > 2000.0; }
    };

    /// 给定到时观测与配置返回推算结果。证据不足时 `valid == false`。
    ///
    /// `quietStations` 是**尚未越过阈值**的同区测站（纬度, 经度），用于「着未着」惩罚：
    /// 候选震源如果让这些站此刻就该收到 P 波，说明位置错了。不传则只用正证据
    /// （站数足够、台网全向时够用；单侧台网时最好传）。
    /// `nowMs` 是当前时刻，用于判断「该站此刻本应已到波」以及惩罚的时间窗。
    static ArrivalEstimate estimate(
        const std::vector<ArrivalObservation>& observations,
        const ArrivalEstimatorConfig& config, const TravelTimes& travelTimes,
        const std::vector<std::pair<double, double>>& quietStations = {},
        long long nowMs = 0);

    /// 把到时解换算成统一的 `SourceEstimate`（供服务层与强度解同构发布）。
    static SourceEstimate toSourceEstimate(const ArrivalEstimate& arrival);
};

/// 本地震源推算内核：纯函数、不依赖 Qt，便于 `run_core_tests.sh` 直接编译。
///
/// 思路（对齐参考实现的 HYP 搜索，但只吃「当前逐站强度」这一种证据）：
///   强度模型 I(R) = a − c·log10(R + r0)，R 为震源距；
///   对每个候选位置用稳健（Huber）残差拟合 a，再叠加未触发站的删失惩罚；
///   极坐标粗扫 + 模式搜索细化；深度取离散候选里损失最小者；
///   最后用同一制式的衰减关系反演震级。
///
/// 明确不做：到时/震相拾取（测站通道没有 P/S 到时）、有限断层、矩张量。
class SourceEstimator {
public:
    /// 给定观测与配置返回推算结果；证据不足或观测无梯度时 `valid == false`。
    static SourceEstimate estimate(const std::vector<StationObservation>& observations,
                                   const SourceEstimatorConfig& config);

    /// 制式衰减关系下的预测强度（用于震级反演与负证据）。
    /// KMA 没有独立衰减模型，按中国 CSIS 近似（MMI 与 CSIS 在中低烈度段接近）。
    static double predictedIntensity(IntensityFamily family, double magnitude,
                                     double epicentralKm, double depthKm);

    /// 该制式下强度是否为有效观测（过滤各源缺测哨兵与饱和值）。
    static bool usableIntensity(IntensityFamily family, double intensity);
};

} // namespace komira
