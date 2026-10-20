#pragma once

#include <QColor>
#include <QImage>
#include <QMutex>
#include <QPainterPath>
#include <QPointF>
#include <QPointer>
#include <QQuickItem>
#include <QVariantMap>
#include <QtGlobal>
#include <QtQml/qqmlregistration.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/geo_geometry.h"
#include "core/vector_map_data.h"
#include "map/vector_mesh.h"

class QPainter;
class QSGNode;

namespace komira {

/// 矢量地图渲染项。
///
/// 投影缓存：墨卡托满足 projX(lon,z) = normX(lon)·256·2^z，即投影结果对 worldSize **线性**。
/// 因此所有几何只在加载/纠偏开关变化时按一个固定的参考层级 [kRefZoom] 投影一次，
/// 绘制时统一乘 2^(zoom-kRefZoom)，任意连续 zoom 都是**精确**结果（无插值误差）。
/// 这样既不必逐顶点重算 34 万个点，也不会在缩放跨过整数层级时（曾经的实现）触发整张
/// 地图的重建 —— 那种重建会在缩放动画中途引入一次明显卡顿。
///
/// 洞（多环）交给 QPainterPath 的 OddEvenFill 处理 —— 175 个区县是多环，
/// 填洞会让底图出现错误色块，故必须按 even-odd 规则原样保留。
///
/// ## 性能模型（2026-10 重构，见 docs/PERF_VECTOR_MAP.md）
///
/// 这一层是全应用唯一的 CPU 光栅化热点：它把 34 万个顶点的多边形画进一张离屏位图，
/// 再作为纹理上传。实测（1920×1440、AA 全开、全精度）单层重绘 13–57 ms，远超一帧预算。
/// 现在按下面五条同时收敛成本，几何语义（含洞、跨日界线副本、GCJ 纠偏）完全不变：
///
/// 1. **LOD 分档**：按「屏幕像素」误差预算对每条环做 Douglas–Peucker 简化，容差量化成
///    2 的幂档位（`tolRef = 2^-t`），使任意 zoom/dpr 下的简化误差 ≤ 0.5 设备像素。
///    全国概览这类「一个省只有几十像素」的场景能丢掉 70–85% 的顶点，肉眼不可分辨。
/// 2. **填充不抗锯齿、描边抗锯齿**：填充边缘由描边覆盖，那圈锯齿看不见；而单独一次
///    AA 填充在多边形上是最大头（实测占一半以上）。
/// 3. **空层跳过**：区县层平时全透明（只有震度区县着色），栅格底图叠加时的世界层
///    也是全透明 —— 这类「可见但无可绘制内容」的层直接不建缓存。
/// 4. **几何与档位都在 worker 线程构建**：投影 + GCJ 纠偏（34 万点 48 ms）与 DP 简化
///    （16–43 ms）不再落在渲染线程上。构建完成前退回到「已就绪的最近档位」绘制，
///    因此既不会整层空白，也不会闪一下。
/// 5. **位图按设备像素比渲染，但有字节预算**：高分屏上 1x 位图被放大两倍会糊；
///    这里按 dpr 光栅化，单张超过 [kCacheBudgetBytes] 时自动降档，避免常驻上百 MB。
///
/// 缓存只在「移出 margin / 缩放档位变化 / 样式变化 / 尺寸变化」时重建，
/// 隐藏（如关闭矢量叠加）时立即释放位图。
class VectorMapItem : public QQuickItem {
    Q_OBJECT
    /// 注册到 KomiraQuake 模块，供 QML 直接 `VectorMapItem { }` 使用。
    QML_ELEMENT
    /// 层名（"world" / "province" / "district"）。非空则该项渲染该层。
    Q_PROPERTY(QString layerName READ layerName WRITE setLayerName NOTIFY layerNameChanged)
    Q_PROPERTY(double centerLat READ centerLat WRITE setCenterLat NOTIFY cameraChanged)
    Q_PROPERTY(double centerLon READ centerLon WRITE setCenterLon NOTIFY centerLonChanged)
    Q_PROPERTY(double zoom READ zoom WRITE setZoom NOTIFY zoomChanged)
    /// 是否对 WGS-84 顶点施加 GCJ-02 纠偏（仅叠加到 GCJ-02 栅格底图时为 true）。
    Q_PROPERTY(bool gcjShift READ gcjShift WRITE setGcjShift NOTIFY gcjShiftChanged)
    // fillColor 与 QQuickPaintedItem 的 fillColor 同名，故重命名为 fill，
    // 否则 QML 赋值会遮蔽基类成员。
    Q_PROPERTY(QColor fill READ fillColor WRITE setFillColor NOTIFY styleChanged)
    Q_PROPERTY(QColor strokeColor READ strokeColor WRITE setStrokeColor NOTIFY styleChanged)
    Q_PROPERTY(qreal strokeWidth READ strokeWidth WRITE setStrokeWidth NOTIFY styleChanged)
    Q_PROPERTY(qreal strokeScale READ strokeScale WRITE setStrokeScale NOTIFY styleChanged)
    /// 区县层：要素名 -> CSIS 烈度原始值。未列出的要素不着色。
    Q_PROPERTY(QVariantMap districtIntensity READ districtIntensity WRITE setDistrictIntensity
                   NOTIFY districtIntensityChanged)
    /// 数据是否已就绪。
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(int featureCount READ featureCount NOTIFY readyChanged)
    /// 关闭 LOD（默认开）：用于取证/回归对照，强制使用全精度几何。
    Q_PROPERTY(bool lodEnabled READ lodEnabled WRITE setLodEnabled NOTIFY lodEnabledChanged)
    /// 几何是否已构建完成（异步）。QML/测试可据此区分「数据未加载」与「几何在算」。
    Q_PROPERTY(bool geometryReady READ geometryReady NOTIFY geometryReadyChanged)

public:
    explicit VectorMapItem(QQuickItem* parent = nullptr);
    ~VectorMapItem() override;

