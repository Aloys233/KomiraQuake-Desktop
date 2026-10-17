#include "map/station_map_item.h"

#include <QPainter>
#include <QQuickWindow>
#include <QSGGeometryNode>
#include <QSGImageNode>
#include <QSGRendererInterface>
#include <QSGTexture>
#include <QSGTextureMaterial>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/coordinate_transform.h"

namespace komira {
namespace {

constexpr double kLatLimit = 85.05112878;

/// 与 MapView.qml 的 projY 逐字对应。size 为「Z 层级下的世界像素」。
double projY(double lat, double size) {
    const double rad = qBound(-kLatLimit, lat, kLatLimit) * M_PI / 180.0;
    const double y = (1.0 - std::log(std::tan(rad) + 1.0 / std::cos(rad)) / M_PI) / 2.0;
    return y * size;
}

/// 与 QML wrapLon 一致：把经度差折到 (-180, 180]。
double wrapLonDelta(double delta) { return delta - std::round(delta / 360.0) * 360.0; }

double luminance(const QColor& c) {
    return 0.2126 * c.redF() + 0.7152 * c.greenF() + 0.0722 * c.blueF();
}

/// 持有图集纹理的几何节点（QSGTextureMaterial 不接管纹理生命周期）。
class StationNode : public QSGGeometryNode {
public:
    ~StationNode() override { delete texture; }
    QSGTexture* texture = nullptr;
    int atlasVersion = -1;
    qreal atlasDpr = -1.0;
    int csisPx = 0;
    int dotPx = 0;
    int idlePx = 0;
};

} // namespace

StationMapItem::StationMapItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
}

void StationMapItem::setSnapshot(QObject* object) {
    if (snapshotObject_ == object) return;
    if (snapshot_) disconnect(snapshot_, nullptr, this, nullptr);
    snapshotObject_ = object;
    snapshot_ = qobject_cast<StationSnapshot*>(object);
    emit snapshotChanged();

    if (!snapshot_) {
        stations_.clear();
        positions_.clear();
        update();
        return;
    }
    connect(snapshot_, &StationSnapshot::changed, this, &StationMapItem::applySnapshot);
    applySnapshot();
}

void StationMapItem::applySnapshot() {
    if (!snapshot_) return;
    const auto& source = snapshot_->entries();
    std::vector<StationSnapshot::Entry> next(source.begin(), source.end());

    // 纠偏只在数据变化时做一次；坐标未变的站按原下标复用上次结果。
    std::vector<QPointF> nextPos(next.size());
    for (std::size_t i = 0; i < next.size(); ++i) {
        const bool unchanged = i < stations_.size() && next[i].lat == stations_[i].lat &&
                               next[i].lon == stations_[i].lon && i < positions_.size();
        if (unchanged && gcjShift_) {
            nextPos[i] = positions_[i];
            continue;
        }
        double lat = next[i].lat;
        double lon = next[i].lon;
        if (gcjShift_) {
            const auto shifted = CoordinateTransform::wgs84ToGcj02(lat, lon);
            lat = shifted.first;
            lon = shifted.second;
        }
        nextPos[i] = QPointF(lon, lat);
    }

    stations_ = std::move(next);
    positions_ = std::move(nextPos);
    update();
}

void StationMapItem::setCenterLat(double v) {
    if (qFuzzyCompare(centerLat_ + 90.0, v + 90.0)) return;
    centerLat_ = v;
    emit cameraChanged();
    update();
}

void StationMapItem::setCenterLon(double v) {
    if (qFuzzyCompare(centerLon_, v)) return;
    centerLon_ = v;
    emit cameraChanged();
    update();
}

void StationMapItem::setZoom(double v) {
    if (qFuzzyCompare(zoom_, v)) return;
    zoom_ = v;
    emit cameraChanged();
    update();
}

void StationMapItem::setGcjShift(bool v) {
    if (gcjShift_ == v) return;
    gcjShift_ = v;
    rebuildProjectedPositions();
    emit gcjShiftChanged();
    update();
}

