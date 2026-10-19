#include "map/station_map_item.h"

#include <QFont>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
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
#include "source/kmoni_station_parser.h"

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

/// 字形墨迹外框（原点为基线起点）。用 outline 而不是 QFontMetricsF::tightBoundingRect：
/// 后者按整像素取整，小字号下与实际栅格结果差 ±0.5 px，且对部分字体会带上额外留白。
/// 实测用 outline 得到的中心与 drawText 栅格结果偏差 < 0.05 px。
QRectF inkBounds(const QFont& font, const QString& text) {
    QPainterPath path;
    path.addText(QPointF(0.0, 0.0), font, text);
    return path.boundingRect();
}

/// 把文字画在 rect 的**视觉中心**。
///
/// drawText(rect, Qt::AlignCenter) 居中的是字体行盒（ascent + descent + leading），
/// 而数字墨迹并不占满行盒：CJK 回退字体（Noto Sans CJK SC）的行盒比 Google Sans 高
/// 4 px 左右，于是同一段代码在不同字体下会整体偏下；1.3 缩放、24 px 方标实测偏右下
/// 约 1 px，看起来就是「数字没居中」。这里改为按墨迹外框对齐，字体、字号、DPI 都不再影响。
void drawGlyphCentered(QPainter& painter, const QRectF& rect, const QString& text,
                       const QFont& font, const QColor& color) {
    painter.setFont(font);
    painter.setPen(color);
    const QRectF ink = inkBounds(font, text);
    const QPointF center = rect.center();
    painter.drawText(QPointF(center.x() - ink.center().x(), center.y() - ink.center().y()), text);
}

/// 方标里的数字字体：跟随应用字体（Google Sans + CJK 回退），并开启等宽数字，
/// 与卡片/时钟一致；两位数字（10/11/12）在小方标里按可用宽度自动降一号字号。
QFont markerFont(const QRectF& body, const QString& text, int size, double ratio, qreal dpr) {
    QFont font = QGuiApplication::font();
    font.setBold(true);
    font.setPixelSize(std::max(1, static_cast<int>(std::lround(size * ratio))));
    font.setFeature(QFont::Tag("tnum"), 1);
    const double maxWidth = body.width() * 0.86;
    const int floorPx = std::max(1, static_cast<int>(std::lround(6.0 * dpr)));
    while (font.pixelSize() > floorPx && inkBounds(font, text).width() > maxWidth) {
        font.setPixelSize(font.pixelSize() - 1);
    }
    return font;
}

/// 方标数值用色：对齐 kanameishi 的图标（0–7 黑字、8 起白字），阈值取亮度 0.5 ——
/// 恰好把 #FF4F00（烈度 8）及更深的红/紫归到白字，绿黄橙档仍是黑字。
QColor markerTextColor(const QColor& fill) {
    return luminance(fill) < 0.5 ? QColor(Qt::white) : QColor(Qt::black);
}

/// 方标描边：高饱和底色用白边把方标从底图上抠出来；浅灰/近白底色（烈度 0–2）
/// 白边等于隐形，改用同色压暗边。
QColor markerRingColor(const QColor& fill, double strength) {
    QColor ring = luminance(fill) > 0.72 ? fill.darker(190) : QColor(Qt::white);
    ring.setAlphaF(strength);
    return ring;
}

