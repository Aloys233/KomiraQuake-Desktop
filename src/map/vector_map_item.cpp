#include "map/vector_map_item.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QMetaObject>
#include <QPainter>
#include <QPointer>
#include <QQuickWindow>
#include <QSGTransformNode>
#include <QSGGeometry>
#include <QSGVertexColorMaterial>

#include <QSGGeometryNode>
#include <QSGImageNode>
#include <QSGTexture>
#include <QThreadPool>
#include <QVector>
#include <QtMath>

#include <algorithm>
#include <climits>
#include <cmath>
#include <utility>

#include "core/coordinate_transform.h"
#include "core/intensity_palette.h"

namespace komira {
namespace {

/// 与 MapView.qml / MapScreen.kt 一致的墨卡托纬度截断。
constexpr double kLatLimit = 85.05112878;
/// LOD 的目标误差（**设备像素**）：简化后顶点偏离原折线的最大距离。
/// 0.5 px 低于肉眼可辨阈值（数据本身量化步长就有 ~0.85 m）。
constexpr double kLodScreenTolerancePx = 0.5;
/// 档位范围：tolRef = 2^(-tier) 个参考层级世界像素。最粗 64 ref px（全国概览再往外缩
/// 时才用得着），最细 1 ref px —— 再细的简化已无收益，直接用全精度。
constexpr int kMinTier = -6;
constexpr int kMaxTier = 0;

/// 纬度 -> 墨卡托 Y（0..size）。公式与 MapView.qml 的 projY 逐字对应。
double projY(double lat, double size) {
    const double rad = qBound(-kLatLimit, lat, kLatLimit) * M_PI / 180.0;
    const double y = (1.0 - std::log(std::tan(rad) + 1.0 / std::cos(rad)) / M_PI) / 2.0;
    return y * size;
}
/// 经度 -> 墨卡托 X（0..size）。与 MapView.qml 的 projX 逐字对应。
double projX(double lon, double size) { return (lon + 180.0) / 360.0 * size; }

/// 世界像素 X -> 经度。
double unprojLon(double x, double size) { return x / size * 360.0 - 180.0; }
/// 世界像素 Y -> 纬度。
double unprojLat(double y, double size) {
    const double n = M_PI - 2.0 * M_PI * y / size;
    return 180.0 / M_PI * std::atan(0.5 * (std::exp(n) - std::exp(-n)));
}

/// 点到线段的最短距离平方（全部在参考层级世界像素平面上）。
inline double squaredDistanceToSegment(const QPointF& p, const QPointF& a, const QPointF& b) {
    const double dx = b.x() - a.x();
    const double dy = b.y() - a.y();
    const double len2 = dx * dx + dy * dy;
    const double px = p.x() - a.x();
    const double py = p.y() - a.y();
    if (len2 <= 1e-12) return px * px + py * py;
    double t = (px * dx + py * dy) / len2;
    t = std::max(0.0, std::min(1.0, t));
    const double ex = px - t * dx;
    const double ey = py - t * dy;
    return ex * ex + ey * ey;
}

/// Douglas–Peucker 简化（迭代版，避免长环上的深递归）。
///
/// 只丢顶点、不移动顶点，因此**不会**产生新的自相交或把环翻面；`tol` 是允许的最大
/// 垂距（参考层级世界像素）。首尾点恒保留，闭环的合法性不变。
void simplifyRing(const std::vector<QPointF>& in, double tol, std::vector<QPointF>& out) {
    const int n = static_cast<int>(in.size());
    out.clear();
    if (n < 3 || tol <= 0.0) {
        out = in;
        return;
    }
    std::vector<char> keep(static_cast<std::size_t>(n), 0);
    keep[0] = 1;
    keep[static_cast<std::size_t>(n) - 1] = 1;
    std::vector<std::pair<int, int>> stack;
    stack.emplace_back(0, n - 1);
    const double tol2 = tol * tol;
    while (!stack.empty()) {
        const auto span = stack.back();
        stack.pop_back();
        const int a = span.first;
        const int b = span.second;
        if (b <= a + 1) continue;
        double best = -1.0;
        int bestIndex = -1;
        for (int i = a + 1; i < b; ++i) {
            const double d2 = squaredDistanceToSegment(in[static_cast<std::size_t>(i)],
                                                       in[static_cast<std::size_t>(a)],
                                                       in[static_cast<std::size_t>(b)]);
            if (d2 > best) {
                best = d2;
                bestIndex = i;
            }
        }
        if (best > tol2 && bestIndex > 0) {
            keep[static_cast<std::size_t>(bestIndex)] = 1;
            stack.emplace_back(a, bestIndex);
            stack.emplace_back(bestIndex, b);
        }
    }
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        if (keep[static_cast<std::size_t>(i)]) out.push_back(in[static_cast<std::size_t>(i)]);
    }
    if (out.size() < 3) out = in;   // 极端形状：宁可保留原环，也不要退化掉
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
std::unordered_map<std::string, VectorMapItem::LayerGeometryPtr> VectorMapItem::baseCaches_;
std::unordered_map<std::string, char> VectorMapItem::baseInFlight_;
std::unordered_map<std::string, VectorMapItem::TierEntry> VectorMapItem::tierCaches_;
std::unordered_map<std::string, char> VectorMapItem::tierInFlight_;
std::vector<QPointer<VectorMapItem>> VectorMapItem::waiters_;
QMutex VectorMapItem::meshCacheMutex_;
std::unordered_map<std::string, VectorMeshBuilder::LayerPtr> VectorMapItem::meshCaches_;
std::unordered_map<std::string, char> VectorMapItem::meshInFlight_;
qint64 VectorMapItem::tierBytes_ = 0;
quint64 VectorMapItem::useCounter_ = 0;

std::string VectorMapItem::baseKey(const std::string& layer, bool gcjShift) {
    return layer + (gcjShift ? ":gcj" : ":wgs");
}

std::string VectorMapItem::tierKey(const std::string& layer, bool gcjShift, int tier) {
    return baseKey(layer, gcjShift) + ":" + std::to_string(tier);
}

VectorMapItem::VectorMapItem(QQuickItem* parent) : QQuickItem(parent) {
    // QML 里尺寸由 anchors 决定，这里不自己算。
    setFlag(ItemHasContents, true);
    // 默认不裁剪到自身边界之外（地图会画出可视区边缘）。
}

VectorMapItem::~VectorMapItem() = default;

void VectorMapItem::componentComplete() {
    QQuickItem::componentComplete();
    // 在 GUI 线程完成加载与几何请求：后续所有异步任务都从 GUI 线程发起，
    // 生命周期受控（不会出现「渲染线程拿着 this 去后台线程」）。
    ensureLoaded();
}

// ---------------------------------------------------------------------------
// 属性 setter：一律在 GUI 线程，改完字段后 publishState() 发布给渲染线程。
// ---------------------------------------------------------------------------

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
    invalidateRenderCache();
    emit styleChanged();
    update();
}

void VectorMapItem::setStrokeColor(const QColor& v) {
    if (strokeColor_ == v) return;
    strokeColor_ = v;
    invalidateRenderCache();
    emit styleChanged();
    update();
}

void VectorMapItem::setStrokeWidth(qreal v) {
    if (qFuzzyCompare(strokeWidth_, v)) return;
    strokeWidth_ = v;
    invalidateRenderCache();
    emit styleChanged();
    update();
}

void VectorMapItem::setStrokeScale(qreal v) {
    if (qFuzzyCompare(strokeScale_, v)) return;
    strokeScale_ = v;
    invalidateRenderCache();
    emit styleChanged();
    update();
}

void VectorMapItem::setDistrictIntensity(const QVariantMap& v) {
    if (districtIntensity_ == v) return;
    districtIntensity_ = v;
    invalidateRenderCache();
    publishState();
    emit districtIntensityChanged();
    update();
}

void VectorMapItem::setLodEnabled(bool v) {
    if (lodEnabled_ == v) return;
    lodEnabled_ = v;
    cacheValid_ = false;
    publishState();
    emit lodEnabledChanged();
    update();
}

void VectorMapItem::itemChange(ItemChange change, const ItemChangeData& value) {
    if (change == ItemVisibleHasChanged && !value.boolValue) {
        // 隐藏即释放：矢量叠加关掉后没必要常驻十几 MB，重新显示时下一帧重建即可。
        dropCache();
    }
    QQuickItem::itemChange(change, value);
}

void VectorMapItem::invalidateRenderCache() {
    ++styleVersion_;
    cacheValid_ = false;
    publishState();
}

// ---------------------------------------------------------------------------
// 几何：加载、（后台）投影、（后台）LOD 档位
// ---------------------------------------------------------------------------

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
    cacheValid_ = false;
    textureValid_ = false;
    geometryReady_ = false;
    {
        QMutexLocker locker(&stateMutex_);
        published_.geometry.reset();
        published_.geometryReady = false;
    }
    requestBaseGeometry();
}