void StationMapItem::setNiedColors(const QVariantList& v) {
    if (niedColors_ == v) return;
    niedColors_ = v;
    ++colorVersion_;
    emit colorsChanged();
    update();
}

void StationMapItem::setCsisColors(const QVariantList& v) {
    if (csisColors_ == v) return;
    csisColors_ = v;
    ++colorVersion_;
    emit colorsChanged();
    update();
}

void StationMapItem::rebuildProjectedPositions() {
    positions_.resize(stations_.size());
    for (std::size_t i = 0; i < stations_.size(); ++i) {
        double lat = stations_[i].lat;
        double lon = stations_[i].lon;
        if (gcjShift_) {
            const auto shifted = CoordinateTransform::wgs84ToGcj02(lat, lon);
            lat = shifted.first;
            lon = shifted.second;
        }
        positions_[i] = QPointF(lon, lat);
    }
}

void StationMapItem::invalidateRenderCache() { update(); }

QSGNode* StationMapItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    // 没有内容时必须返回 nullptr（Qt 会销毁旧节点）。
    if (stations_.empty() || positions_.empty() || width() <= 0 || height() <= 0) {
        return nullptr;
    }

    // 相机、尺寸、DPR 只读一次：GUI 线程同时在拖动并写入这些成员，同一次绘制里混用
    // 新旧值会让画面与所记录的相机对不上 —— 表现就是测站「跟着拖走又弹回」。
    const double lat = centerLat_;
    const double lon = centerLon_;
    const double zoom = zoom_;
    const double w = width();
    const double h = height();
    const qreal dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;

    auto* node = static_cast<StationNode*>(oldNode);
    if (!node) {
        node = new StationNode;
        auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 0);
        geometry->setDrawingMode(QSGGeometry::DrawTriangles);
        geometry->setVertexDataPattern(QSGGeometry::DynamicPattern);
        node->setGeometry(geometry);
        node->setFlag(QSGNode::OwnsGeometry);
        auto* material = new QSGTextureMaterial;
        material->setFlag(QSGMaterial::Blending);
        node->setMaterial(material);
        node->setFlag(QSGNode::OwnsMaterial);
    }

    if (window()->rendererInterface()->graphicsApi() == QSGRendererInterface::Software) {
        return updateSoftwareNode(oldNode, lat, lon, zoom, w, h, dpr);
    }

    // 图集按当前显示尺寸（量化到整像素）重建；缩放动画里只有尺寸跨过整像素时才重建。
    int csisPx = 0, dotPx = 0, idlePx = 0;
    stationSizes(zoom, &csisPx, &dotPx, &idlePx);

    const bool atlasStale = atlas_.isNull() || atlasVersion_ != colorVersion_ ||
                            atlasDpr_ != dpr || csisPx != csisTilePx_ || dotPx != dotTilePx_ ||
                            idlePx != idleTilePx_;
    if (atlasStale) ensureAtlas(dpr, csisPx, dotPx, idlePx);

    auto* material = static_cast<QSGTextureMaterial*>(node->material());
    if (!node->texture || node->atlasVersion != atlasVersion_ || node->atlasDpr != atlasDpr_ ||
        node->csisPx != csisTilePx_ || node->dotPx != dotTilePx_ ||
        node->idlePx != idleTilePx_) {
        QSGTexture* texture =
            window()->createTextureFromImage(atlas_, QQuickWindow::TextureHasAlphaChannel);
        if (!texture) return nullptr;
        texture->setFiltering(QSGTexture::Linear);
        texture->setMipmapFiltering(QSGTexture::None);
        material->setTexture(texture);
        delete node->texture;   // material 已指向新纹理
        node->texture = texture;
        node->atlasVersion = atlasVersion_;
        node->atlasDpr = atlasDpr_;
        node->csisPx = csisTilePx_;
        node->dotPx = dotTilePx_;
        node->idlePx = idleTilePx_;
    }

    const double refSize = worldSize(kRefZoom);
    const double scale = std::pow(2.0, zoom - kRefZoom);
    const double world = refSize * scale;
    const double centerRefY = projY(lat, refSize);

    vertices_.clear();
    vertices_.reserve(stations_.size() * 6);

    const double csisSize = csisPx;
    const double dotSize = dotPx;
    const double idleSize = idlePx;

    auto addQuad = [&](double x, double y, double size, const QRectF& uv) {
        const float half = static_cast<float>(size / 2.0);
        const float fx = static_cast<float>(x);
        const float fy = static_cast<float>(y);
        const float u0 = static_cast<float>(uv.left());
        const float v0 = static_cast<float>(uv.top());
        const float u1 = static_cast<float>(uv.right());
        const float v1 = static_cast<float>(uv.bottom());
        vertices_.push_back({fx - half, fy - half, u0, v0});
        vertices_.push_back({fx + half, fy - half, u1, v0});
        vertices_.push_back({fx - half, fy + half, u0, v1});
        vertices_.push_back({fx + half, fy - half, u1, v0});
        vertices_.push_back({fx + half, fy + half, u1, v1});
        vertices_.push_back({fx - half, fy + half, u0, v1});
    };

    for (std::size_t i = 0; i < stations_.size(); ++i) {
        const StationSnapshot::Entry& s = stations_[i];
        const QPointF& pos = positions_[i];
        const double dx = wrapLonDelta(pos.x() - lon);
        const double x = w / 2.0 + dx / 360.0 * world;
        const double y = h / 2.0 + (projY(pos.y(), refSize) - centerRefY) * scale;

        if (s.csis) {
            if (s.raw >= 0.0) {
                if (x < -csisSize || y < -csisSize || x > w + csisSize || y > h + csisSize) {
                    continue;
                }
                const int level = qBound(0, static_cast<int>(std::lround(s.raw)), 12);
                addQuad(x, y, csisSize, csisUv_[level]);
            } else {
                if (x < -idleSize || y < -idleSize || x > w + idleSize || y > h + idleSize) {
                    continue;
                }
                addQuad(x, y, idleSize, idleUv_);
            }
            continue;
        }

        if (x < -dotSize || y < -dotSize || x > w + dotSize || y > h + dotSize) continue;
        const double raw = (s.raw <= -3.0) ? -3.0 : s.raw;
        const int idx = qBound(0, static_cast<int>(std::lround((raw + 3.0) * 2.0)), 20);
        addQuad(x, y, dotSize, niedUv_[idx]);
    }

    auto* geometry = node->geometry();
    geometry->allocate(static_cast<int>(vertices_.size()));
    if (!vertices_.empty()) {
        std::memcpy(geometry->vertexData(), vertices_.data(),
                    vertices_.size() * sizeof(QSGGeometry::TexturedPoint2D));
    }
    geometry->markVertexDataDirty();
    node->markDirty(QSGNode::DirtyGeometry);

    drawnScale_ = scale;
    drawnCenterLat_ = lat;
    drawnCenterLon_ = lon;
    return node;
}

