#include "map/vector_mesh.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "core/coordinate_transform.h"

namespace komira {
namespace {

constexpr double kLatLimit = 85.05112878;
constexpr int kRefZoom = 10;
constexpr double kPi = 3.14159265358979323846;

double worldSize(int z) { return 256.0 * std::pow(2.0, z); }

double projY(double lat, double size) {
    const double rad = std::max(-kLatLimit, std::min(kLatLimit, lat)) * kPi / 180.0;
    return (1.0 - std::log(std::tan(rad) + 1.0 / std::cos(rad)) / kPi) / 2.0 * size;
}
double projX(double lon, double size) { return (lon + 180.0) / 360.0 * size; }

/// Douglas–Peucker（迭代版）。与 VectorMapItem 的同名实现语义一致，但输入输出都是
/// 交错双精度数组，不与 Qt 容器耦合（本文件可在 worker 线程与外层测试里直接用）。
void simplifyRing(const std::vector<double>& in, double tol, std::vector<double>* out) {
    out->clear();
    const int n = static_cast<int>(in.size() / 2);
    if (n < 3 || tol <= 0.0) {
        *out = in;
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
        const double ax = in[2 * a], ay = in[2 * a + 1];
        const double bx = in[2 * b], by = in[2 * b + 1];
        const double dx = bx - ax;
        const double dy = by - ay;
        const double l2 = dx * dx + dy * dy;
        double best = -1.0;
        int bestIndex = -1;
        for (int i = a + 1; i < b; ++i) {
            const double px = in[2 * i] - ax;
            const double py = in[2 * i + 1] - ay;
            double d2 = 0.0;
            if (l2 <= 1e-12) {
                d2 = px * px + py * py;
            } else {
                double t = (px * dx + py * dy) / l2;
                t = std::max(0.0, std::min(1.0, t));
                const double ex = px - t * dx;
                const double ey = py - t * dy;
                d2 = ex * ex + ey * ey;
            }
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
    out->reserve(in.size());
    for (int i = 0; i < n; ++i) {
        if (keep[static_cast<std::size_t>(i)]) {
            out->push_back(in[2 * i]);
            out->push_back(in[2 * i + 1]);
        }
    }
    if (out->size() < 6) *out = in;   // 退化：宁可保留原环
}

} // namespace

VectorMeshBuilder::LayerPtr VectorMeshBuilder::build(const VectorMapData& data,
                                                     const std::string& layerName, bool gcjShift,
                                                     double lodToleranceRef, const Style& style) {
    const VectorLayer* layer = data.layer(layerName);
    auto result = std::make_shared<Layer>();
    if (!layer) return result;

    const double size = worldSize(kRefZoom);
    // GCJ 纠偏只对区县层生效：世界层含全球要素，纠偏会把蒙古/印度一起平移
    //（与既有实现一致，勿改）。
    const bool applyGcj = gcjShift && layerName == "district";

    std::vector<Feature> features;
    features.reserve(layer->features.size());
    for (const VectorFeature& f : layer->features) {
        Feature feature;
        feature.name = f.name;
        for (const GeoRing& ring : f.polygon) {
            if (ring.size() < 3) continue;
            std::vector<double> projected;
            projected.reserve(ring.size() * 2);
            for (const auto& point : ring) {
                double la = point.first;
                double lo = point.second;
                if (applyGcj) {
                    const auto shifted = CoordinateTransform::wgs84ToGcj02(la, lo);
                    la = shifted.first;
                    lo = shifted.second;
                }
                projected.push_back(projX(lo, size));
                projected.push_back(projY(la, size));
            }
            feature.rings.push_back(std::move(projected));
        }
        if (feature.rings.empty()) continue;
        features.push_back(std::move(feature));
    }

    return buildFromFeatures(features, lodToleranceRef, style);
}

VectorMeshBuilder::LayerPtr VectorMeshBuilder::buildFromFeatures(const std::vector<Feature>& features,
                                                                 double lodToleranceRef,
                                                                 const Style& style) {
    auto result = std::make_shared<Layer>();
    result->featureBounds.resize(features.size());

    const double tilePx = kTileWorldPx;
    // 用 map 而不是 unordered_map：块按 (ty,tx) 有序累积，输出顺序天然稳定，
    // 渲染顺序可复现（诊断与像素回归都依赖这一点）。
    std::map<std::pair<int, int>, Tile> tiles;

    for (std::size_t fi = 0; fi < features.size(); ++fi) {
        const Feature& feature = features[fi];

        // 1) 简化。简化在**全精度世界坐标**下做（double），这样容差就是世界像素，
        //    与调用方按缩放算出的档位直接对应。
        std::vector<std::vector<double>> simplified;
        simplified.reserve(feature.rings.size());
        double fx0 = 1e300, fy0 = 1e300, fx1 = -1e300, fy1 = -1e300;
        for (const auto& ring : feature.rings) {
            std::vector<double> out;
            simplifyRing(ring, lodToleranceRef, &out);
            if (out.size() < 6) continue;
            for (std::size_t i = 0; i + 1 < out.size(); i += 2) {
                fx0 = std::min(fx0, out[i]);
                fx1 = std::max(fx1, out[i]);
                fy0 = std::min(fy0, out[i + 1]);
                fy1 = std::max(fy1, out[i + 1]);
            }
            simplified.push_back(std::move(out));
        }
        if (simplified.empty()) continue;
        result->featureBounds[fi] = Layer::FeatureBounds{fx0, fy0, fx1, fy1};

        // 2) 逐块三角化：每块只把**该块覆盖范围内**的环喂给三角化器，并用裁剪矩形
        //    把结果限制在块内。
        //
        //    为什么不「先全量三角化再按块切分三角形」：那样跨块的大三角形会被切成
        //    多份，实测世界层从 52 万三角形膨胀到 166 万、101 MB。而按块预先裁剪的
        //    做法里，裁剪发生在扫描带内部（只跳过块外扫描带、把带夹到块内），成本与
        //    「块内真实几何量」成正比，总量与全量三角化同一个数量级。
        //
        //    顶点坐标系：三角化在**块内局部坐标**（世界像素减去块原点）下进行，
        //    因此 float32 的精度只与块大小相关（见 kTileWorldPx 的注释）。
        const int fx0t = static_cast<int>(std::floor(fx0 / tilePx));
        const int fx1t = static_cast<int>(std::floor(fx1 / tilePx));
        const int fy0t = static_cast<int>(std::floor(fy0 / tilePx));
        const int fy1t = static_cast<int>(std::floor(fy1 / tilePx));

        for (int ty = fy0t; ty <= fy1t; ++ty) {
            for (int tx = fx0t; tx <= fx1t; ++tx) {
                const double originX = tx * tilePx;
                const double originY = ty * tilePx;

                // 只把**与本块相交**的环平移进来。
                //
                // 不做这个筛选会让成本爆炸：世界层的日本要素有 352 个环、跨几十块，
                // 每块都把 352 条环全量平移 + 做 O(n²) 嵌套分析，实测整层 3.9 秒。
                // 筛选后每块只处理真正落在块内的那几条环。
                const double bx1 = originX + tilePx;
                const double by1 = originY + tilePx;
                std::vector<std::vector<double>> local;
                local.reserve(simplified.size());
                for (const auto& ring : simplified) {
                    // 环的包围盒与块相交才保留。（用环自身的 min/max，只算一次太贵，
                    // 这里直接扫顶点 —— 总代价仍是 O(顶点数)，可接受。）
                    double rx0 = 1e300, ry0 = 1e300, rx1 = -1e300, ry1 = -1e300;
                    for (std::size_t i = 0; i + 1 < ring.size(); i += 2) {
                        rx0 = std::min(rx0, ring[i]);
                        rx1 = std::max(rx1, ring[i]);
                        ry0 = std::min(ry0, ring[i + 1]);
                        ry1 = std::max(ry1, ring[i + 1]);
                    }
                    if (rx1 < originX || rx0 > bx1 || ry1 < originY || ry0 > by1) continue;
                    std::vector<double> shifted;
                    shifted.reserve(ring.size());
                    for (std::size_t i = 0; i + 1 < ring.size(); i += 2) {
                        shifted.push_back(ring[i] - originX);
                        shifted.push_back(ring[i + 1] - originY);
                    }
                    local.push_back(std::move(shifted));
                }
                if (local.empty()) continue;

                std::vector<PolygonTessellator::Ring> rings;
                rings.reserve(local.size());
                for (const auto& r : local) rings.emplace_back(r);

                // 裁剪矩形就是块本身（局部坐标下恒为 [0,tilePx]）。
                PolygonTessellator::ClipRect clip{0.0, 0.0, tilePx, tilePx};
                PolygonTessellator::Mesh mesh;
                PolygonTessellator::tessellateNested(rings, PolygonTessellator::FillRule::EvenOdd,
                                                     &mesh, &clip);
                if (mesh.empty()) continue;

                auto found = tiles.find({ty, tx});
                if (found == tiles.end()) {
                    Tile t;
                    t.originX = originX;
                    t.originY = originY;
                    found = tiles.emplace(std::make_pair(ty, tx), std::move(t)).first;
                }
                Tile& tile = found->second;
                const auto base = static_cast<uint32_t>(tile.vertices.size());
                for (std::size_t i = 0; i + 1 < mesh.vertices.size(); i += 2) {
                    Vertex v;
                    v.x = static_cast<float>(mesh.vertices[i]);
                    v.y = static_cast<float>(mesh.vertices[i + 1]);
                    v.r = style.fillR; v.g = style.fillG; v.b = style.fillB; v.a = style.fillA;
                    tile.vertices.push_back(v);
                }
                for (uint32_t idx : mesh.indices) tile.fillIndices.push_back(base + idx);
            }
        }

        // 4) 描边几何：沿环展开成带状四边形，同样逐块归属。
        //
        //    宽度必须恒定于**屏幕像素**（缩小时线不能变粗糊住底图），所以不把宽度烘
        //    进顶点坐标 —— 顶点只带外侧单位法线，由 shader 按当前缩放扩张（见
        //    vector_mesh_material.h）。每个环点在法线两侧各出一个顶点，相邻两点之间
        //    的四边形由 2 个三角形连成。
        //
        //    描边按**每一段的中点**归属分块（而不是整环归首点所在块）：一个环可能横跨
        //    很多块，整环归一块会让块内坐标溢出 float32 精度带。逐段归属后每段的两端
        //    都在同一块内（段长远小于块边长），坐标恒在 [0, tilePx] 附近。
        for (const auto& ring : simplified) {
            const int n = static_cast<int>(ring.size() / 2);
            if (n < 3) continue;

            // 预剪每个点的法线（含 miter 补偿），再逐段分发。
            std::vector<double> nx(n), ny(n);
            for (int i = 0; i < n; ++i) {
                const int prev = (i + n - 1) % n;
                const int next = (i + 1) % n;
                const double px = ring[2 * prev], py = ring[2 * prev + 1];
                const double cxp = ring[2 * i], cyp = ring[2 * i + 1];
                const double nxp = ring[2 * next], nyp = ring[2 * next + 1];

                // 两条相邻边的单位法线取平均（miter，不做长度补偿）：锐角处会略微变窄，
                // 但不会像完整 miter 那样伸出长尖刺。
                double n1x = -(cyp - py), n1y = cxp - px;
                double n2x = -(nyp - cyp), n2y = nxp - cxp;
                const double l1 = std::hypot(n1x, n1y);
                const double l2 = std::hypot(n2x, n2y);
                if (l1 > 1e-12) { n1x /= l1; n1y /= l1; }
                if (l2 > 1e-12) { n2x /= l2; n2y /= l2; }
                double ux = n1x + n2x;
                double uy = n1y + n2y;
                const double ul = std::hypot(ux, uy);
                if (ul < 1e-6) {
                    ux = n1x;   // 180° 折返：退回上一条边的法线，避免退化
                    uy = n1y;
                } else {
                    ux /= ul;
                    uy /= ul;
                }
                // miter 拉伸补偿：夹角越尖需要越长的法线才能保持线宽，上限 2 倍
                //（宁可尖角略窄，也不要长刺）。
                const double cosHalf = std::max(0.5, (n1x * ux + n1y * uy));
                const double miter = 1.0 / cosHalf;
                nx[i] = ux * miter;
                ny[i] = uy * miter;
            }

            for (int i = 0; i < n; ++i) {
                const int j = (i + 1) % n;
                const double x0 = ring[2 * i], y0 = ring[2 * i + 1];
                const double x1 = ring[2 * j], y1 = ring[2 * j + 1];
                const double mx = 0.5 * (x0 + x1);
                const double my = 0.5 * (y0 + y1);
                const int tx = static_cast<int>(std::floor(mx / tilePx));
                const int ty = static_cast<int>(std::floor(my / tilePx));

                auto found = tiles.find({ty, tx});
                if (found == tiles.end()) {
                    Tile t;
                    t.originX = tx * tilePx;
                    t.originY = ty * tilePx;
                    found = tiles.emplace(std::make_pair(ty, tx), std::move(t)).first;
                }
                Tile& tile = found->second;
                const auto base = static_cast<uint32_t>(tile.vertices.size());

                // 描边宽度烘焙成世界像素：屏幕 1 px 对应 1/screenPerWorldPx 个世界像素。
                const double halfWorld =
                    0.5 * style.strokeWidthPx / std::max(1e-9, style.screenPerWorldPx);
                // 该段两端的 4 个顶点：起点外侧/内侧、终点外侧/内侧。
                const double pts[4][2] = {
                    {x0 + nx[i] * halfWorld, y0 + ny[i] * halfWorld},
                    {x0 - nx[i] * halfWorld, y0 - ny[i] * halfWorld},
                    {x1 + nx[j] * halfWorld, y1 + ny[j] * halfWorld},
                    {x1 - nx[j] * halfWorld, y1 - ny[j] * halfWorld}};
                for (int k = 0; k < 4; ++k) {
                    Vertex v;
                    v.x = static_cast<float>(pts[k][0] - tile.originX);
                    v.y = static_cast<float>(pts[k][1] - tile.originY);
                    v.r = style.strokeR; v.g = style.strokeG; v.b = style.strokeB;
                    v.a = style.strokeA;
                    tile.vertices.push_back(v);
                }
                // 四边形 (a0,a1) -> (b0,b1)：起点外侧/内侧、终点外侧/内侧。
                const uint32_t a0 = base + 0;
                const uint32_t a1 = base + 1;
                const uint32_t b0 = base + 2;
                const uint32_t b1 = base + 3;
                tile.strokeIndices.push_back(a0);
                tile.strokeIndices.push_back(a1);
                tile.strokeIndices.push_back(b1);
                tile.strokeIndices.push_back(a0);
                tile.strokeIndices.push_back(b1);
                tile.strokeIndices.push_back(b0);
            }
        }
    }

    result->tiles.reserve(tiles.size());
    for (auto& entry : tiles) {
        Tile& t = entry.second;
        if (t.empty()) continue;
        result->vertexCount += t.vertices.size();
        result->fillTriangleCount += t.fillIndices.size() / 3;
        result->strokeTriangleCount += t.strokeIndices.size() / 3;
        // 填充与描边合并成一份索引：两者颜色不同但都烘焙在顶点里，因此可以一次
        // draw call 画完（分开反而要多一个节点）。顶点缓冲本来就是共用的。
        t.fillIndices.insert(t.fillIndices.end(), t.strokeIndices.begin(), t.strokeIndices.end());
        t.strokeIndices.clear();
        result->tiles.push_back(std::move(t));
    }
    return result;
}

} // namespace komira