void VectorMapItem::dropCache() {
    cacheValid_ = false;
    textureValid_ = false;
    cacheTier_ = kFullTier + 1;
    if (!cache_.isNull()) cache_ = QImage();
}

void VectorMapItem::publishState() {
    RenderState next;
    next.layer = layerName_;
    next.gcjShift = gcjShift_;
    next.lodEnabled = lodEnabled_;
    next.styleVersion = styleVersion_;
    next.fill = fillColor_;
    next.stroke = strokeColor_;
    next.strokeWidth = strokeWidth_;
    next.strokeScale = strokeScale_;
    next.geometry = geometry_;
    next.geometryReady = geometry_ != nullptr;

    // 区县着色：把 QVariantMap 的 QString 键一次性映射到要素下标。原先这一步在渲染线程
    // 每帧做 1011 次 QString::fromStdString + 哈希查找（实测 0.12 ms，还要分配上千次）。
    if (layerName_ == QStringLiteral("district") && geometry_ && !districtIntensity_.isEmpty()) {
        const auto& features = geometry_->features;
        std::unordered_map<std::string, int> indexByFeature;
        indexByFeature.reserve(features.size());
        for (std::size_t i = 0; i < features.size(); ++i) {
            indexByFeature.emplace(features[i].name, static_cast<int>(i));
        }
        next.shadedIndices.reserve(static_cast<std::size_t>(districtIntensity_.size()));
        next.shadedRaw.reserve(static_cast<std::size_t>(districtIntensity_.size()));
        for (auto it = districtIntensity_.constBegin(); it != districtIntensity_.constEnd(); ++it) {
            const auto found = indexByFeature.find(it.key().toStdString());
            if (found == indexByFeature.end()) continue;
            next.shadedIndices.push_back(found->second);
            next.shadedRaw.push_back(it.value().toDouble());
        }
    }

    QMutexLocker locker(&stateMutex_);
    published_ = std::move(next);
}

void VectorMapItem::requestBaseGeometry() {
    if (!ready_ || layerName_.isEmpty()) return;
    const std::string layer = layerName_.toStdString();
    const std::string key = baseKey(layer, gcjShift_);

    {
        QMutexLocker locker(&geometryCacheMutex_);
        const auto cached = baseCaches_.find(key);
        if (cached != baseCaches_.end()) {
            geometry_ = cached->second;
            geometryReady_ = true;
            publishState();
            return;
        }
        if (baseInFlight_.find(key) == baseInFlight_.end()) baseInFlight_.emplace(key, 1);
        waiters_.push_back(QPointer<VectorMapItem>(this));
    }

    // 34 万点投影 + GCJ 纠偏实测 48–90 ms，DP 简化 20–45 ms：绝不能落在渲染线程上。
    // 任务只捕获**不可变数据**（.kvb 快照 + 层名），完成后再排回 GUI 线程改成员。
    const std::shared_ptr<VectorMapData> snapshot = data_;
    const bool gcj = gcjShift_;
    const QPointer<VectorMapItem> self(this);
    QThreadPool::globalInstance()->start([self, snapshot, layer, key, gcj]() {
        LayerGeometryPtr built = buildBaseGeometry(*snapshot, layer, gcj);
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, key, built]() {
                if (!self) return;
                self->onBaseGeometryBuilt(key, built);
            },
            Qt::QueuedConnection);
    });
}

