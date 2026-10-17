#include "map/vector_map_item.h"

#include <QCoreApplication>
#include <QImage>
#include <QDir>
#include <QFileInfo>
#include <QPainter>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGTexture>
#include <QtMath>

#include "core/coordinate_transform.h"
#include "core/intensity_palette.h"

namespace komira {
namespace {

constexpr double kTileSize = 256.0;
/// 与 MapView.qml / MapScreen.kt 一致的墨卡托纬度截断。
constexpr double kLatLimit = 85.05112878;

/// 纬度 -> 墨卡托 Y（0..size）。公式与 MapView.qml 的 projY 逐字对应。
double projY(double lat, double size) {
    const double rad = qBound(-kLatLimit, lat, kLatLimit) * M_PI / 180.0;
    const double y = (1.0 - std::log(std::tan(rad) + 1.0 / std::cos(rad)) / M_PI) / 2.0;
    return y * size;
}
/// 世界像素 X -> 经度。
/// 经度 -> 墨卡托 X（0..size）。与 MapView.qml 的 projX 逐字对应。
double projX(double lon, double size) { return (lon + 180.0) / 360.0 * size; }

/// 世界像素 X -> 经度。
double unprojLon(double x, double size) {
    return x / size * 360.0 - 180.0;
}
/// 世界像素 Y -> 纬度。
double unprojLat(double y, double size) {
    const double n = M_PI - 2.0 * M_PI * y / size;
    return 180.0 / M_PI * std::atan(0.5 * (std::exp(n) - std::exp(-n)));
}

/// assets 目录解析，与 AppController 的查找顺序一致（可执行文件旁 → ../share）。
QString resolveDataPath() {
    QString root = QCoreApplication::applicationDirPath() + QStringLiteral("/assets");
    if (QFileInfo::exists(root + QStringLiteral("/map_bounds.kvb"))) {
        return root + QStringLiteral("/map_bounds.kvb");
    }
    const QString installed =
        QDir(QCoreApplication::applicationDirPath())
            .absoluteFilePath(QStringLiteral("../share/komiraquake/assets/map_bounds.kvb"));
    if (QFileInfo::exists(installed)) return installed;
    return root + QStringLiteral("/map_bounds.kvb");
}

} // namespace

QMutex VectorMapItem::sharedDataMutex_;
std::shared_ptr<VectorMapData> VectorMapItem::sharedData_;
QString VectorMapItem::sharedDataPath_;
bool VectorMapItem::sharedDataLoadAttempted_ = false;
QMutex VectorMapItem::geometryCacheMutex_;
std::unordered_map<std::string, VectorMapItem::GeometryCachePtr>
    VectorMapItem::geometryCaches_;

VectorMapItem::VectorMapItem(QQuickItem* parent) : QQuickItem(parent) {
    // QML 里尺寸由 anchors 决定，这里不自己算。
    setFlag(ItemHasContents, true);
    // 默认不裁剪到自身边界之外（地图会画出可视区边缘）。
}

VectorMapItem::~VectorMapItem() = default;

void VectorMapItem::setLayerName(const QString& v) {
    if (layerName_ == v) return;
    layerName_ = v;
    invalidateCache();
    emit layerNameChanged();
    update();
}

void VectorMapItem::setCenterLat(double v) {
    if (qFuzzyCompare(centerLat_, v)) return;
    centerLat_ = v;
    emit cameraChanged();
    update();
}

void VectorMapItem::setCenterLon(double v) {
    if (qFuzzyCompare(centerLon_, v)) return;
    centerLon_ = v;
    emit centerLonChanged();
    emit cameraChanged();
    update();
}

void VectorMapItem::setZoom(double v) {
    if (qFuzzyCompare(zoom_, v)) return;
    zoom_ = v;
    emit zoomChanged();
    update();
}

void VectorMapItem::setGcjShift(bool v) {
    if (gcjShift_ == v) return;
    gcjShift_ = v;
    invalidateCache();
    emit gcjShiftChanged();
    update();
}

void VectorMapItem::setFillColor(const QColor& v) {
    if (fillColor_ == v) return;
    fillColor_ = v;
    emit styleChanged();
    update();
    invalidateRenderCache();
}

void VectorMapItem::setStrokeColor(const QColor& v) {
    if (strokeColor_ == v) return;
    strokeColor_ = v;
    emit styleChanged();
    update();
    invalidateRenderCache();
}

void VectorMapItem::setStrokeWidth(qreal v) {
    if (qFuzzyCompare(strokeWidth_, v)) return;
    strokeWidth_ = v;
    emit styleChanged();
    update();
    invalidateRenderCache();
}

void VectorMapItem::setStrokeScale(qreal v) {
    if (qFuzzyCompare(strokeScale_, v)) return;
    strokeScale_ = v;
    emit styleChanged();
    update();
    invalidateRenderCache();
}

void VectorMapItem::setDistrictIntensity(const QVariantMap& v) {
    if (districtIntensity_ == v) return;
    districtIntensity_ = v;
    emit districtIntensityChanged();
    update();
    invalidateRenderCache();
}

void VectorMapItem::ensureLoaded() {
    if (ready_) return;

    const QString path = resolveDataPath();
    {
        QMutexLocker locker(&sharedDataMutex_);
        if (!sharedDataLoadAttempted_ || sharedDataPath_ != path) {
            auto data = std::make_shared<VectorMapData>();
            sharedData_ = data->loadFromFile(path.toStdString()) ? std::move(data) : nullptr;
            sharedDataPath_ = path;
            sharedDataLoadAttempted_ = true;
        }
        data_ = sharedData_;
    }

    if (data_) {
        featureCount_ = static_cast<int>(data_->featureCount());
        ready_ = true;
        invalidateCache();
        emit readyChanged();
    }
}

void VectorMapItem::invalidateCache() {
    pathsBuilt_ = false;
    geometryCache_.reset();
}

void VectorMapItem::rebuildCache() {
    geometryCache_.reset();
    cachedLayer_ = layerName_;
    cachedGcjShift_ = gcjShift_;
    pathsBuilt_ = true;

    if (!ready_ || layerName_.isEmpty()) return;
    const VectorLayer* layer = data_->layer(layerName_.toStdString());
    if (!layer) return;

    const std::string cacheKey = layerName_.toStdString() + (gcjShift_ ? ":gcj" : ":wgs");
    {
        QMutexLocker locker(&geometryCacheMutex_);
        const auto cached = geometryCaches_.find(cacheKey);
        if (cached != geometryCaches_.end()) {
            geometryCache_ = cached->second;
            return;
        }

        auto built = std::make_shared<GeometryCache>();

        // 所有几何按**固定参考层级**投影一次（见类注释）：墨卡托对 worldSize 线性，
        // 绘制时乘 2^(zoom-kRefZoom) 即得任意连续 zoom 的精确结果。zoom 变化不再重建，
        // 因此缩放动画不会在跨整数层级时卡一下。
        const double size = worldSize(kRefZoom);
        built->paths.reserve(layer->features.size());
        built->bounds.reserve(layer->features.size());
        built->names.reserve(layer->features.size());

        const bool isDistrict = layerName_ == QStringLiteral("district");

        for (const VectorFeature& f : layer->features) {
            // 视口外的要素不必建路径：全国范围下区县层几乎总是部分可见，
            // 但世界层在 z>=8 时 95% 的要素都在屏外。
            QPainterPath path;
            bool any = false;
            GeoBounds b;
            bool first = true;

            for (const GeoRing& ring : f.polygon) {
                if (ring.size() < 3) continue;
                path.moveTo(ring.front().second, ring.front().first);
                for (std::size_t i = 1; i < ring.size(); ++i) {
                    double lon = ring[i].second;
                    double lat = ring[i].first;
                    // GCJ-02 纠偏只在预投影时施加一次，绝不放进绘制循环。
                    if (gcjShift_ && isDistrict) {
                        const auto p = CoordinateTransform::wgs84ToGcj02(lat, lon);
                        lat = p.first;
                        lon = p.second;
                    }
                    path.lineTo(lon, lat);
                }
                any = true;

                for (const auto& pt : ring) {
                    if (first) {
                        b.minLat = b.maxLat = pt.first;
                        b.minLon = b.maxLon = pt.second;
                        first = false;
                    } else {
                        b.minLat = std::min(b.minLat, pt.first);
                        b.maxLat = std::max(b.maxLat, pt.first);
                        b.minLon = std::min(b.minLon, pt.second);
                        b.maxLon = std::max(b.maxLon, pt.second);
                    }
                }
            }
            if (!any) continue;

            // 经纬度 -> 参考层级世界像素（绘制时再统一乘 2^(zoom-kRefZoom)）。
            QPainterPath projected;
            projected.setFillRule(Qt::OddEvenFill);  // 洞靠 even-odd 自然镂空
            projected.reserve(path.elementCount());
            for (int i = 0; i < path.elementCount(); ++i) {
                const auto& e = path.elementAt(i);
                if (e.isMoveTo()) {
                    const double lon = e.x;
                    const double lat = qBound(-kLatLimit, e.y, kLatLimit);
                    projected.moveTo((lon + 180.0) / 360.0 * size, projY(lat, size));
                } else if (e.isLineTo()) {
                    const double lon = e.x;
                    const double lat = qBound(-kLatLimit, e.y, kLatLimit);
                    projected.lineTo((lon + 180.0) / 360.0 * size, projY(lat, size));
                }
            }
            built->paths.push_back(std::move(projected));
            built->bounds.push_back(b);
            built->names.push_back(f.name);
        }

        geometryCache_ = built;
        geometryCaches_.emplace(cacheKey, std::move(built));
    }
}

QSGNode* VectorMapItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    ensureLoaded();

