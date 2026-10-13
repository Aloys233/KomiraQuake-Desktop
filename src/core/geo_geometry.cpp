#include "core/geo_geometry.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QtMath>

#include "core/quake_calculator.h"

namespace komira {
namespace {

constexpr double kEarthRadiusKm = QuakeCalculator::kEarthRadiusKm;
constexpr double kKmPerDegreeLat = 111.32;

/// 以查询点为原点的局部平面近似（等距圆柱）。
///
/// 区县尺度（平均跨度约 50 km）下该近似的误差远小于数据本身的 5 m 量化步长，
/// 却把「点到线段」从每段一次球面三角运算降为二维叉积 —— 全量 1011 个区县
/// 约 34 万段，桌面端约 3 ms、低端 Android 约 15-40 ms，因此可以放在事件更新时算。
struct LocalPlane {
    double lat0;
    double lon0;
    double kmPerDegLon;

    LocalPlane(double lat, double lon)
        : lat0(lat),
          lon0(lon),
          // 与 pxPerKm 一致地夹住 cosLat，避免极点附近除零。
          kmPerDegLon(kKmPerDegreeLat * std::max(0.01, std::cos(lat * M_PI / 180.0))) {}

    double x(double lon) const { return (lon - lon0) * kmPerDegLon; }
    double y(double lat) const { return (lat - lat0) * kKmPerDegreeLat; }
};

/// 二维点 (px,py) 到线段 (ax,ay)-(bx,by) 的最短距离平方。
double squaredDistanceToSegment2D(double px, double py, double ax, double ay, double bx, double by) {
    const double dx = bx - ax;
    const double dy = by - ay;
    const double lenSq = dx * dx + dy * dy;
    if (lenSq <= 0.0) {
        const double ex = px - ax;
        const double ey = py - ay;
        return ex * ex + ey * ey;
    }
    // 参数 t 夹到 [0,1]，使投影落在线段上而非其延长线。
    double t = ((px - ax) * dx + (py - ay) * dy) / lenSq;
    t = std::max(0.0, std::min(1.0, t));
    const double ex = px - (ax + t * dx);
    const double ey = py - (ay + t * dy);
    return ex * ex + ey * ey;
}

/// 射线法（even-odd）：统计点向右投射时与环边的交点个数，奇数则在面内。
///
/// 在经纬度平面上直接做 —— 射线法只看拓扑（穿越次数），故经纬度平面上的结果与
/// 投影无关，无需先转平面坐标。
bool pointInRing(const GeoRing& ring, double lat, double lon) {
    bool inside = false;
    const size_t n = ring.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const double latI = ring[i].first;
        const double lonI = ring[i].second;
        const double latJ = ring[j].first;
        const double lonJ = ring[j].second;
        // 半开区间比较，避免顶点恰好落在射线所在纬度上时被重复计数。
        if ((latI > lat) != (latJ > lat)) {
            const double t = (lat - latI) / (latJ - latI);
            if (lon < lonI + t * (lonJ - lonI)) inside = !inside;
        }
    }
    return inside;
}

} // namespace

bool GeoGeometry::pointInPolygon(const GeoPolygon& polygon, double lat, double lon) {
    // even-odd：任意环命中即翻转。洞的环绕向与外环相反，翻转后洞内点自然判为「不在」。
    bool inside = false;
    for (const GeoRing& ring : polygon) {
        if (pointInRing(ring, lat, lon)) inside = !inside;
    }
    return inside;
}

double GeoGeometry::distanceToPolygonKm(const GeoPolygon& polygon, double lat, double lon) {
    if (pointInPolygon(polygon, lat, lon)) return 0.0;

    const LocalPlane plane(lat, lon);
    const double px = 0.0;
    const double py = 0.0;
    double bestSq = std::numeric_limits<double>::infinity();
    for (const GeoRing& ring : polygon) {
        const size_t n = ring.size();
        if (n < 2) continue;
        for (size_t i = 0, j = n - 1; i < n; j = i++) {
            const double ax = plane.x(ring[j].second);
            const double ay = plane.y(ring[j].first);
            const double bx = plane.x(ring[i].second);
            const double by = plane.y(ring[i].first);
            bestSq = std::min(bestSq, squaredDistanceToSegment2D(px, py, ax, ay, bx, by));
        }
    }
    return std::isfinite(bestSq) ? std::sqrt(bestSq) : 0.0;
}

double GeoGeometry::distanceToSegmentKm(double latA, double lonA, double latB, double lonB,
                                        double latP, double lonP) {
    const LocalPlane plane(latP, lonP);
    return std::sqrt(squaredDistanceToSegment2D(0.0, 0.0, plane.x(lonA), plane.y(latA),
                                                plane.x(lonB), plane.y(latB)));
}

double GeoBounds::maxDistanceKm(double lat, double lon) const {
    // 取「盒内离查询点最远的角点」作上界。纬度跨度直接换算，经度跨度按查询点所在
    // 纬度的最大 cos 因子（即 1.0）换算，故结果是保守上界。
    const double dLat = std::max(std::fabs(lat - minLat), std::fabs(maxLat - lat)) * kKmPerDegreeLat;
    const double dLon = std::max(std::fabs(lon - minLon), std::fabs(maxLon - lon)) * kKmPerDegreeLat;
    return std::sqrt(dLat * dLat + dLon * dLon);
}

GeoBounds boundsOf(const GeoPolygon& polygon) {
    GeoBounds bounds;
    bool first = true;
    for (const GeoRing& ring : polygon) {
        for (const auto& pt : ring) {
            if (first) {
                bounds.minLat = bounds.maxLat = pt.first;
                bounds.minLon = bounds.maxLon = pt.second;
                first = false;
                continue;
            }
            bounds.minLat = std::min(bounds.minLat, pt.first);
            bounds.maxLat = std::max(bounds.maxLat, pt.first);
            bounds.minLon = std::min(bounds.minLon, pt.second);
            bounds.maxLon = std::max(bounds.maxLon, pt.second);
        }
    }
    return bounds;
}

} // namespace komira