QPointF StationMapItem::screenPosForTest(double lat, double lon) const {
    const double refSize = worldSize(kRefZoom);
    const double world = refSize * drawnScale_;
    const double dx = wrapLonDelta(lon - drawnCenterLon_);
    const double x = width() / 2.0 + dx / 360.0 * world;
    const double y = height() / 2.0 +
                     (projY(lat, refSize) - projY(drawnCenterLat_, refSize)) * drawnScale_;
    return QPointF(x, y);
}

void StationMapItem::stationSizes(double zoom, int* csisPx, int* dotPx, int* idlePx) {
    *csisPx = static_cast<int>(std::round(qBound(16.0, 16.0 + (zoom - 4.0) * 1.4, 24.0)));
    *dotPx = static_cast<int>(std::round(2.0 * qBound(1.6, 1.2 + (zoom - 4.0) * 0.45, 4.5)));
    *idlePx = static_cast<int>(std::round(2.0 * qBound(5.0, 5.0 + (zoom - 3.0) * 0.8, 9.0)));
}

QSGNode* StationMapItem::updateSoftwareNode(QSGNode* oldNode, double lat, double lon,
                                            double zoom, double w, double h, qreal dpr) {
    auto* node = static_cast<QSGImageNode*>(oldNode);
    const bool created = node == nullptr;
    if (!node) {
        node = window()->createImageNode();
        node->setOwnsTexture(true);
    }

    int csisPx = 0, dotPx = 0, idlePx = 0;
    stationSizes(zoom, &csisPx, &dotPx, &idlePx);
    if (atlas_.isNull() || atlasVersion_ != colorVersion_ || atlasDpr_ != dpr ||
        csisPx != csisTilePx_ || dotPx != dotTilePx_ || idlePx != idleTilePx_) {
        ensureAtlas(dpr, csisPx, dotPx, idlePx);
    }

    const QSize want(std::max(1, static_cast<int>(std::ceil(w * dpr))),
                     std::max(1, static_cast<int>(std::ceil(h * dpr))));
    if (softwareImage_.size() != want) {
        softwareImage_ = QImage(want, QImage::Format_ARGB32_Premultiplied);
    }
    softwareImage_.fill(Qt::transparent);

    const double refSize = worldSize(kRefZoom);
    const double scale = std::pow(2.0, zoom - kRefZoom);
    const double world = refSize * scale;
    const double centerRefY = projY(lat, refSize);
    const double atlasW = atlas_.width();
    const double atlasH = atlas_.height();
    const double csisSize = csisPx;
    const double dotSize = dotPx;
    const double idleSize = idlePx;

    QPainter painter(&softwareImage_);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.scale(dpr, dpr);

    auto drawTile = [&](double x, double y, double size, const QRectF& uv) {
        const QRectF src(uv.x() * atlasW, uv.y() * atlasH, uv.width() * atlasW,
                         uv.height() * atlasH);
        painter.drawImage(QRectF(x - size / 2.0, y - size / 2.0, size, size), atlas_, src);
    };

    for (std::size_t i = 0; i < stations_.size(); ++i) {
        const StationSnapshot::Entry& s = stations_[i];
        const QPointF& pos = positions_[i];
        const double dx = wrapLonDelta(pos.x() - lon);
        const double x = w / 2.0 + dx / 360.0 * world;
        const double y = h / 2.0 + (projY(pos.y(), refSize) - centerRefY) * scale;

        if (s.csis) {
            if (s.raw >= 0.0) {
                if (x < -csisSize || y < -csisSize || x > w + csisSize || y > h + csisSize) {
                    continue;
                }
                const int level = qBound(0, static_cast<int>(std::lround(s.raw)), 12);
                drawTile(x, y, csisSize, csisUv_[level]);
            } else {
                if (x < -idleSize || y < -idleSize || x > w + idleSize || y > h + idleSize) {
                    continue;
                }
                drawTile(x, y, idleSize, idleUv_);
            }
            continue;
        }
        if (x < -dotSize || y < -dotSize || x > w + dotSize || y > h + dotSize) continue;
        const double raw = (s.raw <= -3.0) ? -3.0 : s.raw;
        const int idx = qBound(0, static_cast<int>(std::lround((raw + 3.0) * 2.0)), 20);
        drawTile(x, y, dotSize, niedUv_[idx]);
    }
    painter.end();

    QSGTexture* texture =
        window()->createTextureFromImage(softwareImage_, QQuickWindow::TextureHasAlphaChannel);
    if (!texture) {
        if (created) delete node;
        return nullptr;
    }
    node->setTexture(texture);
    node->setFiltering(QSGTexture::Linear);
    node->setRect(QRectF(0, 0, w, h));

    drawnScale_ = scale;
    drawnCenterLat_ = lat;
    drawnCenterLon_ = lon;
    return node;
}