    if (!pathsBuilt_ || gcjShift_ != cachedGcjShift_ || layerName_ != cachedLayer_) {
        rebuildCache();
        cacheValid_ = false;   // 投影结构变了，离屏缓存作废
    }
    if (!geometryCache_ || geometryCache_->paths.empty() || width() <= 0 || height() <= 0) {
        return nullptr;
    }

    // 相机与尺寸只读一次：渲染线程与 GUI 线程并发，同一次绘制里混用新旧值会让
    // 位图与 cacheWorldX_ 对不上（矢量层会相对标记整体偏移）。
    const double lat = centerLat_;
    const double lon = centerLon_;
    const double zoom = zoom_;
    const double w = width();
    const double h = height();
    // 矢量层仍按 1x 光栅化：位图只在「移出 margin / 缩放 / 样式变化」时重渲，
    // 而 dpr 光栅化会让三层缓存各涨到 4 倍（高 DPI 下上百 MB），重渲也慢 4 倍。
    // 放大由 GPU 采样完成，观感与旧的 QQuickPaintedItem 路径一致（缓存本来就是 1x）。
    const qreal dpr = 1.0;
    const double cw = w + 2 * kCacheMarginPx;
    const double ch = h + 2 * kCacheMarginPx;
    // 渲染尺寸比视口再多留一圈余量：右上角事件列表开合会连续改变控件宽度，
    // 缓存与视口严格等大时每一帧都要整层重绘（三个矢量层 × 上百个多边形），
    // 150ms 的开合动画直接变成幻灯片。缓存偏大时按 1:1 绘制、由 MapView 裁剪，
    // 缩小时可以一直复用，放大时也能撑住几帧才重绘。
    const double cwRender = cw + kCacheGrowSlackPx;
    const double chRender = ch + kCacheGrowSlackPx;
    const QSize wantSize(static_cast<int>(std::ceil(cw * dpr)),
                         static_cast<int>(std::ceil(ch * dpr)));