void VectorMapItem::onBaseGeometryBuilt(const std::string& key, const LayerGeometryPtr& built) {
    std::vector<QPointer<VectorMapItem>> waiters;
    {
        QMutexLocker locker(&geometryCacheMutex_);
        baseCaches_[key] = built;
        baseInFlight_.erase(key);
        waiters.swap(waiters_);
    }
    // 同一份几何可能被多个 item 等待（历史行为里 world/province/district 各建一份，
    // GCJ 切换与重复建图也会撞上）：逐个唤醒，让它们各自从缓存里取。
    for (const auto& waiter : waiters) {
        if (!waiter || waiter == this) continue;
        QMetaObject::invokeMethod(
            waiter,
            [ptr = waiter.data()]() {
                ptr->geometry_.reset();
                ptr->requestBaseGeometry();
            },
            Qt::QueuedConnection);
    }

    if (baseKey(layerName_.toStdString(), gcjShift_) != key) return;   // 期间换层了
    if (geometry_ != built) {
        geometry_ = built;
        geometryReady_ = true;
        publishState();
    }
    emit geometryReadyChanged();
    update();
}

VectorMapItem::LodTierPtr VectorMapItem::resolveTier(const std::string& layer, bool gcj, int wanted) {
    const std::string wantedKey = tierKey(layer, gcj, wanted);
    bool needBuild = false;
    lodTierOutOfDate_ = false;
    {
        QMutexLocker locker(&geometryCacheMutex_);
        const auto exact = tierCaches_.find(wantedKey);
        if (exact != tierCaches_.end()) {
            exact->second.lastUse = ++useCounter_;
            return exact->second.tier;
        }
        // 想要的档位还没建好：先退回已就绪的最近档位（宁可用稍粗的几何，也不要整层空白）。
        LodTierPtr best;
        const std::string prefix = baseKey(layer, gcj) + ":";
        int bestDistance = INT_MAX;
        for (auto& entry : tierCaches_) {
            if (entry.first.rfind(prefix, 0) != 0) continue;
            const int distance = std::abs(entry.second.tier->tier - wanted);
            if (distance < bestDistance) {
                bestDistance = distance;
                best = entry.second.tier;
                entry.second.lastUse = ++useCounter_;
            }
        }
        if (tierInFlight_.find(wantedKey) == tierInFlight_.end()) {
            tierInFlight_.emplace(wantedKey, 1);
            needBuild = true;
        }
        // 想要的档位还没建好（缩放动画/首次进入常见）：**必须**退回最近档位继续画，
        // 否则这一帧整层空白 —— 早期版本在这里 return nullptr，表现为「缩放时地图闪一下」。
        if (best) {
            lodTierOutOfDate_ = true;
            if (needBuild) {
                const QPointer<VectorMapItem> self(this);
                QMetaObject::invokeMethod(
                    QCoreApplication::instance(),
                    [self, wantedKey, layer, gcj, wanted]() {
                        if (self) self->startTierBuild(wantedKey, layer, gcj, wanted);
                    },
                    Qt::QueuedConnection);
            }
            return best;
        }
    }

    if (needBuild) {
        // 从渲染线程发起时不能捕获 this 去做重活：排回 GUI 线程，由它持有生命周期。
        const QPointer<VectorMapItem> self(this);
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, wantedKey, layer, gcj, wanted]() {
                if (self) self->startTierBuild(wantedKey, layer, gcj, wanted);
            },
            Qt::QueuedConnection);
    }
    // 一个档位都没有（几何刚建好、首个档位还在算）：本帧留空，下一帧就好。
    return nullptr;
}

void VectorMapItem::startTierBuild(const std::string& key, const std::string& layer, bool gcj,
                                   int tier) {
    LayerGeometryPtr base;
    {
        QMutexLocker locker(&geometryCacheMutex_);
        const auto it = baseCaches_.find(baseKey(layer, gcj));
        if (it != baseCaches_.end()) base = it->second;
        if (!base) {
            tierInFlight_.erase(key);
            return;
        }
    }
    const QPointer<VectorMapItem> self(this);
    QThreadPool::globalInstance()->start([self, base, key, tier]() {
        LodTierPtr built = buildTier(*base, tier);
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, key, built]() {
                if (!self) return;
                self->onTierBuilt(key, built);
            },
            Qt::QueuedConnection);
    });
}

void VectorMapItem::onTierBuilt(const std::string& key, const LodTierPtr& built) {
    {
        QMutexLocker locker(&geometryCacheMutex_);
        if (tierCaches_.find(key) == tierCaches_.end()) {
            tierBytes_ += built->bytes;
            tierCaches_.emplace(key, TierEntry{built, ++useCounter_});
        }
        tierInFlight_.erase(key);
        evictTiersLocked();
    }
    // 新档位只是「更合适的精度」，不是「必须立刻重绘」：当前位图在几何上仍然正确
    // （缓存里的路径来自上一档，画出来只是略粗/略细）。相机静止时才值得换一张，
    // 缩放/拖动途中重绘等于给动画加卡顿，交给下一次自然重建即可。
    if (cacheValid_ && cacheTier_ != built->tier && cameraAtRest()) {
        cacheValid_ = false;
        update();
    }
}

bool VectorMapItem::cameraAtRest() const {
    return std::abs(zoom_ - std::round(zoom_)) < 1e-9;
}