    QString layerName() const { return layerName_; }
    void setLayerName(const QString& v);

    double centerLat() const { return centerLat_; }
    void setCenterLat(double v);
    double centerLon() const { return centerLon_; }
    void setCenterLon(double v);
    double zoom() const { return zoom_; }
    void setZoom(double v);

    bool gcjShift() const { return gcjShift_; }
    void setGcjShift(bool v);

    QColor fillColor() const { return fillColor_; }
    void setFillColor(const QColor& v);
    QColor strokeColor() const { return strokeColor_; }
    void setStrokeColor(const QColor& v);
    qreal strokeWidth() const { return strokeWidth_; }
    void setStrokeWidth(qreal v);
    qreal strokeScale() const { return strokeScale_; }
    void setStrokeScale(qreal v);

    QVariantMap districtIntensity() const { return districtIntensity_; }
    void setDistrictIntensity(const QVariantMap& v);

    bool ready() const { return ready_; }
    int featureCount() const { return featureCount_; }
    bool geometryReady() const { return geometryReady_; }

    bool lodEnabled() const { return lodEnabled_; }
    void setLodEnabled(bool v);

    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override;

    /// 最近一帧实际绘制所用的「参考层级世界像素 -> 屏幕」映射：
    ///   screenX = (projX(lon, worldSize(kRefZoom)) - drawnCenterX_) * drawnScale_ + width()/2
    /// 它不是相机参数，而是**真的画在屏幕上的变换**（含离屏缓存分支）。回归测试据此断言
    /// 矢量层与震中/定位标记同源 —— 曾因缓存复用了一个过期的 scale，缩放时两者错位。
    Q_INVOKABLE QPointF screenPosForTest(double lat, double lon) const;
    /// 最近一帧所用的 LOD 容差（参考层级世界像素；0 = 全精度）。供回归测试断言
    /// 「低缩放用粗档、高缩放回全精度」，无需读私有状态。
    Q_INVOKABLE double lodToleranceForTest() const { return drawnTolRef_; }
    /// 当前缓存的字节数（0 表示已释放）。供内存回归使用。
    Q_INVOKABLE qint64 cacheBytesForTest() const { return cache_.sizeInBytes(); }
    /// 离屏位图被重新光栅化的次数。拖动探针用它判断「这一轮到底有没有真的重绘」——
    /// 只看耗时会误判（没触发重建时每帧都是 0 ms）。
    Q_INVOKABLE qint64 cacheRebuildsForTest() const { return cacheRebuilds_; }
    Q_INVOKABLE qint64 paintNodeCallsForTest() const { return paintNodeCalls_; }
    /// 当前是否走 GPU 三角形路径（false = 软件后端的 CPU 光栅化回退）。
    /// 回归测试据此断言「有 GPU 时不该再光栅化整层位图」。
    Q_INVOKABLE bool gpuPathActiveForTest() const { return gpuPathActive_; }
    /// 本帧实际提交的三角形数（GPU 路径）或 0（CPU 回退）。视口裁剪后应为数千级，
    /// 而不是整层的百万级 —— 这是 GPU 路径的核心收益所在。
    Q_INVOKABLE qint64 submittedTrianglesForTest() const { return submittedTriangles_; }
    /// 本帧实际绘制的分块数。
    Q_INVOKABLE int drawnTilesForTest() const { return drawnTiles_; }
    /// 当前缓存的 GPU 网格字节数（CPU 回退时为 0）。
    Q_INVOKABLE qint64 meshBytesForTest() const;
    /// 几何（顶点/索引缓冲）被重新上传的次数。
    ///
    /// 相机平移/缩放**不应**增加它：GPU 路径的收益正是「几何传一次、之后只改变换」。
    /// 早期实现每帧重建全部 QSGGeometry，拖动时每帧 +1（用户实测「开省界就卡」）。
    Q_INVOKABLE qint64 geometryUploadsForTest() const { return geometryUploads_; }
    /// 同步构建指定容差档位并返回该档路径数（仅测试用；正常路径走异步）。
    Q_INVOKABLE int buildTierForTest(double tolRef);

signals:
    void layerNameChanged();
    void cameraChanged();
    void centerLonChanged();
    void zoomChanged();
    void gcjShiftChanged();
    void styleChanged();
    void districtIntensityChanged();
    void readyChanged();
    void lodEnabledChanged();
    void geometryReadyChanged();

protected:
    /// 隐藏时立刻释放位图（矢量叠加关闭是常态，没必要常驻几十 MB）。
    void itemChange(ItemChange change, const ItemChangeData& value) override;
    /// QML 组件完成后在 GUI 线程完成加载与几何请求。
    void componentComplete() override;

private:
    /// 惰性加载 .kvb（同步一次，3.7 MiB）。
    void ensureLoaded();
    /// 从渲染线程安全地请求下一帧重绘（update() 本身只能在对象所属线程调用）。
    void scheduleUpdate();
    /// 图层 / 纠偏开关变化时作废投影缓存（与 zoom 无关）与位图缓存。
    void invalidateCache();
    /// 取回（或请求后台构建）本层在参考层级的世界像素几何（GUI 线程）。
    void requestBaseGeometry();