    const double refSize = worldSize(kRefZoom);
    const double scale = std::pow(2.0, zoom - kRefZoom);
    const double centerWX = projX(lon, refSize);
    const double centerWY = projY(lat, refSize);
    const double curWorldX = centerWX * scale;
    const double curWorldY = centerWY * scale;

    // 用 QSGImageNode 而不是 QQuickPaintedItem：后者每次 update() 都要把整张位图
    // 重新 CPU 合成并上传纹理，高 DPI 下矢量三层每帧要搬上百 MB。
    auto* node = static_cast<QSGImageNode*>(oldNode);
    if (!node) {
        node = window()->createImageNode();
        node->setOwnsTexture(true);
        textureValid_ = false;
    }

    double r = 1.0;
    bool reuseGeometry = false;
    // 缓存大于视口即可复用：>= 而不是 ==。地图在侧栏动画里持续变窄时，
    // 早已渲染好的大图只需平移裁剪，不必每帧重画整层矢量。
    if (cacheValid_ && cacheStyleVersion_ == styleVersion_ && cacheDpr_ == dpr &&
        cache_.width() >= wantSize.width() && cache_.height() >= wantSize.height()) {
        r = scale / cacheScale_;
        const double cacheCenterWX = cacheWorldX_ / cacheScale_;
        const double cacheCenterWY = cacheWorldY_ / cacheScale_;
        const double invR = cacheScale_ / scale;
        const double cacheW = cache_.width() / dpr;
        const double cacheH = cache_.height() / dpr;
        // 缓存半宽换算到当前比例后，扣掉视口自身需要的半宽，剩下的就是可容忍的位移。
        const double boundX = cacheW / 2.0 - w / 2.0 * invR;
        const double boundY = cacheH / 2.0 - h / 2.0 * invR;
        const bool inRange = std::abs((centerWX - cacheCenterWX) * cacheScale_) <= boundX &&
                             std::abs((centerWY - cacheCenterWY) * cacheScale_) <= boundY &&
                             std::abs(r - 1.0) <= kCacheScaleTol;
        // 停在整数层级时位图必须与像素 1:1，否则放大后发虚。
        const bool atRestInteger = std::abs(zoom - std::round(zoom)) < 1e-9;
        reuseGeometry = inRange && !(atRestInteger && std::abs(r - 1.0) > 1e-9);
    }