void VectorMapItem::evictTiersLocked() {
    while (tierBytes_ > kTierBudgetBytes && tierCaches_.size() > 1) {
        auto victim = tierCaches_.end();
        for (auto it = tierCaches_.begin(); it != tierCaches_.end(); ++it) {
            if (victim == tierCaches_.end() || it->second.lastUse < victim->second.lastUse) {
                victim = it;
            }
        }
        if (victim == tierCaches_.end()) break;
        tierBytes_ -= victim->second.tier ? victim->second.tier->bytes : 0;
        tierCaches_.erase(victim);
    }
}

// ---------------------------------------------------------------------------
// 纯函数构建（任意线程安全）
// ---------------------------------------------------------------------------

VectorMapItem::LayerGeometryPtr VectorMapItem::buildBaseGeometry(const VectorMapData& data,
                                                                 const std::string& layerName,
                                                                 bool gcjShift) {
    const VectorLayer* layer = data.layer(layerName);
    auto geometry = std::make_shared<LayerGeometry>();
    if (!layer) return geometry;

    // 所有几何按**固定参考层级**投影一次（见类注释）：墨卡托对 worldSize 线性，
    // 绘制时乘 2^(zoom-kRefZoom) 即得任意连续 zoom 的精确结果。
    const double size = worldSize(kRefZoom);
    // GCJ 纠偏只对区县层生效：世界层含全球要素，outOfChina 是矩形 bbox，纠偏会把
    // 蒙古/印度一起平移（历史行为，勿改）。
    const bool applyGcj = gcjShift && layerName == "district";

    geometry->features.reserve(layer->features.size());
    for (const VectorFeature& f : layer->features) {
        Feature feature;
        feature.name = f.name;
        bool first = true;
        feature.rings.reserve(f.polygon.size());
        for (const GeoRing& ring : f.polygon) {
            if (ring.size() < 3) continue;
            std::vector<QPointF> points;
            points.reserve(ring.size());
            for (const auto& point : ring) {
                double la = point.first;
                double lo = point.second;
                if (applyGcj) {
                    const auto shifted = CoordinateTransform::wgs84ToGcj02(la, lo);
                    la = shifted.first;
                    lo = shifted.second;
                }
                points.emplace_back(projX(lo, size), projY(la, size));
            }
            // 包围盒用**原始经纬度**算：经度副本（±360°）判定与纬度裁剪都基于它。
            for (const auto& point : ring) {
                if (first) {
                    feature.bounds.minLat = feature.bounds.maxLat = point.first;
                    feature.bounds.minLon = feature.bounds.maxLon = point.second;
                    first = false;
                } else {
                    feature.bounds.minLat = std::min(feature.bounds.minLat, point.first);
                    feature.bounds.maxLat = std::max(feature.bounds.maxLat, point.first);
                    feature.bounds.minLon = std::min(feature.bounds.minLon, point.second);
                    feature.bounds.maxLon = std::max(feature.bounds.maxLon, point.second);
                }
            }
            feature.rings.push_back(std::move(points));
        }
        if (feature.rings.empty()) continue;
        geometry->features.push_back(std::move(feature));
    }
    return geometry;
}

VectorMapItem::LodTierPtr VectorMapItem::buildTier(const LayerGeometry& base, int tier) {
    auto built = std::make_shared<LodTier>();
    built->tier = tier;
    // 档位 t 的容差是 2^(-t) 参考层级世界像素；kFullTier 表示不简化。
    const double tol = tier >= kFullTier ? 0.0 : std::pow(2.0, -double(tier));
    built->paths.reserve(base.features.size());

    std::vector<QPointF> simplified;
    qint64 elements = 0;
    for (const Feature& feature : base.features) {
        QPainterPath path;
        path.setFillRule(Qt::OddEvenFill);   // 洞靠 even-odd 自然镂空
        bool any = false;
        for (const auto& ring : feature.rings) {
            if (tol > 0.0) {
                simplifyRing(ring, tol, simplified);
            } else {
                simplified = ring;
            }
            if (simplified.size() < 3) continue;
            path.addPolygon(QPolygonF(QVector<QPointF>(simplified.begin(), simplified.end())));
            any = true;
        }
        if (!any) continue;
        elements += path.elementCount();
        built->paths.push_back(std::move(path));
    }
    built->bytes = elements * qint64(sizeof(QPainterPath::Element));
    return built;
}

// ---------------------------------------------------------------------------
// 渲染线程
// ---------------------------------------------------------------------------

bool VectorMapItem::hasDrawableContent(const RenderState& state) {
    if (!state.geometry || state.geometry->features.empty()) return false;
    if (state.layer == QStringLiteral("district")) {
        // 区县层平时完全透明：没有着色区县时一个面都不该画（1011 面全描边实测 24 ms）。
        return !state.shadedIndices.empty();
    }
    const bool wantsStroke = state.strokeWidth > 0.0 && state.stroke.alpha() > 0;
    return state.fill.alpha() > 0 || wantsStroke;
}



// ---------------------------------------------------------------------------
// GPU 路径
// ---------------------------------------------------------------------------
//
// 与 CPU 路径（离屏位图）的分工：
//   · 有 GPU：把三角化网格直接交给场景图，平移/缩放只改节点变换，**不重建几何**；
//     按分块做视口裁剪，一帧只提交可见块的索引。
//   · 软件后端：仍走 QPainter 光栅化（软件渲染器不支持自定义材质，且那条路径是给
//     CI/远程桌面兜底的，性能不敏感）。
//
// 两条路径共用同一份 .kvb 数据与同一套 LOD 容差语义，因此画面一致。

qint64 VectorMapItem::meshBytesForTest() const {
    return mesh_ ? static_cast<qint64>(mesh_->bytes()) : 0;
}

void VectorMapItem::startMeshBuild(const std::string& key, const std::string& layer, bool gcj,
                                   double tolRef, const VectorMeshBuilder::Style& style) {
    const std::shared_ptr<VectorMapData> snapshot = data_;
    if (!snapshot) return;
    const QPointer<VectorMapItem> self(this);
    // 三角化是纯计算（真实数据上 district 层约 150 ms），绝不能在渲染线程做。
    QThreadPool::globalInstance()->start([self, snapshot, layer, gcj, tolRef, style, key]() {
        auto built = VectorMeshBuilder::build(*snapshot, layer, gcj, tolRef, style);
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, key, built]() {
                if (!self) return;
                self->onMeshBuilt(key, built);
            },
            Qt::QueuedConnection);
    });
}