    /// 瓦片 256 px 下的世界尺寸。
    static double worldSize(int z) { return 256.0 * std::pow(2.0, z); }
    /// 几何投影到该固定参考层级下的世界像素（见类注释）。取中间层级使缩放系数
    /// 与坐标量级都适中：z 跨度 1..18 时最大放大 2^8，与逐层级投影的旧实现同量级。
    static constexpr int kRefZoom = 10;

    /// 单个要素：名称、包围盒、以及每条环在**参考层级世界像素**下的顶点（已施加 GCJ）。
    struct Feature {
        std::string name;
        GeoBounds bounds;
        std::vector<std::vector<QPointF>> rings;
    };

    /// 一层的全部要素（不含任何简化）。由 layer+gcj 唯一决定，进程内共享。
    struct LayerGeometry {
        std::vector<Feature> features;
    };
    using LayerGeometryPtr = std::shared_ptr<const LayerGeometry>;

    /// 某一 LOD 档位：与 LayerGeometry::features 下标一一对应的填充路径。
    struct LodTier {
        std::vector<QPainterPath> paths;
        int tier = 0;
        qint64 bytes = 0;
    };
    using LodTierPtr = std::shared_ptr<const LodTier>;

    /// 全精度档（不简化）。用一个大到不可能与真实档冲突的 tier 值表示。
    static constexpr int kFullTier = 1000;