    if (!reuseGeometry) {
        renderToCache(dpr, cwRender, chRender);
        cacheWorldX_ = curWorldX;
        cacheWorldY_ = curWorldY;
        cacheScale_ = scale;
        cacheDpr_ = dpr;
        cacheStyleVersion_ = styleVersion_;
        cacheValid_ = true;
        textureValid_ = false;
    }

    if (!textureValid_) {
        QSGTexture* texture =
            window()->createTextureFromImage(cache_, QQuickWindow::TextureHasAlphaChannel);
        if (!texture) return nullptr;
        texture->setFiltering(QSGTexture::Linear);
        texture->setMipmapFiltering(QSGTexture::None);
        node->setTexture(texture);
        textureValid_ = true;
    }

    // 位图实际尺寸（可能因余量而大于视口）；绘制矩形必须按它来，否则会被二次缩放。
    const double cacheW = cache_.width() / dpr;
    const double cacheH = cache_.height() / dpr;
    if (reuseGeometry) {
        const double tx = r * (cacheWorldX_ - cacheW / 2.0) - curWorldX + w / 2.0;
        const double ty = r * (cacheWorldY_ - cacheH / 2.0) - curWorldY + h / 2.0;
        node->setRect(QRectF(tx, ty, cacheW * r, cacheH * r));
    } else {
        // 位图与视口同心：左上角相对 item 左上的偏移是 (w - cacheW)/2。
        node->setRect(QRectF((w - cacheW) / 2.0, (h - cacheH) / 2.0, cacheW, cacheH));
    }

    drawnScale_ = scale;
    drawnCenterX_ = centerWX;
    drawnCenterY_ = centerWY;
    return node;
}

QPointF VectorMapItem::screenPosForTest(double lat, double lon) const {
    const double refSize = worldSize(kRefZoom);
    const double x = (projX(lon, refSize) - drawnCenterX_) * drawnScale_ + width() / 2.0;
    const double y = (projY(lat, refSize) - drawnCenterY_) * drawnScale_ + height() / 2.0;
    return QPointF(x, y);
}

void VectorMapItem::renderToCache(qreal dpr, double cw, double ch) {
    const int pw = std::max(1, static_cast<int>(std::ceil(cw * dpr)));
    const int ph = std::max(1, static_cast<int>(std::ceil(ch * dpr)));
    if (cache_.size() != QSize(pw, ph)) cache_ = QImage(pw, ph, QImage::Format_ARGB32_Premultiplied);
    cache_.fill(Qt::transparent);
    QPainter painter(&cache_);
    // 按实际像素比缩放而不是直接用 dpr：像素尺寸取整后略有偏差，用实际比例才能保证
    // 逻辑坐标与 QML 的投影逐像素一致。
    painter.scale(double(pw) / cw, double(ph) / ch);
    drawLayerContent(painter, cw, ch);
    painter.end();
}