void VectorMapItem::onMeshBuilt(const std::string& key, const VectorMeshBuilder::LayerPtr& built) {
    {
        QMutexLocker locker(&meshCacheMutex_);
        meshCaches_[key] = built;
        meshInFlight_.erase(key);
    }
    // 网格就绪后必须主动请求一帧。
    //
    // 不能只判 gpuPathActive_：**首次**构建时它还是 false（因为没有网格就走不到
    // updateGpuNode），若在这里跳过 update()，就永远没有下一帧来取这份网格 ——
    // 死锁，表现为 GPU 路径完全不生效。
    update();
}

VectorMeshBuilder::LayerPtr VectorMapItem::resolveMesh(const std::string& layer, bool gcj,
                                                       double tolRef,
                                                       const VectorMeshBuilder::Style& style,
                                                       bool* needAnotherFrame) {
    // 容差量化到 2 的幂档：否则缩放动画里每一帧的 tolRef 都不同，等于每帧重建几何。
    //
    // 档位语义必须与 CPU 路径**逐字一致**（见 updatePaintNode 里的 wantedTier）：
    // 算出的 tier 一旦 >= kMaxTier，说明 0.5 设备像素的误差预算已细于 1 参考层级
    // 像素，此时直接用全精度（kFullTier），而不是夹到 kMaxTier —— 后者会用一个
    // 比预算更粗的档，拉近后线条明显发钝。
    int tier = kFullTier;
    if (tolRef > 0.0) {
        const int computed = static_cast<int>(std::lround(-std::log2(tolRef)));
        if (computed < kMaxTier) tier = std::max(kMinTier, computed);
    }
    // key 必须包含样式：颜色与描边宽度都烘焙进了顶点，换主题/改线宽要重建网格。
    const std::string styleKey = ":" + std::to_string(style.fillR) + "," +
                                 std::to_string(style.fillG) + "," + std::to_string(style.fillB) +
                                 "," + std::to_string(style.fillA) + ":" +
                                 std::to_string(style.strokeR) + "," +
                                 std::to_string(style.strokeG) + "," +
                                 std::to_string(style.strokeB) + "," +
                                 std::to_string(style.strokeA) + ":" +
                                 std::to_string(int(style.strokeWidthPx * 100));
    const std::string key = baseKey(layer, gcj) + ":gpu:" + std::to_string(tier) + styleKey;
    if (needAnotherFrame) *needAnotherFrame = false;

    {
        QMutexLocker locker(&meshCacheMutex_);
        const auto found = meshCaches_.find(key);
        if (found != meshCaches_.end()) {
            {
                QMutexLocker meshLock(&meshMutex_);
                mesh_ = found->second;
                meshKey_ = key;
                // kFullTier（不简化）记 0，与 CPU 路径 lodToleranceForTest 的语义一致；
            // 否则测试无法区分「全精度」与「最细档」。
            meshTolRef_ = (tier >= kFullTier) ? 0.0 : std::pow(2.0, -double(tier));
            }
            return found->second;
        }
        if (meshInFlight_.find(key) == meshInFlight_.end()) {
            meshInFlight_.emplace(key, 1);
            if (needAnotherFrame) *needAnotherFrame = true;
            // 构建必须用**量化后**的容差：key 是由量化档位算出来的，若拿原始 tolRef
            // 去建，下次按 key 查时内容与档位不符（早期版本因此永远命中不了缓存，
            // 表现为 GPU 路径一直不生效）。
            const double tierTol = tier >= kFullTier ? 0.0 : std::pow(2.0, -double(tier));
            const QPointer<VectorMapItem> self(this);
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [self, key, layer, gcj, tierTol, style]() {
                    if (self) self->startMeshBuild(key, layer, gcj, tierTol, style);
                },
                Qt::QueuedConnection);
        }
    }
    // 该档还没建好：继续用当前持有的网格（可能更粗），不要整层空白。
    QMutexLocker meshLock(&meshMutex_);
    return mesh_;
}