    /// All map items in one MapView use the same immutable source data. Keeping this
    /// process-local avoids parsing the 3.7 MiB .kvb once per visible layer.
    static QMutex sharedDataMutex_;
    static std::shared_ptr<VectorMapData> sharedData_;
    static QString sharedDataPath_;
    static bool sharedDataLoadAttempted_;

    /// 基础几何与 LOD 档位都只依赖 layer+gcj（以及容差档），故全进程共享：
    /// world/province/district 三个 item 不会各自重算同一份投影。
    static QMutex geometryCacheMutex_;
    static std::unordered_map<std::string, LayerGeometryPtr> baseCaches_;
    static std::unordered_map<std::string, char> baseInFlight_;
    /// 等待某层几何构建完成的 item（构建完成后逐个唤醒）。用 QPointer：QML 用 new 创建
    /// item，没有 shared_ptr，且 item 可能在构建期间被销毁。
    static std::vector<QPointer<VectorMapItem>> waiters_;
    struct TierEntry {
        LodTierPtr tier;
        quint64 lastUse = 0;
    };
    static std::unordered_map<std::string, TierEntry> tierCaches_;
    static std::unordered_map<std::string, char> tierInFlight_;
    static qint64 tierBytes_;
    static quint64 useCounter_;
    /// 档位缓存的总字节上限。超出按最近最少使用淘汰（仍被 item 持有的档位由 shared_ptr
    /// 保活，不受淘汰影响）。
    static constexpr qint64 kTierBudgetBytes = 64LL * 1024 * 1024;

    /// 构建一层的参考层级几何。纯函数，可在任意线程调用。
    static LayerGeometryPtr buildBaseGeometry(const VectorMapData& data, const std::string& layerName,
                                              bool gcjShift);
    /// 由基础几何构建某一档位的填充路径。纯函数，可在任意线程调用。
    static LodTierPtr buildTier(const LayerGeometry& base, int tier);
    /// 淘汰超出预算的档位。调用方必须持有 geometryCacheMutex_。
    static void evictTiersLocked();
    static std::string baseKey(const std::string& layer, bool gcjShift);
    static std::string tierKey(const std::string& layer, bool gcjShift, int tier);

    /// 后台几何构建完成（GUI 线程回调）。
    void onBaseGeometryBuilt(const std::string& key, const LayerGeometryPtr& built);
    /// 后台 LOD 档位构建完成（GUI 线程回调）。
    void onTierBuilt(const std::string& key, const LodTierPtr& built);
    /// GUI 线程发起一次档位构建（渲染线程不能直接跑重活：生命周期不可控）。
    void startTierBuild(const std::string& key, const std::string& layer, bool gcj, int tier);

    QString layerName_;
    double centerLat_ = 35.0;
    double centerLon_ = 105.0;
    double zoom_ = 6.0;
    bool gcjShift_ = false;
    QColor fillColor_ = QColor(0, 0, 0, 0);
    QColor strokeColor_ = Qt::transparent;
    qreal strokeWidth_ = 0.0;
    qreal strokeScale_ = 1.0;
    QVariantMap districtIntensity_;
    bool lodEnabled_ = true;

    bool ready_ = false;
    /// 参考层级几何是否已构建完成（GUI 线程写，只反映本 item 的状态）。
    bool geometryReady_ = false;
    int featureCount_ = 0;
    std::shared_ptr<VectorMapData> data_;