void VectorMapItem::drawLayerContent(QPainter& painter, double vw, double vh) {
    const double refSize = worldSize(kRefZoom);
    const double scale = std::pow(2.0, zoom_ - kRefZoom);
    const double originX = projX(centerLon_, refSize) * scale - vw / 2.0;
    const double originY = projY(centerLat_, refSize) * scale - vh / 2.0;

    // 视口（这里是「视口 + margin」的大矩形）在参考层级世界像素中的范围。
    const double left = originX / scale;
    const double top = originY / scale;
    const double right = (originX + vw) / scale;
    const double bottom = (originY + vh) / scale;
    const double worldPx = refSize;

    // 视口对应的经纬度范围。经度可能超出 ±180（相机贴近日界线时），环绕绘制正是
    // 靠这一点把要素的 ±360° 副本搬回视口内。
    const double viewWest = unprojLon(left, worldPx);
    const double viewEast = unprojLon(right, worldPx);
    // 纬度方向要小心：unprojLat(top) 是**北**边界（纬度更大）、unprojLat(bottom) 是
    // **南**边界。故「完全在北边」判 b.minLat > 北边界、「完全在南边」判
    // b.maxLat < 南边界 —— 与经度的 min/max 用法相反，写反会把视口内的要素全部裁掉
    // （地图一片空白）。
    const double viewNorth = unprojLat(top, worldPx);
    const double viewSouth = unprojLat(bottom, worldPx);

    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.save();
    // 整体缩放 + 平移：顶点不再逐帧重算。
    painter.translate(-originX, -originY);
    painter.scale(scale, scale);
    // 线宽按屏幕像素折算，否则缩小时描边会粗得糊住底图。
    painter.setPen(Qt::NoPen);
    painter.setBrush(fillColor_);

    const bool isDistrict = layerName_ == QStringLiteral("district");
    for (std::size_t i = 0; i < geometryCache_->paths.size(); ++i) {
        const GeoBounds& b = geometryCache_->bounds[i];
        // 纬度裁剪与经度副本无关，先做一次。
        if (b.minLat > viewNorth || b.maxLat < viewSouth) continue;

        QColor fill = fillColor_;
        bool shaded = false;
        if (isDistrict) {
            // 区县层**平时完全透明**（对齐 kanameishi 的 eewBaseMapDefaultFill/Stroke，
            // 两者 alpha 均为 0）：只在有震度的区县才描边 + 填色。
            // 否则 1011 条描边会在地图上铺满网格，反而看不清底图。
            const auto it = districtIntensity_.constFind(
                QString::fromStdString(geometryCache_->names[i]));
            if (it != districtIntensity_.constEnd()) {
                const auto rgb = intensity_palette::intensityColor(it.value().toDouble());
                // 填充用满不透明（kanameishi 的 classNameColors fillOpacity: 1），
                // 描边 #bbbbbb 单独一色，靠"底色 + 描边"区分等级。
                fill = QColor(rgb.r, rgb.g, rgb.b, 255);
                shaded = true;
            }
        }

        // 描边：区县层仅对已着色者描边（#bbbbbb，见 kanameishi eewBaseMapActiveStroke）。
        const bool wantsStroke =
            strokeWidth_ > 0.0 && strokeColor_.alpha() > 0 && (!isDistrict || shaded);
        QPen pen;
        if (wantsStroke) {
            pen = QPen(strokeColor_);
            pen.setWidthF(strokeWidth_ * strokeScale_ / scale);
            pen.setJoinStyle(Qt::RoundJoin);
        }

        // 数据只含 [-180,180] 的一份几何；相机经度被 QML 夹在 ±180，但视口可越界。
        // 视口跨越日界线时，把同一要素按 -360/0/+360 三份分别判定并绘制，缺的那半幅
        // 就补了回来 —— 与栅格瓦片的 wrapTile 行为一致。中/日等远离日界线的要素
        // 只会命中 0 号那份，开销可忽略。
        for (int copy = -1; copy <= 1; ++copy) {
            const double lonShift = copy * 360.0;
            if (b.maxLon + lonShift < viewWest || b.minLon + lonShift > viewEast) continue;

            painter.save();
            if (copy != 0) painter.translate(copy * worldPx, 0.0);
            if (fill.alpha() > 0) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(fill);
                painter.drawPath(geometryCache_->paths[i]);
            }
            if (wantsStroke) {
                painter.setPen(pen);
                painter.setBrush(Qt::NoBrush);
                painter.drawPath(geometryCache_->paths[i]);
            }
            painter.restore();
        }
    }

    painter.restore();
}

} // namespace komira