/// 方标本体（圆角底 + 描边 + 居中数字），CSIS 与 KMA 共用。
///
/// `rect` 是**设备像素**坐标：图集按设备像素排版，Qt 才会把字号栅格化到整设备像素，
/// 数字与描边都比「先按逻辑像素画、再整体缩放 dpr」锐利得多。
void drawLabeledMarker(QPainter& painter, const QRectF& rect, const QColor& fillColor,
                       const QString& text, bool strong, qreal dpr) {
    const double size = rect.width();
    const double border = (strong ? 1.6 : 0.9) * dpr;
    const QRectF body = rect.adjusted(border / 2.0, border / 2.0, -border / 2.0, -border / 2.0);
    const double radius = size * 0.22;

    // 带数字的方标要读数字，底色不做重度半透明：0.75 时浅灰档（烈度 0–2）在浅色
    // 底图上几乎糊成一片，数字也跟着发灰。
    QColor fill = fillColor;
    fill.setAlphaF(strong ? 0.94 : 0.88);
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawRoundedRect(body, radius, radius);

    // 描边只为把方标从底图上「抠」出来：高饱和底色用白边；浅灰/近白底色换同色压暗边，
    // 否则白边 + 半透明灰块在浅色底图上看不见。
    painter.setPen(QPen(markerRingColor(fillColor, strong ? 0.95 : 0.45), border));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(body, radius, radius);

    const QFont font = markerFont(body, text, static_cast<int>(std::lround(size)),
                                  strong ? 0.58 : 0.48, dpr);
    drawGlyphCentered(painter, body, text, font, markerTextColor(fillColor));
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
        projYRef_.clear();
        update();
        emit stationsApplied();
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
    rebuildProjectedPositions();
    update();
    emit stationsApplied();
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

void StationMapItem::setKmaColors(const QVariantList& v) {
    if (kmaColors_ == v) return;
    kmaColors_ = v;
    ++colorVersion_;
    emit colorsChanged();
    update();
}

void StationMapItem::setKmaDotColors(const QVariantList& v) {
    if (kmaDotColors_ == v) return;
    kmaDotColors_ = v;
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
    const double refSize = worldSize(kRefZoom);
    projYRef_.resize(positions_.size());
    for (std::size_t i = 0; i < positions_.size(); ++i) {
        projYRef_[i] = projY(positions_[i].y(), refSize);
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
        const double y = h / 2.0 + (projYRef_[i] - centerRefY) * scale;

        if (s.kma) {
            if (s.raw >= 0.0) {
                // 该通道的韩国 MMI 静稳基线是 1（全站常年 1）。0/1 两档取圆点色阶的
                // 深蓝（与 NIED 缺测点同色）；MMI >= 2 才改用 `kmaIntColorBand` 的
                // 带数字方标（数字 = MMI，色档沿用 kanameishi 的 MMI+2）。
                const int mmi = qBound(0, static_cast<int>(std::lround(s.raw)), 11);
                if (mmi >= 2) {
                    if (x < -csisSize || y < -csisSize || x > w + csisSize || y > h + csisSize) {
                        continue;
                    }
                    addQuad(x, y, csisSize, kmaMarkerUv_[mmi - 2]);
                } else {
                    if (x < -dotSize || y < -dotSize || x > w + dotSize || y > h + dotSize) {
                        continue;
                    }
                    addQuad(x, y, dotSize, kmaDotUv_[mmi]);
                }
            } else {
                if (x < -idleSize || y < -idleSize || x > w + idleSize || y > h + idleSize) {
                    continue;
                }
                addQuad(x, y, idleSize, idleUv_);
            }
            continue;
        }

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

double StationMapItem::markerSize() const {
    int csisPx = 0, dotPx = 0, idlePx = 0;
    stationSizes(zoom_, &csisPx, &dotPx, &idlePx);
    return std::max(csisPx, std::max(dotPx, idlePx));
}

QPointF StationMapItem::stationScreenPos(int index) const {
    if (index < 0 || index >= static_cast<int>(positions_.size())
        || index >= static_cast<int>(projYRef_.size())) {
        return QPointF(-1e9, -1e9);
    }
    const double refSize = worldSize(kRefZoom);
    const double scale = std::pow(2.0, zoom_ - kRefZoom);
    const double world = refSize * scale;
    const double dx = wrapLonDelta(positions_[static_cast<std::size_t>(index)].x() - centerLon_);
    return QPointF(width() / 2.0 + dx / 360.0 * world,
                   height() / 2.0 + (projYRef_[static_cast<std::size_t>(index)] -
                                     projY(centerLat_, refSize)) * scale);
}

QVariantMap StationMapItem::stationAt(double sx, double sy) const {
    QVariantMap out;
    out[QStringLiteral("found")] = false;
    if (positions_.empty() || projYRef_.size() != positions_.size()) return out;

    // 命中半径 = 标记半径 + 3 px 余量：圆点很小（缩远时只有 3 px），不留余量就得
    // 像素级瞄准；余量再大会让浮层出现在空地上。
    const double radius = std::max(4.0, markerSize() * 0.5 + 3.0);
    const double radius2 = radius * radius;

    const double refSize = worldSize(kRefZoom);
    const double scale = std::pow(2.0, zoom_ - kRefZoom);
    const double world = refSize * scale;
    const double centerRefY = projY(centerLat_, refSize);
    const double lonToPx = world / 360.0;

    // 纯算术 O(n)：不做投影、不分配、不碰场景图。视野内的站优先（同一像素附近有多个
    // 站时取最近的；完全重合时后画的在上，故用 <= 让后者胜出）。
    int best = -1;
    double bestDist2 = radius2;
    for (std::size_t i = 0; i < positions_.size(); ++i) {
        const double x = width() / 2.0 + wrapLonDelta(positions_[i].x() - centerLon_) * lonToPx;
        const double dx = x - sx;
        // 纵向先粗筛：地图上层叠的站点很多，省掉大部分 y 投影。
        if (dx > radius || dx < -radius) continue;
        const double y = height() / 2.0 + (projYRef_[i] - centerRefY) * scale;
        const double dy = y - sy;
        if (dy > radius || dy < -radius) continue;
        const double d2 = dx * dx + dy * dy;
        if (d2 <= bestDist2) {
            bestDist2 = d2;
            best = static_cast<int>(i);
        }
    }
    if (best < 0) return out;

    const StationSnapshot::Entry& e = stations_[static_cast<std::size_t>(best)];
    out[QStringLiteral("found")] = true;
    out[QStringLiteral("index")] = best;
    out[QStringLiteral("lat")] = e.lat;
    out[QStringLiteral("lon")] = e.lon;
    out[QStringLiteral("raw")] = e.raw;
    out[QStringLiteral("csis")] = e.csis;
    out[QStringLiteral("kma")] = e.kma;
    // 显示文本在 C++ 侧一次算好，浮层不再做分档判断。
    if (e.kma) {
        out[QStringLiteral("standard")] = QStringLiteral("kma");
        out[QStringLiteral("intensity")] =
            e.raw < 0.0 ? QStringLiteral("--")
                        : QString::number(std::max(0, std::min(11, static_cast<int>(std::lround(e.raw)))));
    } else if (e.csis) {
        out[QStringLiteral("standard")] = QStringLiteral("csis");
        out[QStringLiteral("intensity")] =
            e.raw < 0.0 ? QStringLiteral("--")
                        : QString::number(std::max(0, static_cast<int>(std::lround(e.raw))));
    } else {
        out[QStringLiteral("standard")] = QStringLiteral("jma");
        out[QStringLiteral("intensity")] =
            e.raw >= 99.0 ? QStringLiteral("7") : jmaShindoText(e.raw);
    }
    // 元数据下标以 Entry::meta 为准（正常路径下就等于 best）；越界时退回 best。
    int metaIndex = e.meta >= 0 ? e.meta : best;
    if (snapshot_ && metaIndex < static_cast<int>(snapshot_->metas().size())) {
        const StationSnapshot::StationMeta& m =
            snapshot_->metas()[static_cast<std::size_t>(metaIndex)];
        out[QStringLiteral("name")] = m.name;
        out[QStringLiteral("area")] = m.area;
        out[QStringLiteral("code")] = m.code;
        out[QStringLiteral("type")] = m.type;
        out[QStringLiteral("source")] = m.source;
    }
    return out;
}

QImage StationMapItem::markerTileForTest(int level, double zoom, double dpr) {
    // 用一次性实例渲染，绝不碰正在显示的图层：GPU 后端下场景图渲染线程可能同时读
    // this->atlas_，测试里改写它会变成数据竞争。
    StationMapItem probe;
    probe.setNiedColors(niedColors_);
    probe.setCsisColors(csisColors_);
    probe.setKmaColors(kmaColors_);
    probe.setKmaDotColors(kmaDotColors_);
    int csisPx = 0, dotPx = 0, idlePx = 0;
    stationSizes(zoom, &csisPx, &dotPx, &idlePx);
    probe.ensureAtlas(dpr, csisPx, dotPx, idlePx);
    const int lv = qBound(0, level, probe.csisUv_.size() - 1);
    const QRectF uv = probe.csisUv_.at(lv);
    const int x = static_cast<int>(std::lround(uv.x() * probe.atlas_.width()));
    const int w = static_cast<int>(std::lround(uv.width() * probe.atlas_.width()));
    // 垫一层实心底色，测试才能直接按亮度找文字墨迹（图集本身是透明底）。
    QColor under = probe.csisColor(lv);
    under.setAlphaF(1.0);
    QImage tile(std::max(1, w), probe.atlas_.height(), QImage::Format_ARGB32_Premultiplied);
    tile.fill(under);
    QPainter painter(&tile);
    painter.drawImage(QRect(0, 0, tile.width(), tile.height()), probe.atlas_,
                      QRect(x, 0, w, probe.atlas_.height()));
    return tile;
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

    // 软件后端是**逐块 drawImage** 把 2500 个图块画进整层位图，成本随像素数平方增长：
    // 实测 dpr=1 时 1.0 ms/帧、dpr=2 时 18.8 ms/帧（同一份数据）。这个后端只在无 GPU
    // 的环境（CI/远程桌面/虚拟机）出现，清晰度让位于可用性：把 dpr 限到 1.5，
    // 并把整层位图控制在 800 万像素以内。
    qreal drawDpr = qMin<qreal>(dpr, 1.5);
    const double pixels = w * h * double(drawDpr) * double(drawDpr);
    if (pixels > 8.0e6) drawDpr = std::sqrt(8.0e6 / std::max(1.0, w * h));
    dpr = qMax<qreal>(1.0, drawDpr);

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
        const double y = h / 2.0 + (projYRef_[i] - centerRefY) * scale;

        if (s.kma) {
            if (s.raw >= 0.0) {
                const int mmi = qBound(0, static_cast<int>(std::lround(s.raw)), 11);
                if (mmi >= 2) {
                    if (x < -csisSize || y < -csisSize || x > w + csisSize || y > h + csisSize) {
                        continue;
                    }
                    drawTile(x, y, csisSize, kmaMarkerUv_[mmi - 2]);
                } else {
                    if (x < -dotSize || y < -dotSize || x > w + dotSize || y > h + dotSize) {
                        continue;
                    }
                    drawTile(x, y, dotSize, kmaDotUv_[mmi]);
                }
            } else {
                if (x < -idleSize || y < -idleSize || x > w + idleSize || y > h + idleSize) {
                    continue;
                }
                drawTile(x, y, idleSize, idleUv_);
            }
            continue;
        }

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

    // 图集按**设备像素**排版：字号、描边、圆角都是整数设备像素，数字不再被整体缩放
    // 采样一遍，1.25/1.3/1.5 这类分数缩放下也不会糊。
    const auto toDevice = [dpr](double v) {
        return std::max(1, static_cast<int>(std::lround(v * dpr)));
    };
    const int pad = std::max(1, toDevice(kTilePad));
    const int csisDev = toDevice(csisPx);
    const int dotDev = toDevice(dotPx);
    const int idleDev = toDevice(idlePx);

    const int cellH = std::max({csisDev, dotDev, idleDev}) + 2 * pad;
    const int csisCell = csisDev + 2 * pad;
    const int dotCell = dotDev + 2 * pad;
    const int idleCell = idleDev + 2 * pad;
    const int totalW = 13 * csisCell + 10 * csisCell + 14 * dotCell + 21 * dotCell + idleCell;

    atlas_ = QImage(totalW, cellH, QImage::Format_ARGB32_Premultiplied);
    atlas_.fill(Qt::transparent);

    QPainter painter(&atlas_);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    auto normalize = [&](const QRectF& r) {
        return QRectF(r.x() / totalW, r.y() / cellH, r.width() / totalW, r.height() / cellH);
    };

    int x = 0;
    csisUv_.resize(13);
    for (int level = 0; level <= 12; ++level) {
        const QRectF r(x + pad, pad, csisDev, csisDev);
        drawMarkerTile(painter, r, level, dpr);
        csisUv_[level] = normalize(r);
        x += csisCell;
    }

    // KMA：14 档普通圆点 + 10 个带数字方标（`kmaIntColorBand`，MMI 2..11）。
    // 与 kanameishi 一致：普通态一律圆点，MMI >= 2 换成方标；0/1 两档在
    // `kmaDotColorBand` 中都定义为与 NIED 缺测点相同的深蓝。
    kmaDotUv_.resize(14);
    for (int level = 0; level <= 13; ++level) {
        const double y = (cellH - dotDev) / 2.0;
        const QRectF r(x + pad, y, dotDev, dotDev);
        painter.setPen(Qt::NoPen);
        painter.setBrush(kmaDotColor(level));
        painter.drawEllipse(r);
        kmaDotUv_[level] = normalize(r);
        x += dotCell;
    }
    kmaMarkerUv_.resize(10);
    for (int i = 0; i < 10; ++i) {
        const QRectF r(x + pad, pad, csisDev, csisDev);
        drawKmaMarkerTile(painter, r, i + 2, dpr);
        kmaMarkerUv_[i] = normalize(r);
        x += csisCell;
    }

    const double dotY = (cellH - dotDev) / 2.0;
    niedUv_.resize(21);
    for (int i = 0; i < 21; ++i) {
        const QRectF r(x + pad, dotY, dotDev, dotDev);
        painter.setPen(Qt::NoPen);
        painter.setBrush(niedColor(i));
        painter.drawEllipse(r);
        niedUv_[i] = normalize(r);
        x += dotCell;
    }

    {
        const double idleY = (cellH - idleDev) / 2.0;
        const QRectF r(x + pad, idleY, idleDev, idleDev);
        painter.setBrush(QColor(68, 102, 170, 128));
        painter.setPen(QPen(QColor(68, 102, 170, 179), std::max(1.0, 1.2 * dpr)));
        painter.drawEllipse(r);
        idleUv_ = normalize(r);
    }

    painter.end();
    atlasVersion_ = colorVersion_;
    atlasDpr_ = dpr;
}

void StationMapItem::drawMarkerTile(QPainter& painter, const QRectF& rect, int level,
                                    qreal dpr) const {
    drawLabeledMarker(painter, rect, csisColor(level), QString::number(level), level >= 5, dpr);
}

void StationMapItem::drawKmaMarkerTile(QPainter& painter, const QRectF& rect, int mmi,
                                       qreal dpr) const {
    drawLabeledMarker(painter, rect, kmaColor(mmi + 2), QString::number(mmi), mmi >= 5, dpr);
}

QColor StationMapItem::csisColor(int level) const {
    if (csisColors_.isEmpty()) return QColor(QStringLiteral("#9F9F9F"));
    const int idx = qBound(0, level, csisColors_.size() - 1);
    return QColor(csisColors_.at(idx).toString());
}

QColor StationMapItem::kmaColor(int level) const {
    if (kmaColors_.isEmpty()) return QColor(QStringLiteral("#9F9F9F"));
    const int idx = qBound(0, level, kmaColors_.size() - 1);
    return QColor(kmaColors_.at(idx).toString());
}

QColor StationMapItem::kmaDotColor(int level) const {
    if (kmaDotColors_.isEmpty()) return QColor(QStringLiteral("#0003cf"));
    const int idx = qBound(0, level, kmaDotColors_.size() - 1);
    return QColor(kmaDotColors_.at(idx).toString());
}

QColor StationMapItem::niedColor(int idx) const {
    if (niedColors_.isEmpty()) return QColor(QStringLiteral("#0003cf"));
    const int i = qBound(0, idx, niedColors_.size() - 1);
    return QColor(niedColors_.at(i).toString());
}

} // namespace komira
