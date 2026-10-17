#pragma once

#include <QColor>
#include <QImage>
#include <QMutex>
#include <QtGlobal>
#include <QPainterPath>
#include <QQuickItem>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>
#include <memory>
#include <unordered_map>

#include "core/vector_map_data.h"

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

    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override;

    /// 最近一帧实际绘制所用的「参考层级世界像素 -> 屏幕」映射：
    ///   screenX = (projX(lon, worldSize(kRefZoom)) - drawnCenterX_) * drawnScale_ + width()/2
    /// 它不是相机参数，而是**真的画在屏幕上的变换**（含离屏缓存分支）。回归测试据此断言
    /// 矢量层与震中/定位标记同源 —— 曾因缓存复用了一个过期的 scale，缩放时两者错位。
    Q_INVOKABLE QPointF screenPosForTest(double lat, double lon) const;

signals:
    void layerNameChanged();
    void cameraChanged();
    void centerLonChanged();
    void zoomChanged();
    void gcjShiftChanged();
    void styleChanged();
    void districtIntensityChanged();
    void readyChanged();

private:
    /// 惰性加载 .kvb（同步一次，3.7 MiB）。
    void ensureLoaded();
    /// 图层 / 纠偏开关变化时作废投影缓存（与 zoom 无关）。
    void invalidateCache();
    void rebuildCache();

    /// 瓦片 256 px 下的世界尺寸。
    static double worldSize(int z) { return 256.0 * std::pow(2.0, z); }
    /// 几何投影到该固定参考层级下的世界像素（见类注释）。取中间层级使缩放系数
    /// 与坐标量级都适中：z 跨度 1..18 时最大放大 2^8，与逐层级投影的旧实现同量级。
    static constexpr int kRefZoom = 10;

    struct GeometryCache {
        std::vector<QPainterPath> paths;
        std::vector<GeoBounds> bounds;
        std::vector<std::string> names;
    };

    using GeometryCachePtr = std::shared_ptr<const GeometryCache>;

    /// All map items in one MapView use the same immutable source data. Keeping this
    /// process-local avoids parsing the 3.7 MiB .kvb once per visible layer.
    static QMutex sharedDataMutex_;
    static std::shared_ptr<VectorMapData> sharedData_;
    static QString sharedDataPath_;
    static bool sharedDataLoadAttempted_;

    /// Projection depends only on layer + GCJ mode, not on paint style or camera.
    /// Share it too so world/province/district items do not rebuild identical geometry.
    static QMutex geometryCacheMutex_;
    static std::unordered_map<std::string, GeometryCachePtr> geometryCaches_;

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

    bool ready_ = false;
    int featureCount_ = 0;
    std::shared_ptr<VectorMapData> data_;

    /// 投影缓存。路径坐标为「参考层级 [kRefZoom] 下的世界像素」，绘制时整体缩放。
    bool pathsBuilt_ = false;
    bool cachedGcjShift_ = false;
    QString cachedLayer_;
    GeometryCachePtr geometryCache_;

    /// 把「视口 + 四周 margin」渲成一张离屏位图。相机在 margin 范围内且缩放系数未明显
    /// 变化时，只对位图做仿射变换（平移 + 等比缩放），不再光栅化多边形 ——
    /// QQuickPaintedItem 是 CPU 光栅化，每帧重画上千条路径会直接吃掉帧预算
    ///（实测 world 层单层就 6 ms/帧）。
    static constexpr int kCacheMarginPx = 192;
    /// 渲染缓存时额外多渲染的总尺寸（左右各一半）：侧栏开合会连续改变宽度，
    /// 有这圈余量时缩窄可以一直复用，变宽也要跨过余量才重绘一次。
    static constexpr int kCacheGrowSlackPx = 256;
    /// 复用位图时允许的缩放系数偏差（|scale/cacheScale-1|）。超出即重渲，兼顾
    /// 「缩放动画流畅」与「静止时边缘锐利」——位图放大太多会发虚。
    static constexpr double kCacheScaleTol = 0.15;
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
    /// 场景图节点上是否已有与 cache_ 对应的纹理（节点重建时置 false）。
    bool textureValid_ = false;
    /// 任何影响外观的 setter 都自增；与 cacheStyleVersion_ 不等即作废缓存。
    int styleVersion_ = 0;
    int cacheStyleVersion_ = -1;

    /// 最近一帧真实画出的「参考层级世界像素 -> 屏幕」映射的中心与缩放
    ///（见 screenPosForTest 的说明）。仅用于回归测试。
    double drawnScale_ = 1.0;
    double drawnCenterX_ = 0.0;
    double drawnCenterY_ = 0.0;

    void invalidateRenderCache() {
        ++styleVersion_;
        cacheValid_ = false;
    }
    /// 按当前相机把本层画进 cache_（按 dpr 渲染，贴图时再缩小回逻辑尺寸）。
    void renderToCache(qreal dpr, double cw, double ch);
    /// 把本层内容画到 painter 上，视口为 (vw, vh)，中心为当前相机。
    void drawLayerContent(QPainter& painter, double vw, double vh);
};

} // namespace komira