    /// 基础几何（参考层级世界像素）。GUI 线程独占；渲染线程通过 published_ 取。
    LayerGeometryPtr geometry_;
    /// 要素名 -> 下标（与 geometry_->features 对齐）。区县着色时用它在 GUI 线程把
    /// QVariantMap 的键解析成下标，避免渲染线程每帧做 1011 次字符串哈希。
    ///
    /// **只在几何换新时建一次**：旧实现在每次 publishState() 里重建整张表，
    /// 而 publishState 会被每个 setter（fill/stroke/lod/districtIntensity…）调用。
    std::unordered_map<std::string, int> featureIndex_;

    /// 跨线程发布给渲染线程的**不可变**快照。
    /// updatePaintNode 在渲染线程执行，而所有 setter 都在 GUI 线程 —— 直接共享成员就是
    /// 数据竞争。GUI 线程在每次变化时打包一份，渲染线程在锁内拷出局部副本再画。
    struct RenderState {
        QString layer;
        bool gcjShift = false;
        bool lodEnabled = true;
        int styleVersion = 0;
        QColor fill;
        QColor stroke;
        qreal strokeWidth = 0.0;
        qreal strokeScale = 1.0;
        /// 参考层级几何（不可变，进程内共享）。
        LayerGeometryPtr geometry;
        /// 区县要素名 -> 该要素的着色下标（与 features 对齐）。已按名字预解析，绘制循环
        /// 不再做任何 QString/std::string 转换，也不再查 QVariantMap。
        std::vector<int> shadedIndices;
        /// 与 shadedIndices 同序的烈度值。
        std::vector<double> shadedRaw;
        bool geometryReady = false;
    };
    mutable QMutex stateMutex_;
    RenderState published_;

    /// 发布一份新的渲染快照（GUI 线程调用；锁内只做拷贝）。
    void publishState();

    /// 把「视口 + 四周 margin」渲成一张离屏位图。相机在 margin 范围内且缩放系数未明显
    /// 变化时，只对位图做仿射变换（平移 + 等比缩放），不再光栅化多边形。
    ///
    /// margin/slack 直接决定「拖动多少像素才重建一次」，是**手感**的关键：
    /// 视口 1280 宽时，192+256 的组合允许平移 320 px 才重绘一次；160+128 只有 224 px，
    /// 重建频率高 43%，拖动就变成一路小卡。真实屏幕（2560×1600）上差距比位图面积
    /// 省下的那点成本重要得多 —— 宁可用一张大点的位图，也不要一路重绘。
    static constexpr int kCacheMarginPx = 192;
    static constexpr int kCacheGrowSlackPx = 256;
    /// 复用位图时允许的缩放系数偏差（|scale/cacheScale-1|）。超出即重渲，兼顾
    /// 「缩放动画流畅」与「静止时边缘锐利」——位图放大太多会发虚。
    static constexpr double kCacheScaleTol = 0.15;
    /// 缓存位图固定按 **1x** 光栅化，由 GPU 采样放大。
    ///
    /// 曾试过按设备像素比渲染（高分屏更锐利），但在 2560×1600 / dpr≈1.6 的屏幕上：
    /// 位图从 2.76 Mpx 涨到 5.52 Mpx，单次重建成本翻倍、重建次数不变，缩放与拖动
    /// **明显变卡**。放大由 GPU 完成，1x 位图的观感与旧实现一致（它本来就是 1x）。
    static constexpr qreal kRasterDpr = 1.0;
    QImage cache_;
    bool cacheValid_ = false;
    /// 缓存渲染时相机中心在世界像素中的位置（参考层级坐标，**已含当时 scale**）。
    double cacheWorldX_ = 0.0;
    double cacheWorldY_ = 0.0;
    /// 缓存渲染时所用的缩放系数 2^(zoom-kRefZoom)。任何缩放都要按 s/此值放大位图，
    /// 否则位图停留在旧比例上 —— 曾因此让矢量层在缩放时与标记错位并跳变。
    double cacheScale_ = 1.0;
    /// 缓存位图的设备像素比；变化即重渲。
    qreal cacheDpr_ = 1.0;
    /// 缓存渲染时所用的 LOD 档位。**只用于测试断言**，不作为缓存失效条件：
    /// 档位是几何精度，换档后位图仍然可用（下一轮重建时才换成新精度）。早期把它
    /// 当成失效条件，缩放跨档时会整层重建，动画期间反复重建 = 严重卡顿。
    int cacheTier_ = kFullTier + 1;
    /// 场景图节点上是否已有与 cache_ 对应的纹理（节点重建时置 false）。
    bool textureValid_ = false;
    /// 缓存位图被重新光栅化的累计次数（诊断/回归用）。
    qint64 cacheRebuilds_ = 0;
    /// updatePaintNode 被调用的累计次数（诊断/回归用）。
    qint64 paintNodeCalls_ = 0;
    /// 任何影响外观的 setter 都自增；与 cacheStyleVersion_ 不等即作废缓存。
    int styleVersion_ = 0;
    int cacheStyleVersion_ = -1;