QSGNode* VectorMapItem::updateGpuNode(QSGNode* oldNode, const RenderState& state, double lat,
                                      double lon, double zoom, double scale) {
    // 锁内拷一份 shared_ptr：渲染线程遍历期间 GUI 线程可能正在换网格（换档/换层）。
    // 拷出来后即使 GUI 线程把 mesh_ 换掉，本地这份仍保活，不会悬垂。
    VectorMeshBuilder::LayerPtr mesh;
    double tolRefSnapshot = 0.0;
    {
        QMutexLocker locker(&meshMutex_);
        mesh = mesh_;
        tolRefSnapshot = meshTolRef_;
    }
    if (!mesh) return nullptr;

    const double refSize = worldSize(kRefZoom);
    const double w = width();
    const double h = height();
    const double centerWX = projX(lon, refSize);
    const double centerWY = projY(lat, refSize);
    const double halfW = w / 2.0 / scale;
    const double halfH = h / 2.0 / scale;
    const double viewX0 = centerWX - halfW;
    const double viewX1 = centerWX + halfW;
    const double viewY0 = centerWY - halfH;
    const double viewY1 = centerWY + halfH;

    const bool isDistrict = state.layer == QStringLiteral("district");
    const bool wantsStroke = state.strokeWidth > 0.0 && state.stroke.alpha() > 0;
    // 区县层只在有震度时画（平时完全透明），与 CPU 路径的 hasDrawableContent 一致。
    if (isDistrict && state.shadedIndices.empty()) {
        drawnTiles_ = 0;
        submittedTriangles_ = 0;
        return oldNode;
    }

    if (!window()) return oldNode;
    const qreal dpr = window()->effectiveDevicePixelRatio();
    const QSizeF viewportDev(w * dpr, h * dpr);
    if (!(dpr > 0.0) || w <= 0 || h <= 0) return oldNode;

    // 节点结构：root -> [QSGTransformNode(每块) -> QSGGeometryNode]
    //
    // 几何只在 mesh/样式真的变化时重建一次，相机变化只更新变换矩阵。
    QSGNode* root = oldNode ? oldNode : new QSGNode;

    qint64 visibleSignature = 0;
    int visibleCount = 0;
    const double batchSpan =
        VectorMeshBuilder::kTileWorldPx * VectorMeshBuilder::kMaxTilesPerBatch;
    for (const auto& batch : mesh->batches) {
        const bool visible = batch.originX <= viewX1 && batch.originX + batchSpan >= viewX0 &&
                             batch.originY <= viewY1 && batch.originY + batchSpan >= viewY0;
        if (!visible || batch.indices.size() < 3) continue;
        ++visibleCount;
        visibleSignature = visibleSignature * 131 + static_cast<qint64>(batch.originX) * 31 +
                           static_cast<qint64>(batch.originY);
    }
    visibleSignature = visibleSignature * 131 + visibleCount;

    const bool geometryStale = !uploadedGeometryValid_ || uploadedMeshKey_ != meshKey_ ||
                               uploadedTileSignature_ != visibleSignature ||
                               root->childCount() == 0;
    if (geometryStale) {
        while (QSGNode* c = root->firstChild()) {
            root->removeChildNode(c);
            delete c;
        }
        uploadedMeshKey_ = meshKey_;
        uploadedTileSignature_ = visibleSignature;
        uploadedGeometryValid_ = true;
        ++geometryUploads_;
    }

    int drawn = 0;
    qint64 tris = 0;
    QSGNode* child = root->firstChild();

    for (const auto& batch : mesh->batches) {
        const bool visible = batch.originX <= viewX1 && batch.originX + batchSpan >= viewX0 &&
                             batch.originY <= viewY1 && batch.originY + batchSpan >= viewY0;
        if (!visible || batch.indices.size() < 3) continue;

        QMatrix4x4 m;
        m.translate(static_cast<float>(w / 2.0 + (batch.originX - centerWX) * scale),
                    static_cast<float>(h / 2.0 + (batch.originY - centerWY) * scale));
        m.scale(static_cast<float>(scale), static_cast<float>(scale));

        QSGTransformNode* xform = nullptr;
        if (geometryStale) {
            xform = new QSGTransformNode;
            root->appendChildNode(xform);

            auto* node = new QSGGeometryNode;
            auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(),
                                             static_cast<int>(batch.vertices.size()),
                                             static_cast<int>(batch.indices.size()),
                                             QSGGeometry::UnsignedIntType);
            geometry->setDrawingMode(QSGGeometry::DrawTriangles);
            auto* v = geometry->vertexDataAsColoredPoint2D();
            for (std::size_t i = 0; i < batch.vertices.size(); ++i) {
                const auto& src = batch.vertices[i];
                v[i].set(src.x, src.y, src.r, src.g, src.b, src.a);
            }
            auto* idx = geometry->indexDataAsUInt();
            for (std::size_t i = 0; i < batch.indices.size(); ++i) idx[i] = batch.indices[i];
            geometry->setVertexDataPattern(QSGGeometry::StaticPattern);
            geometry->setIndexDataPattern(QSGGeometry::StaticPattern);
            node->setGeometry(geometry);
            node->setFlag(QSGNode::OwnsGeometry);
            auto* material = new QSGVertexColorMaterial;
            material->setFlag(QSGMaterial::Blending);
            node->setMaterial(material);
            node->setFlag(QSGNode::OwnsMaterial);
            xform->appendChildNode(node);
        } else {
            while (child && child->type() != QSGNode::TransformNodeType) child = child->nextSibling();
            xform = static_cast<QSGTransformNode*>(child);
            if (!xform) continue;
            child = xform->nextSibling();
        }
        xform->setMatrix(m);

        ++drawn;
        tris += static_cast<qint64>(batch.indices.size() / 3);
    }

    drawnTiles_ = drawn;
    submittedTriangles_ = tris;

    // 记录本帧真实使用的变换，供回归测试断言「与震中/定位标记同源」。
    drawnScale_ = scale;
    drawnCenterX_ = centerWX;
    drawnCenterY_ = centerWY;
    drawnTolRef_ = tolRefSnapshot > 0.0 ? tolRefSnapshot : 0.0;
    return root;
}