void StationMapItem::ensureAtlas(qreal dpr, int csisPx, int dotPx, int idlePx) {
    csisTilePx_ = csisPx;
    dotTilePx_ = dotPx;
    idleTilePx_ = idlePx;

    const int pad = kTilePad;
    const int cellH = std::max({csisPx, dotPx, idlePx}) + 2 * pad;
    const int csisCell = csisPx + 2 * pad;
    const int dotCell = dotPx + 2 * pad;
    const int idleCell = idlePx + 2 * pad;
    const int totalW = 13 * csisCell + 21 * dotCell + idleCell;

    const int imgW = std::max(1, static_cast<int>(std::ceil(totalW * dpr)));
    const int imgH = std::max(1, static_cast<int>(std::ceil(cellH * dpr)));
    atlas_ = QImage(imgW, imgH, QImage::Format_ARGB32_Premultiplied);
    atlas_.fill(Qt::transparent);

    QPainter painter(&atlas_);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.scale(double(imgW) / totalW, double(imgH) / cellH);

    auto normalize = [&](const QRectF& r) {
        return QRectF(r.x() / totalW, r.y() / cellH, r.width() / totalW, r.height() / cellH);
    };

    int x = 0;
    csisUv_.resize(13);
    for (int level = 0; level <= 12; ++level) {
        const QRectF r(x + pad, pad, csisPx, csisPx);
        drawMarkerTile(painter, r, level);
        csisUv_[level] = normalize(r);
        x += csisCell;
    }

    const double dotY = (cellH - dotPx) / 2.0;
    niedUv_.resize(21);
    for (int i = 0; i < 21; ++i) {
        const QRectF r(x + pad, dotY, dotPx, dotPx);
        painter.setPen(Qt::NoPen);
        painter.setBrush(niedColor(i));
        painter.drawEllipse(r);
        niedUv_[i] = normalize(r);
        x += dotCell;
    }

    {
        const double idleY = (cellH - idlePx) / 2.0;
        const QRectF r(x + pad, idleY, idlePx, idlePx);
        painter.setBrush(QColor(68, 102, 170, 128));
        painter.setPen(QPen(QColor(68, 102, 170, 179), 1.2));
        painter.drawEllipse(r);
        idleUv_ = normalize(r);
    }

    painter.end();
    atlasVersion_ = colorVersion_;
    atlasDpr_ = dpr;
}

