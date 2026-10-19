#include "core/polygon_tessellator.h"

#include <algorithm>
#include <cmath>

namespace komira {
namespace {

/// 边缘：y 方向非退化（水平边不参与填充配对，它们只贡献扫描带边界）。
struct Edge {
    double x0 = 0.0;
    double y0 = 0.0;
    /// dx/dy。水平边不会进这个结构，故除零不会发生。
    double invSlope = 0.0;
    double yMin = 0.0;
    double yMax = 0.0;
    /// 绕向权重（非零规则用）：沿 +x 扫过时，向下的边「进入」记 +1、向上的边「离开」记 -1。
    int winding = 0;
    /// 该边在 y 处的 x。
    double xAt(double y) const { return x0 + (y - y0) * invSlope; }
};

/// 同一个扫描带内的一横跨区间，左右边界各是一条边。
struct Span {
    uint32_t leftEdge = 0;
    uint32_t rightEdge = 0;
    /// 未夹取的左右 x（夹取留到输出时做，见 emitSpan 的注释）。
    double xLa = 0.0;
    double xRa = 0.0;
    double xLb = 0.0;
    double xRb = 0.0;
    double yA = 0.0;
    double yB = 0.0;
};

/// 把梯形写成两个三角形。
///
/// 每端取 min/max：两条边在同一扫描带内**交叉**时（自相交输入才会出现），原始左右
/// 顺序在 yA 与 yB 处相反，直接连会得到翻面四边形；取 min/max 后，两个三角形恰好
/// 就是交叉产生的两块区域，面积与 even-odd 的语义一致，不会整块丢失。
///
/// `clip` 非空时把两端 x 夹到矩形内。y 方向由调用方负责（整带跳过/夹取），这里只
/// 处理 x —— 因为扫描带本身就是按 y 切的矩形条，y 的裁剪等价于「跳过带」。
void emitSpan(const Span& s, PolygonTessellator::Mesh* out,
              const PolygonTessellator::ClipRect* clip) {
    double ax = std::min(s.xLa, s.xRa);
    double bx = std::max(s.xLa, s.xRa);
    double cx = std::min(s.xLb, s.xRb);
    double dx = std::max(s.xLb, s.xRb);
    if (clip) {
        // 夹取必须**先夹后判**：夹完可能整端落到矩形外（宽度变负），也可能两端都被
        // 压到同一条边界（宽度为 0）。这两种情况都不能出三角形，否则会把矩形外的
        // 面积也算进去（实测裁 25..75 时多出整整一倍面积）。
        ax = std::max(ax, clip->x0);
        bx = std::min(bx, clip->x1);
        cx = std::max(cx, clip->x0);
        dx = std::min(dx, clip->x1);
        if (bx <= ax && dx <= cx) return;
    }

    // 两端都退化成一点时没有面积，直接跳过（避免给 GPU 灌零面积三角形）。
    const double topWidth = bx - ax;
    const double botWidth = dx - cx;
    if (topWidth <= 0.0 && botWidth <= 0.0) return;
    // 夹取后可能整条带落在矩形外：任一端宽度为负即说明该端被夹没了。
    if (topWidth < 0.0 || botWidth < 0.0) return;

    const auto base = static_cast<uint32_t>(out->vertices.size() / 2);
    out->vertices.push_back(ax);
    out->vertices.push_back(s.yA);
    out->vertices.push_back(bx);
    out->vertices.push_back(s.yA);
    out->vertices.push_back(dx);
    out->vertices.push_back(s.yB);
    out->vertices.push_back(cx);
    out->vertices.push_back(s.yB);

    out->indices.push_back(base + 0);
    out->indices.push_back(base + 1);
    out->indices.push_back(base + 2);
    out->indices.push_back(base + 0);
    out->indices.push_back(base + 2);
    out->indices.push_back(base + 3);
}

} // namespace

void PolygonTessellator::tessellate(const std::vector<Ring>& rings, FillRule fillRule, Mesh* out,
                                   const ClipRect* clip) {
    if (!out) return;
    out->clear();
    if (rings.empty()) return;

    std::vector<Edge> edges;
    std::vector<double> ys;

    for (const Ring& ring : rings) {
        if (!ring.xy || ring.count < 3) continue;
        // 先确认整条环的坐标都是有限值：坏坐标会让扫描带排序失去传递性，宁可整环跳过
        // 也不要让 NaN 混进排序。
        bool finite = true;
        for (int i = 0; i < ring.count && finite; ++i) {
            finite = std::isfinite(ring.xy[2 * i]) && std::isfinite(ring.xy[2 * i + 1]);
        }
        if (!finite) continue;

        for (int i = 0; i < ring.count; ++i) {
            const double x0 = ring.xy[2 * i];
            const double y0 = ring.xy[2 * i + 1];
            ys.push_back(y0);
            const int j = (i + 1) % ring.count;   // 隐式闭合：末点连回首点
            const double x1 = ring.xy[2 * j];
            const double y1 = ring.xy[2 * j + 1];
            if (y0 == y1) continue;   // 水平边不参与横跨配对

            Edge e;
            e.x0 = x0;
            e.y0 = y0;
            e.invSlope = (x1 - x0) / (y1 - y0);
            e.yMin = std::min(y0, y1);
            e.yMax = std::max(y0, y1);
            // 沿 +x 方向扫掠：向下（y 增大）的边是被「进入」的一侧。
            e.winding = (y1 > y0) ? -1 : 1;
            edges.push_back(e);
        }
    }
    if (edges.empty()) return;

    std::sort(ys.begin(), ys.end());
    ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
    if (ys.size() < 2) return;

    // 活动边表：边按 yMin 排序，扫描带自下而上推进，故每条边**只入表一次、只出表一次**。
    // 早先的实现每个带都重扫全部边（O(带数 × 边数)）：真实数据上 world 层 492 ms，
    // 换成本表后同一份数据降到个位数毫秒，且复杂度与要素顶点数线性相关。
    std::vector<uint32_t> byYMin(edges.size());
    for (std::size_t i = 0; i < edges.size(); ++i) byYMin[i] = static_cast<uint32_t>(i);
    std::sort(byYMin.begin(), byYMin.end(), [&](uint32_t a, uint32_t b) {
        if (edges[a].yMin != edges[b].yMin) return edges[a].yMin < edges[b].yMin;
        return a < b;
    });

    std::vector<uint32_t> active;
    std::vector<Span> openSpans;     // 正在累积（等待向下合并）的梯形
    std::vector<Span> bandSpans;     // 当前扫描带内的区间
    std::size_t nextToAdd = 0;

    const auto flushOpen = [&]() {
        for (const Span& s : openSpans) emitSpan(s, out, clip);
        openSpans.clear();
    };

    for (std::size_t k = 0; k + 1 < ys.size(); ++k) {
        const double yA = ys[k];
        const double yB = ys[k + 1];
        if (!(yB > yA)) continue;

        // y 方向裁剪：整条带落在矩形外就跳过。这一步让成本与「块内真实几何量」
        // 成正比 —— 块外的扫描带一个梯形都不会产出，也就不会进入顶点缓冲。
        if (clip && (yB <= clip->y0 || yA >= clip->y1)) {
            flushOpen();
            continue;
        }

        // y 方向裁剪：先用扫描带与矩形求交，得到本带实际要输出的 y 范围。
        //
        // 必须**夹取 y 之后再求 x**：边是斜的，x 随 y 变化，直接用原带的 yA/yB 求 x
        // 会把矩形内外的面积一起算上（实测裁 25..75 时面积翻倍）。夹取后左右边都用
        // 同一条边方程在新 y 处求值，接缝依旧严格重合。
        double bandY0 = yA;
        double bandY1 = yB;
        if (clip) {
            bandY0 = std::max(yA, clip->y0);
            bandY1 = std::min(yB, clip->y1);
            if (!(bandY1 > bandY0)) {
                flushOpen();
                continue;
            }
        }

        // 出表：yMax <= yA 的边不再覆盖本带（扫描带按顶点 y 切分，不存在「部分覆盖」）。
        active.erase(std::remove_if(active.begin(), active.end(),
                                    [&](uint32_t i) { return edges[i].yMax <= yA; }),
                     active.end());
        // 入表：yMin <= yA 的边。yMin 单调不减，指针只前进。
        while (nextToAdd < byYMin.size() && edges[byYMin[nextToAdd]].yMin <= yA) {
            active.push_back(byYMin[nextToAdd]);
            ++nextToAdd;
        }
        if (active.size() < 2) {
            flushOpen();
            continue;
        }

        // 按带内中点的 x 排序：带内若有两边交叉，中点序是唯一自洽的左右序。
        const double yMid = 0.5 * (bandY0 + bandY1);
        std::sort(active.begin(), active.end(), [&](uint32_t a, uint32_t b) {
            const double xa = edges[a].xAt(yMid);
            const double xb = edges[b].xAt(yMid);
            if (xa != xb) return xa < xb;
            return a < b;   // 同 x 时用下标定序，保证排序结果可复现
        });

        // 按填充规则配对成横跨区间。
        bandSpans.clear();
        int winding = 0;
        bool parity = false;
        for (std::size_t i = 0; i + 1 < active.size(); ++i) {
            if (fillRule == FillRule::EvenOdd) {
                parity = !parity;
            } else {
                winding += edges[active[i]].winding;
            }
            const bool inside = (fillRule == FillRule::EvenOdd) ? parity : (winding != 0);
            if (!inside) continue;

            const Edge& left = edges[active[i]];
            const Edge& right = edges[active[i + 1]];
            Span s;
            s.leftEdge = active[i];
            s.rightEdge = active[i + 1];
            s.yA = bandY0;
            s.yB = bandY1;
            s.xLa = left.xAt(bandY0);
            s.xLb = left.xAt(bandY1);
            s.xRa = right.xAt(bandY0);
            s.xRb = right.xAt(bandY1);
            bandSpans.push_back(s);
        }

        // 向下合并：相邻扫描带的左右边界都是同两条边时，合成一个梯形。没有这一步，
        // 一条含 n 个顶点的环会退化成 n 条细梯形（4n 个顶点），顶点数翻好几倍。
        const bool mergeable = openSpans.size() == bandSpans.size() && !openSpans.empty();
        bool same = mergeable;
        if (mergeable) {
            for (std::size_t i = 0; i < bandSpans.size(); ++i) {
                if (openSpans[i].leftEdge != bandSpans[i].leftEdge ||
                    openSpans[i].rightEdge != bandSpans[i].rightEdge) {
                    same = false;
                    break;
                }
            }
        }
        if (same) {
            for (std::size_t i = 0; i < bandSpans.size(); ++i) {
                openSpans[i].yB = bandY1;
                openSpans[i].xLb = bandSpans[i].xLb;
                openSpans[i].xRb = bandSpans[i].xRb;
            }
        } else {
            flushOpen();
            openSpans = bandSpans;
        }
    }
    flushOpen();
}

double PolygonTessellator::signedArea(const std::vector<Ring>& rings) {
    double total = 0.0;
    for (const Ring& ring : rings) {
        if (!ring.xy || ring.count < 3) continue;
        double twice = 0.0;
        for (int i = 0; i < ring.count; ++i) {
            const int j = (i + 1) % ring.count;
            twice += ring.xy[2 * i] * ring.xy[2 * j + 1] - ring.xy[2 * j] * ring.xy[2 * i + 1];
        }
        total += 0.5 * twice;
    }
    return total;
}

double PolygonTessellator::meshArea(const Mesh& mesh) {
    double total = 0.0;
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const uint32_t a = mesh.indices[i];
        const uint32_t b = mesh.indices[i + 1];
        const uint32_t c = mesh.indices[i + 2];
        const double ax = mesh.vertices[2 * a], ay = mesh.vertices[2 * a + 1];
        const double bx = mesh.vertices[2 * b], by = mesh.vertices[2 * b + 1];
        const double cx = mesh.vertices[2 * c], cy = mesh.vertices[2 * c + 1];
        total += 0.5 * std::fabs((bx - ax) * (cy - ay) - (by - ay) * (cx - ax));
    }
    return total;
}

