#pragma once

#include <cstdint>
#include <vector>

namespace komira {

/// 一条闭合环的经纬度序列（度，WGS-84）。首尾点相同。
using GeoRing = std::vector<std::pair<double, double>>;
/// 一个面的全部环：首个为外环，其余为洞（飞地、内陆空洞）。
using GeoPolygon = std::vector<GeoRing>;

/// 地图几何：点在面内判定与点到面最短距离。
///
/// 供矢量地图（区县级 EEW 烈度着色）使用。刻意不依赖 Qt，以便进入 core_engine_test
/// 与 desktop/run_core_tests.sh 的纯 g++ 编译路径。
///
/// 坐标一律为 WGS-84 度。经度不在这里做环绕：渲染层按视口把要素按 ±360° 复制绘制，
/// 副本只是经度整体平移，故点在面内的判定仍是简单的经度直接比较。
class GeoGeometry {
public:
    /// 射线法（even-odd）判定点是否落在面内。点在洞内判为「不在」。
    ///
    /// 必须用 even-odd 规则而非「任一环命中即命中」：后者会让洞内的点被误判为在面内。
    static bool pointInPolygon(const GeoPolygon& polygon, double lat, double lon);

    /// 点到面的最短距离（km）。点在面内返回 0。
    ///
    /// 洞内**不算**面内（洞不属于该区县），故返回到最近边界的距离而非 0。
    ///
    /// 区内取 0 是**有意的保守选择**：地震预警里显示更高烈度属于安全侧，
    /// 而区内采样平均既更贵、又落在不安全侧。
    static double distanceToPolygonKm(const GeoPolygon& polygon, double lat, double lon);

    /// 点到单条线段的最短距离（km），按 Haversine 逐段近似。
    static double distanceToSegmentKm(double latA, double lonA, double latB, double lonB,
                                      double latP, double lonP);
};

/// 经纬度包围盒。用于按影响半径裁剪候选面，避免对全部区县做精确距离计算。
struct GeoBounds {
    double minLat = 0.0;
    double minLon = 0.0;
    double maxLat = 0.0;
    double maxLon = 0.0;

    bool contains(double lat, double lon) const {
        return lat >= minLat && lat <= maxLat && lon >= minLon && lon <= maxLon;
    }
    /// 盒内任一点到 (lat, lon) 的上界（km），保守但无需遍历顶点。
    double maxDistanceKm(double lat, double lon) const;
};

/// 计算一组环的包围盒。
GeoBounds boundsOf(const GeoPolygon& polygon);

} // namespace komira