QSGNode* VectorMapItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    ++paintNodeCalls_;

    // 相机与样式只读一次：GUI 线程可能正在写，锁内拷一份局部副本再画。
    RenderState state;
    {
        QMutexLocker locker(&stateMutex_);
        state = published_;
    }
    const double lat = centerLat_;
    const double lon = centerLon_;
    const double zoom = zoom_;
    const bool lodEnabled = state.lodEnabled;

    if (!state.geometryReady || !state.geometry) {
        if (!ready_) {
            // 正常情况下 componentComplete() 已在 GUI 线程加载完；这里只兜底。
            const QPointer<VectorMapItem> self(this);
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [self]() {
                    if (self) self->ensureLoaded();
                },
                Qt::QueuedConnection);
        }
        return nullptr;
    }
    if (!hasDrawableContent(state) || width() <= 0 || height() <= 0) {
        dropCache();
        return nullptr;
    }

    // GPU 路径：不建离屏位图，直接把三角化网格交给场景图。软件后端仍走下面的
    // QPainter 光栅化（软件渲染器不支持自定义材质）。
    const bool softwareBackend =
        !window() || window()->rendererInterface()->graphicsApi() == QSGRendererInterface::Software;
    if (!softwareBackend) {
        const double gpuScale = std::pow(2.0, zoom - kRefZoom);
        const qreal dprNow = window() ? window()->effectiveDevicePixelRatio() : 1.0;
        double tolRef = 0.0;
        if (lodEnabled) {
            tolRef = kLodScreenTolerancePx / (gpuScale * dprNow);
        }
        // 样式烘焙进顶点：颜色与描边宽度都算进去，换主题/改线宽会重建网格。
        VectorMeshBuilder::Style style;
        style.fillR = static_cast<uint8_t>(state.fill.red());
        style.fillG = static_cast<uint8_t>(state.fill.green());
        style.fillB = static_cast<uint8_t>(state.fill.blue());
        style.fillA = static_cast<uint8_t>(state.fill.alpha());
        style.strokeR = static_cast<uint8_t>(state.stroke.red());
        style.strokeG = static_cast<uint8_t>(state.stroke.green());
        style.strokeB = static_cast<uint8_t>(state.stroke.blue());
        style.strokeA = static_cast<uint8_t>(state.stroke.alpha());
        style.strokeWidthPx = state.strokeWidth * state.strokeScale;
        style.screenPerWorldPx = gpuScale * dprNow;
        // 样式完全透明（既无填充也无描边）时**不建网格**。
        //
        // 栅格叠加模式下 world 层的 fill 与 stroke 都是 "transparent"：它一个像素
        // 都不画。早期实现照样为它三角化了整层（实测 z6 重建 215 ms、77 个块），
        // 于是每次缩放跨档都要白花 200 ms —— 这正是「开省界就卡」的主因之一。
        if (style.fillA == 0 && (style.strokeA == 0 || style.strokeWidthPx <= 0.0)) {
            {
                QMutexLocker locker(&meshMutex_);
                mesh_.reset();
                meshKey_.clear();
            }
            uploadedGeometryValid_ = false;
            drawnTiles_ = 0;
            submittedTriangles_ = 0;
            return nullptr;
        }

        bool needFrame = false;
        resolveMesh(state.layer.toStdString(), state.gcjShift, tolRef, style, &needFrame);
        if (mesh_) {
            gpuPathActive_ = true;
            cacheValid_ = false;   // GPU 路径不使用离屏位图
            if (!cache_.isNull()) cache_ = QImage();
            return updateGpuNode(oldNode, state, lat, lon, zoom, gpuScale);
        }
        // 网格还在建：本帧留空（下一帧就好）。
        return nullptr;
    }
    gpuPathActive_ = false;

    const double w = width();
    const double h = height();
    const double cw = w + 2 * kCacheMarginPx;
    const double ch = h + 2 * kCacheMarginPx;
    // 渲染尺寸比视口再多留一圈余量：右上角事件列表开合会连续改变控件宽度，
    // 缓存与视口严格等大时每一帧都要整层重绘。
    const double cwRender = cw + kCacheGrowSlackPx;
    const double chRender = ch + kCacheGrowSlackPx;

    // 固定 1x：放大交给 GPU（见 kRasterDpr 的注释）。
    const qreal dpr = kRasterDpr;
    const QSize wantSize(static_cast<int>(std::ceil(cw * dpr)),
                         static_cast<int>(std::ceil(ch * dpr)));

    const double refSize = worldSize(kRefZoom);
    const double scale = std::pow(2.0, zoom - kRefZoom);
    const double centerWX = projX(lon, refSize);
    const double centerWY = projY(lat, refSize);
    const double curWorldX = centerWX * scale;
    const double curWorldY = centerWY * scale;

    // 期望档位：用本帧相机（不是成员）算，避免与 setter 竞争。
    int wantedTier = kFullTier;
    if (lodEnabled) {
        const double scaleWithDpr = scale * dpr;
        const double tolRef = kLodScreenTolerancePx / scaleWithDpr;
        const int tier = static_cast<int>(std::lround(-std::log2(tolRef)));
        if (tier < kMaxTier) wantedTier = std::max(kMinTier, tier);
    }
    const std::string layer = state.layer.toStdString();
    LodTierPtr tier = resolveTier(layer, state.gcjShift, wantedTier);
    if (!tier) {
        // 该档位还在后台构建，且没有任何可退回的档位：本帧留空（下一帧就好）。
        return nullptr;
    }

    // 用 QSGImageNode 而不是 QQuickPaintedItem：后者每次 update() 都要把整张位图
    // 重新 CPU 合成并上传纹理。
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
    if (cacheValid_ && cacheStyleVersion_ == state.styleVersion && cacheDpr_ == dpr &&
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
        renderToCache(dpr, cwRender, chRender, *tier, state, lat, lon, zoom, scale);
        cacheWorldX_ = curWorldX;
        cacheWorldY_ = curWorldY;
        cacheScale_ = scale;
        cacheDpr_ = dpr;
        cacheTier_ = tier->tier;
        cacheStyleVersion_ = state.styleVersion;
        cacheValid_ = true;
        textureValid_ = false;
        ++cacheRebuilds_;
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
    drawnTolRef_ = (tier->tier >= kFullTier) ? 0.0 : std::pow(2.0, -double(tier->tier));

    // 后台档位建好之前先画粗档。**不要**在这里无条件 scheduleUpdate()：那会让场景图
    // 在缩放动画期间每帧都请求新档位、每帧重绘。只在「档位确实变了且缓存是旧精度」
    // 时补一帧，且由 onTierBuilt() 驱动（见那里的注释）。
    if (lodTierOutOfDate_) lodTierOutOfDate_ = false;
    return node;
}

QPointF VectorMapItem::screenPosForTest(double lat, double lon) const {
    const double refSize = worldSize(kRefZoom);
    const double x = (projX(lon, refSize) - drawnCenterX_) * drawnScale_ + width() / 2.0;
    const double y = (projY(lat, refSize) - drawnCenterY_) * drawnScale_ + height() / 2.0;
    return QPointF(x, y);
}

