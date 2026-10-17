#pragma once

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QPointer>
#include <QQuickItem>
#include <QSGGeometry>
#include <QVector>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>
#include <vector>

#include "model/station_snapshot.h"

class QSGNode;

namespace komira {

/// 测站震度图层（NIED 圆点 / Pancakes·Wolfx 的 CSIS 方标）。
///
/// 演进史（每一步都踩过坑，别走回头路）：
/// 1. QML Canvas：相机每变一次就在 JS 里整层重画，600 个 CSIS 方标实测 60 ms/帧。
/// 2. QQuickPaintedItem + 离屏位图：单帧是快了，但它每次 update() 都要把整张位图
///    重新 CPU 合成并上传纹理 —— 高 DPI 下每帧几十 MB，用户体感反而更卡。
/// 3. QSGImageNode + 离屏位图：纹理只在数据变化时上传，平移只改 rect。仍然有两个问题：
///    重绘/上传一张全窗口纹理本身不便宜，而且 updatePaintNode 里多次读相机成员变量会
///    和 GUI 线程的拖动写入竞争 —— 图像与记录的相机对不上，图层就「跟一下再弹回去」。
/// 4. 当前实现：直接构建 QSG 顶点（纹理图集 + 每帧重建四边形）。每帧只上传几十万字节的
///    顶点，纹理图集只在缩放/配色变化时重建一次。
///
/// 图集固定按最大尺寸渲染（方标 24 px、圆点直径 18 px），绘制时按当前 zoom 缩放四边形，
/// 因此缩放动画期间不需要重建图集。
class StationMapItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    /// 测站快照（AppController::stationSnapshot）。指针恒定，内容变化走 changed()。
    /// 经纬度为 WGS-84（未纠偏）；gcjShift 打开时在此处统一纠偏。
    Q_PROPERTY(QObject* snapshot READ snapshot WRITE setSnapshot NOTIFY snapshotChanged)
    Q_PROPERTY(double centerLat READ centerLat WRITE setCenterLat NOTIFY cameraChanged)
    Q_PROPERTY(double centerLon READ centerLon WRITE setCenterLon NOTIFY cameraChanged)
    Q_PROPERTY(double zoom READ zoom WRITE setZoom NOTIFY cameraChanged)
    /// 是否把测站坐标纠偏到 GCJ-02（叠加到国内 GCJ 底图时为 true）。
    Q_PROPERTY(bool gcjShift READ gcjShift WRITE setGcjShift NOTIFY gcjShiftChanged)
    /// NIED 连续震度色带（21 档，raw -3..7 每 0.5 一档）。
    Q_PROPERTY(QVariantList niedColors READ niedColors WRITE setNiedColors NOTIFY colorsChanged)
    /// CSIS 烈度色带（下标 0..12）。
    Q_PROPERTY(QVariantList csisColors READ csisColors WRITE setCsisColors NOTIFY colorsChanged)

public:
    explicit StationMapItem(QQuickItem* parent = nullptr);

    QObject* snapshot() const { return snapshotObject_; }
    void setSnapshot(QObject* object);

    double centerLat() const { return centerLat_; }
    void setCenterLat(double v);
    double centerLon() const { return centerLon_; }
    void setCenterLon(double v);
    double zoom() const { return zoom_; }
    void setZoom(double v);

    bool gcjShift() const { return gcjShift_; }
    void setGcjShift(bool v);

    QVariantList niedColors() const { return niedColors_; }
    void setNiedColors(const QVariantList& v);
    QVariantList csisColors() const { return csisColors_; }
    void setCsisColors(const QVariantList& v);

    /// 最近一帧实际绘制所用的投影（与 MapView 的 screenX/screenY 同源），供回归测试断言。
    Q_INVOKABLE QPointF screenPosForTest(double lat, double lon) const;

signals:
    void snapshotChanged();
    void cameraChanged();
    void gcjShiftChanged();
    void colorsChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override;

private:
    static double worldSize(int z) { return 256.0 * std::pow(2.0, z); }
    /// 与 VectorMapItem 一致的固定参考层级：坐标只按它算一次。
    static constexpr int kRefZoom = 10;

    /// 快照内容变化后重建纠偏坐标。
    void applySnapshot();
    void rebuildProjectedPositions();
    void invalidateRenderCache();

    /// 图集：13 个 CSIS 方标 + 21 档 NIED 圆点 + 1 个缺测蓝点。
    static void stationSizes(double zoom, int* csisPx, int* dotPx, int* idlePx);
    /// 软件后端（CI 的 QT_QUICK_BACKEND=software）不支持 QSGTextureMaterial 的顶点绘制，
    /// 退回「整层位图 + QSGImageNode」：只在测试/无 GPU 环境使用，性能不敏感。
    QSGNode* updateSoftwareNode(QSGNode* oldNode, double lat, double lon, double zoom, double w,
                                double h, qreal dpr);
    void ensureAtlas(qreal dpr, int csisPx, int dotPx, int idlePx);
    void drawMarkerTile(QPainter& painter, const QRectF& rect, int level) const;
    QColor csisColor(int level) const;
    QColor niedColor(int idx) const;

    static constexpr int kCsisTile = 24;
    static constexpr int kDotTile = 18;
    static constexpr int kTilePad = 1;
    static constexpr int kCsisCell = kCsisTile + 2 * kTilePad;
    static constexpr int kDotCell = kDotTile + 2 * kTilePad;

    QObject* snapshotObject_ = nullptr;
    QPointer<StationSnapshot> snapshot_;
    std::vector<StationSnapshot::Entry> stations_;
    /// 纠偏后的坐标（lon, lat），与 stations_ 下标一一对应。
    std::vector<QPointF> positions_;
    QVariantList niedColors_;
    QVariantList csisColors_;

    double centerLat_ = 35.0;
    double centerLon_ = 105.0;
    double zoom_ = 6.0;
    bool gcjShift_ = false;

    QImage atlas_;
    qreal atlasDpr_ = -1.0;
    int atlasVersion_ = -1;
    QVector<QRectF> csisUv_;
    QVector<QRectF> niedUv_;
    QRectF idleUv_;
    int csisTilePx_ = 0;
    int dotTilePx_ = 0;
    int idleTilePx_ = 0;
    /// 只随配色表变化：图集重建的判据。
    int colorVersion_ = 0;

    /// 复用的顶点缓冲，避免每帧重新分配。
    std::vector<QSGGeometry::TexturedPoint2D> vertices_;
    /// 软件后端使用的整层位图。
    QImage softwareImage_;

    // 最近一帧真实画出的变换（仅用于测试断言）。
    double drawnScale_ = 1.0;
    double drawnCenterLat_ = 0.0;
    double drawnCenterLon_ = 0.0;
};

} // namespace komira