void StationMapItem::drawMarkerTile(QPainter& painter, const QRectF& rect, int level) const {
    const double size = rect.width();
    QColor fill = csisColor(level);
    const double border = level >= 5 ? 1.6 : 0.9;
    const QRectF body = rect.adjusted(border / 2.0, border / 2.0, -border / 2.0, -border / 2.0);
    const double radius = size * 0.22;

    fill.setAlphaF(0.75);
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawRoundedRect(body, radius, radius);

    painter.setPen(
        QPen(level >= 5 ? QColor(255, 255, 255, 242) : QColor(255, 255, 255, 115), border));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(body, radius, radius);

    QFont font(QStringLiteral("sans-serif"));
    font.setBold(true);
    font.setPixelSize(
        std::max(1, static_cast<int>(std::lround(size * (level >= 5 ? 0.58 : 0.48)))));
    painter.setFont(font);
    painter.setPen(luminance(csisColor(level)) > 0.42 ? QColor(Qt::black) : QColor(Qt::white));
    painter.drawText(rect, Qt::AlignCenter, QString::number(level));
}

QColor StationMapItem::csisColor(int level) const {
    if (csisColors_.isEmpty()) return QColor(QStringLiteral("#9F9F9F"));
    const int idx = qBound(0, level, csisColors_.size() - 1);
    return QColor(csisColors_.at(idx).toString());
}

QColor StationMapItem::niedColor(int idx) const {
    if (niedColors_.isEmpty()) return QColor(QStringLiteral("#0003cf"));
    const int i = qBound(0, idx, niedColors_.size() - 1);
    return QColor(niedColors_.at(i).toString());
}

} // namespace komira