int VectorMapItem::buildTierForTest(double tolRef) {
    LayerGeometryPtr base;
    {
        QMutexLocker locker(&stateMutex_);
        base = published_.geometry;
    }
    if (!base) return -1;
    int tier = kFullTier;
    if (tolRef > 0.0) tier = static_cast<int>(std::lround(-std::log2(tolRef)));
    LodTierPtr built = buildTier(*base, tier);
    const std::string key = tierKey(layerName_.toStdString(), gcjShift_, tier);
    QMutexLocker locker(&geometryCacheMutex_);
    if (tierCaches_.find(key) == tierCaches_.end()) {
        tierBytes_ += built->bytes;
        tierCaches_.emplace(key, TierEntry{built, ++useCounter_});
        evictTiersLocked();
    }
    return static_cast<int>(built->paths.size());
}

void VectorMapItem::scheduleUpdate() {
    // updatePaintNode 跑在渲染线程，而 QQuickItem::update() 会直接改窗口的待更新标记，
    // 不是线程安全的。统一排回对象所属线程再调。
    const QPointer<VectorMapItem> self(this);
    QMetaObject::invokeMethod(
        QCoreApplication::instance(),
        [self]() {
            if (self) self->update();
        },
        Qt::QueuedConnection);
}

void VectorMapItem::renderToCache(qreal dpr, double cw, double ch, const LodTier& tier,
                                  const RenderState& state, double lat, double lon, double zoom,
                                  double scale) {
    const int pw = std::max(1, static_cast<int>(std::ceil(cw * dpr)));
    const int ph = std::max(1, static_cast<int>(std::ceil(ch * dpr)));
    if (cache_.size() != QSize(pw, ph)) cache_ = QImage(pw, ph, QImage::Format_ARGB32_Premultiplied);
    cache_.fill(Qt::transparent);
    QPainter painter(&cache_);
    // 按实际像素比缩放而不是直接用 dpr：像素尺寸取整后略有偏差，用实际比例才能保证
    // 逻辑坐标与 QML 的投影逐像素一致。
    painter.scale(double(pw) / cw, double(ph) / ch);
    drawLayerContent(painter, cw, ch, tier, state, lat, lon, zoom, scale);
    painter.end();
}

void VectorMapItem::drawLayerContent(QPainter& painter, double vw, double vh, const LodTier& tier,
                                     const RenderState& state, double lat, double lon, double zoom,
                                     double scale) {
    Q_UNUSED(zoom);
    const double refSize = worldSize(kRefZoom);
    const double originX = projX(lon, refSize) * scale - vw / 2.0;
    const double originY = projY(lat, refSize) * scale - vh / 2.0;

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

    const auto& features = state.geometry->features;
    const auto& paths = tier.paths;

    painter.save();
    // 整体缩放 + 平移：顶点不再逐帧重算。
    painter.translate(-originX, -originY);
    painter.scale(scale, scale);
    // 线宽按屏幕像素折算，否则缩小时描边会粗得糊住底图。
    painter.setPen(Qt::NoPen);
    painter.setBrush(state.fill);

    const bool isDistrict = state.layer == QStringLiteral("district");
    const bool wantsStroke = state.strokeWidth > 0.0 && state.stroke.alpha() > 0;
    // 填充免抗锯齿：多边形填充的 AA 是这一层最大的单笔开销（实测占 35–55%），
    // 而它的锯齿边会被描边整条盖住，看不出来。只有「没有描边可盖」时才必须开。
    const bool fillAntialias = !wantsStroke;

    QPen pen;
    if (wantsStroke) {
        pen = QPen(state.stroke);
        pen.setWidthF(state.strokeWidth * state.strokeScale / scale);
        pen.setJoinStyle(Qt::RoundJoin);
    }

    // 数据只含 [-180,180] 的一份几何；相机经度被 QML 夹在 ±180，但视口可越界。
    // 视口跨越日界线时，把同一要素按 -360/0/+360 三份分别判定并绘制，缺的那半幅
    // 就补了回来 —— 与栅格瓦片的 wrapTile 行为一致。中/日等远离日界线的要素
    // 只会命中 0 号那份，开销可忽略。
    const auto drawCopies = [&](std::size_t index) {
        const GeoBounds& b = features[index].bounds;
        if (b.minLat > viewNorth || b.maxLat < viewSouth) return;
        for (int copy = -1; copy <= 1; ++copy) {
            const double lonShift = copy * 360.0;
            if (b.maxLon + lonShift < viewWest || b.minLon + lonShift > viewEast) continue;
            painter.save();
            if (copy != 0) painter.translate(copy * worldPx, 0.0);
            painter.drawPath(paths[index]);
            painter.restore();
        }
    };

    if (isDistrict) {
        // 区县层**平时完全透明**：只在有震度的区县才描边 + 填色。shadedIndices 已在
        // GUI 线程按要素名解析好，绘制循环里不做任何查找。
        painter.setRenderHint(QPainter::Antialiasing, false);
        for (std::size_t k = 0; k < state.shadedIndices.size(); ++k) {
            const auto index = static_cast<std::size_t>(state.shadedIndices[k]);
            if (index >= paths.size()) continue;
            const auto rgb = intensity_palette::intensityColor(state.shadedRaw[k]);
            // 填充用满不透明（kanameishi 的 classNameColors fillOpacity: 1），
            // 描边 #bbbbbb 单独一色，靠「底色 + 描边」区分等级。
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(rgb.r, rgb.g, rgb.b, 255));
            drawCopies(index);
        }
        if (wantsStroke) {
            // 描边必须抗锯齿：它是可见的轮廓线。
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(pen);
            for (std::size_t k = 0; k < state.shadedIndices.size(); ++k) {
                const auto index = static_cast<std::size_t>(state.shadedIndices[k]);
                if (index >= paths.size()) continue;
                drawCopies(index);
            }
        }
        painter.restore();
        return;
    }

    painter.setRenderHint(QPainter::Antialiasing, fillAntialias);
    for (std::size_t i = 0; i < paths.size(); ++i) drawCopies(i);

    if (wantsStroke) {
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(pen);
        for (std::size_t i = 0; i < paths.size(); ++i) drawCopies(i);
    }

    painter.restore();
}

} // namespace komira