    /// 取一个可立即绘制的档位：优先「想要的档」，否则返回 nullptr 并把缺失的档位
    /// 排进后台构建队列（下一帧即可拿到）。调用方通过 lodTierOutOfDate_ 判断是否需要
    /// 再画一帧来换档。
    LodTierPtr resolveTier(const std::string& layer, bool gcj, int wanted);
    /// 相机是否停在整数层级（静止）。换 LOD 档位只在静止时做，避免缩放动画中途重绘。
    bool cameraAtRest() const;
    /// 本层当前是否有任何可绘制内容（空层跳过：区县层无着色、叠加模式下的世界层）。
    static bool hasDrawableContent(const RenderState& state);
    /// 释放离屏位图与其纹理引用。
    void dropCache();

    // ---- GPU 路径（QSGGeometry + 自定义材质）---------------------------------
    //
    // 有 GPU 时不再把整层光栅化进离屏位图，而是把三角化后的几何直接交给场景图：
    //   · 平移/缩放只改节点变换，**不重建任何几何**（原先每次移出 margin 就要
    //     重新光栅化 13–57 ms）；
    //   · 按分块做视口裁剪，一帧只提交可见块的索引（实测 z6 仅 9 块、5.7 万三角形）；
    //   · 描边宽度由 shader 按屏幕像素扩张，缩放时线宽恒定且几何复用。
    //
    // 软件后端（无 GPU）仍走 QPainter 光栅化：软件渲染器不支持自定义材质，
    // 且那条路径本来就是给 CI/远程桌面兜底的，性能不敏感。
    bool gpuPathActive_ = false;
    qint64 submittedTriangles_ = 0;
    int drawnTiles_ = 0;
    /// 按 (layer, gcj, tier, style) 缓存的 GPU 网格。与 CPU 档位缓存同样进程内共享。
    ///
    /// 带**字节预算 + LRU 淘汰**：key 里烘焙了完整样式（主题色 / 线宽 / dpr 换算后的
    /// 屏幕比例），换一次主题或连续拖滑块就会多出一整套新 key；旧实现只进不出，
    /// 每个 key 是几十 MB 的顶点缓冲，长时间运行会把内存吃满。
    struct MeshEntry {
        VectorMeshBuilder::LayerPtr layer;
        quint64 lastUse = 0;
    };
    static QMutex meshCacheMutex_;
    static std::unordered_map<std::string, MeshEntry> meshCaches_;
    static std::unordered_map<std::string, char> meshInFlight_;
    /// 当前网格缓存占用的字节数。
    static qint64 meshBytes_;
    /// 网格缓存预算（与 CPU 档位缓存同量级：两个后端不会同时活跃）。
    static constexpr qint64 kMeshBudgetBytes = 64LL * 1024 * 1024;
    /// 淘汰超预算的网格。调用方必须持有 meshCacheMutex_。
    static void evictMeshesLocked();
    /// 已上传到场景图的几何是否与当前 mesh_ 对应。
    ///
    /// 场景图节点**只在几何真的变了**（换层/换档/换样式）时才重建，相机变化只更新
    /// 变换矩阵。早期实现每帧重建全部 QSGGeometry 并把顶点索引重新 memcpy 一遍 ——
    /// 那是逐帧上传几 MB 的 GPU 缓冲，比原来的 CPU 光栅化还慢（用户实测「开省界
    /// 很卡」正是这个原因）。
    std::string uploadedMeshKey_;
    bool uploadedGeometryValid_ = false;
    /// 几何上传次数（诊断/回归用）。
    qint64 geometryUploads_ = 0;
    /// 上一次建节点时**可见块集合**的特征（可见块数量 + 起止块索引）。
    ///
    /// 只在「可见块集合变化」时才重建节点树：相机平移一点点会跨过块边界，此时旧的
    /// 节点集合不再匹配（少一块/多一块），必须补建或删掉；但绝大多数帧集合不变，
    /// 只更新变换矩阵。
    qint64 uploadedTileSignature_ = -1;