namespace {

/// 环的包围盒与绝对面积，嵌套分组用。
struct RingBox {
    double x0 = 0.0, x1 = 0.0, y0 = 0.0, y1 = 0.0;
    double absArea = 0.0;
    std::size_t index = 0;
};

/// 射线法（even-odd）判点是否在环内。只用于嵌套判定，性能不敏感。
bool pointInRing(const double* xy, int count, double px, double py) {
    bool inside = false;
    for (int i = 0, j = count - 1; i < count; j = i++) {
        const double xi = xy[2 * i], yi = xy[2 * i + 1];
        const double xj = xy[2 * j], yj = xy[2 * j + 1];
        if (((yi > py) != (yj > py)) && (px < (xj - xi) * (py - yi) / (yj - yi) + xi)) {
            inside = !inside;
        }
    }
    return inside;
}

} // namespace

void PolygonTessellator::tessellateNested(const std::vector<Ring>& rings, FillRule fillRule,
                                          Mesh* out, const ClipRect* clip) {
    if (!out) return;
    out->clear();
    if (rings.empty()) return;

    // 只有「一个环」时没有嵌套可言，直接走联合扫描。
    if (rings.size() == 1) {
        tessellate(rings, fillRule, out, clip);
        return;
    }

    std::vector<RingBox> boxes;
    boxes.reserve(rings.size());
    for (std::size_t i = 0; i < rings.size(); ++i) {
        const Ring& r = rings[i];
        if (!r.xy || r.count < 3) continue;
        bool finite = true;
        double x0 = r.xy[0], x1 = r.xy[0], y0 = r.xy[1], y1 = r.xy[1];
        for (int k = 0; k < r.count && finite; ++k) {
            const double x = r.xy[2 * k], y = r.xy[2 * k + 1];
            finite = std::isfinite(x) && std::isfinite(y);
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
        if (!finite) continue;
        const std::vector<Ring> one{r};
        RingBox b;
        b.x0 = x0;
        b.x1 = x1;
        b.y0 = y0;
        b.y1 = y1;
        b.absArea = std::fabs(signedArea(one));
        b.index = i;
        boxes.push_back(b);
    }
    if (boxes.size() < 2) {
        tessellate(rings, fillRule, out, clip);
        return;
    }

    // 每个环的嵌套深度 = 有多少个「更大的环」真的把它包住。
    // 只在包围盒相交时才做点内测试 —— 真实数据里这一层剪枝几乎滤掉全部候选。
    struct Nest {
        int depth = 0;
        int parent = -1;
    };
    std::vector<Nest> nest(boxes.size());
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        const Ring& ri = rings[boxes[i].index];
        // 取环首点为测试点。环可能自相交（真实数据 90% 的环都有），此时「在环内」对
        // 环边界上的点没有意义；但嵌套判定只需要一个代表点，首点足够稳定。
        const double px = ri.xy[0];
        const double py = ri.xy[1];
        int depth = 0;
        int parent = -1;
        double bestArea = 0.0;
        for (std::size_t k = 0; k < boxes.size(); ++k) {
            if (k == i) continue;
            if (boxes[k].absArea <= boxes[i].absArea) continue;   // 只有更大的环才可能包住它
            if (px < boxes[k].x0 || px > boxes[k].x1 || py < boxes[k].y0 || py > boxes[k].y1) continue;
            const Ring& rk = rings[boxes[k].index];
            if (!pointInRing(rk.xy, rk.count, px, py)) continue;
            ++depth;
            if (parent < 0 || boxes[k].absArea < bestArea) {
                bestArea = boxes[k].absArea;
                parent = static_cast<int>(k);
            }
        }
        nest[i].depth = depth;
        nest[i].parent = parent;
    }

    // 深度为偶数的环开一个新面；它的直接子环（深度恰为 +1）作为洞。
    // 深度为奇数的环不单独成面（否则会在洞里再填回去）。
    std::vector<Ring> group;
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        if (nest[i].depth % 2 != 0) continue;
        group.clear();
        group.push_back(rings[boxes[i].index]);
        for (std::size_t k = 0; k < boxes.size(); ++k) {
            if (nest[k].parent != static_cast<int>(i)) continue;
            group.push_back(rings[boxes[k].index]);
        }
        Mesh sub;
        tessellate(group, fillRule, &sub, clip);
        const auto base = static_cast<uint32_t>(out->vertices.size() / 2);
        out->vertices.insert(out->vertices.end(), sub.vertices.begin(), sub.vertices.end());
        out->indices.reserve(out->indices.size() + sub.indices.size());
        for (uint32_t idx : sub.indices) out->indices.push_back(base + idx);
    }
}


} // namespace komira