    /// 本 item 当前持有的网格（与 meshKey_ 对应）。
    ///
    /// GUI 线程写、渲染线程读，故**必须**经 meshMutex_ 访问：早期版本直接裸读，
    /// 在缩放换档（GUI 线程正在换 shared_ptr、渲染线程正在解引用）时 SIGSEGV。
    VectorMeshBuilder::LayerPtr mesh_;
    mutable QMutex meshMutex_;
    std::string meshKey_;
    double meshTolRef_ = -1.0;
    /// 后台网格构建完成（GUI 线程回调）。
    void onMeshBuilt(const std::string& key, const VectorMeshBuilder::LayerPtr& built);
    /// GUI 线程发起一次网格构建。
    void startMeshBuild(const std::string& key, const std::string& layer, bool gcj, double tolRef,
                        const VectorMeshBuilder::Style& style);
    /// 取（或请求构建）本层当前缩放的网格；无可用网格时返回 nullptr。
    VectorMeshBuilder::LayerPtr resolveMesh(const std::string& layer, bool gcj, double tolRef,
                                            const VectorMeshBuilder::Style& style,
                                            bool* needAnotherFrame);
    /// GPU 路径的节点构建。返回 nullptr 表示本帧无内容。
    QSGNode* updateGpuNode(QSGNode* oldNode, const RenderState& state, double lat, double lon,
                           double zoom, double scale);

    /// 最近一帧真实画出的「参考层级世界像素 -> 屏幕」映射的中心与缩放
    ///（见 screenPosForTest 的说明）。仅用于回归测试。
    double drawnScale_ = 1.0;
    double drawnCenterX_ = 0.0;
    double drawnCenterY_ = 0.0;
    double drawnTolRef_ = 0.0;

    /// 任何影响外观的 setter 都调用：作废位图并重新发布渲染快照。
    void invalidateRenderCache();
    /// 渲染线程专用：本帧是否有「想要的档位尚未就绪」的情形（需要再画一帧换档）。
    bool lodTierOutOfDate_ = false;

    /// 按当前相机把本层画进 cache_（按 dpr 渲染，贴图时再缩小回逻辑尺寸）。
    void renderToCache(qreal dpr, double cw, double ch, const LodTier& tier,
                       const RenderState& state, double lat, double lon, double zoom,
                       double scale);
    /// 把本层内容画到 painter 上，视口为 (vw, vh)，中心为当前相机。
    /// 相机参数按值传入：不能在绘制中途再读成员（GUI 线程正在写）。
    void drawLayerContent(QPainter& painter, double vw, double vh, const LodTier& tier,
                          const RenderState& state, double lat, double lon, double zoom,
                          double scale);
};

} // namespace komira
