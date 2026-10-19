#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace komira {

/// 多边形三角化（扫描线梯形分解）。
///
/// ## 为什么不用耳切 / Qt 的 qTriangulate
///
/// 矢量层要喂给 GPU 的面包含大量环：世界层含跨日界线副本，区县层有 175 个多环面
/// （外环 + 飞地洞）。实测在真实 `.kvb` 上，Qt 私有的 `qTriangulate`（内部先把坐标
/// 放大 32 倍再取整到 1/32 网格）需要：world 层 11.7 万点 **124 ms**、district 层
/// 34 万点 **171 ms** —— 每次换 LOD 档都要付一遍，不可接受。耳切法对凹多边形是
/// O(n²)，对自相交输入更是没有定义行为。
///
/// 本实现是**梯形分解**：按所有顶点的 y 值扫掠，在每个扫描带内按填充规则把边两两
/// 配对成横跨区间，再合并「左右边界在同一条边上」的相邻带，得到梯形四边形。
///
///   · 复杂度 O(n log n)，与凹度无关；自相交/零面积/重复点都不会崩。
///   · **水密**：相邻梯形共用同一条 y 边界，且左右边的 x 都由同一条边方程在**同一个
///     y** 处求值，因此接缝严格重合 —— 不会像「逐三角形独立光栅化」那样在 GPU 上
///     露出 1 px 亮线。
///   · 输出三角形数与输出顶点数同阶：一个方形只出 1 个梯形（2 个三角形），不会被
///     逐带切成一串细条。
///
/// 刻意不依赖 Qt：本文件进 `run_core_tests.sh` 的纯 g++ 编译路径，三角化逻辑可以在
/// 没有显示环境的 CI 上直接断言。
class PolygonTessellator {
public:
    enum class FillRule {
        /// 奇偶规则：洞靠「穿过奇数条边」自动镂空。矢量层的填充用它 —— 不必先判断
        /// 哪个环是洞，也不依赖环的绕向（数据来自第三方，绕向不可信）。
        EvenOdd,
        /// 非零规则：按绕向累加。描边轮廓用这个，否则折线自重叠处会被挖出空洞。
        Winding,
    };

    /// 一条环：`xy` 是交错的 x,y 双精度数组（顶点数 = `count`）。环**隐式闭合**
    /// （最后一点与第一点之间自动补一条边，不要自己重复首点）。
    struct Ring {
        const double* xy = nullptr;
        int count = 0;

        Ring() = default;
        /// 从 `std::vector<double>`（交错 x,y）构造，便于测试与调用方。
        explicit Ring(const std::vector<double>& v) : xy(v.data()), count(static_cast<int>(v.size() / 2)) {}
    };

    struct Mesh {
        /// 交错 x,y **double** 顶点，与输入同空间。调用方负责在转换成 GPU 顶点时
        /// 减掉分块原点（让 float32 落在精度甜区），并做分块归类。
        std::vector<double> vertices;
        std::vector<uint32_t> indices;

        void clear() {
            vertices.clear();
            indices.clear();
        }
        std::size_t triangleCount() const { return indices.size() / 3; }
        std::size_t vertexCount() const { return vertices.size() / 2; }
        bool empty() const { return indices.empty(); }
    };

    /// 裁剪矩形（世界像素）。用于把三角化结果限制在一个分块内。
    ///
    /// 裁剪发生在**扫描带内部**：只跳过完全在矩形外的扫描带、把跨越边界的带夹到
    /// 矩形内、再把每段横跨区间的 x 夹到 [x0,x1]。因此输出三角形数与「该块内真实
    /// 有多少几何」成正比 —— 而不是「先全量三角化再逐块切分」那样成倍膨胀
    /// （实测世界层 52 万 → 166 万三角形、101 MB）。
    struct ClipRect {
        double x0 = 0.0;
        double y0 = 0.0;
        double x1 = 0.0;
        double y1 = 0.0;
    };

    /// 把一组环三角化（各环按 `fillRule` 联合求解，所以外环与洞要放进同一次调用）。
    /// `out` 会先被 `clear()`。退化输入（空环、少于 3 点、共线）不产三角形但不报错。
    /// `clip` 非空时只输出裁剪矩形内的部分。
    static void tessellate(const std::vector<Ring>& rings, FillRule fillRule, Mesh* out,
                           const ClipRect* clip = nullptr);

    /// 同上，但**先按环的嵌套关系分组**再逐组三角化。
    ///
    /// 真实数据里一个要素常常是「一堆互不相交的岛」而不是「外环 + 洞」：世界层的
    /// 日本要素有 352 个环，其中只有极少数是真洞。联合扫描会把 352 条环的边全部丢进
    /// 同一活动边表，每带配对出几十个横跨区间 —— 实测 93k 个梯形。分组后每组只含
    /// 「一个外环 + 它的直接子洞」，梯形数降到按岛分别计算的水平。
    ///
    /// 语义不变：分组只影响「哪些环一起参与配对」，even-odd 结果与联合扫描**逐像素
    /// 等价**（实测面积差 0.000000%）。嵌套深度按「更大的环是否包含它」定义，深度为
    /// 偶数的环开新面、其直接子环作为洞。
    static void tessellateNested(const std::vector<Ring>& rings, FillRule fillRule, Mesh* out,
                                 const ClipRect* clip = nullptr);


    /// 有符号面积（鞋带公式，各环分别求和后累加）。用于单测校验「三角化前后面积守恒」。
    static double signedArea(const std::vector<Ring>& rings);

    /// `mesh` 中所有三角形面积绝对值之和。无自相交输入下应与 `|signedArea|` 一致。
    static double meshArea(const Mesh& mesh);
};

} // namespace komira
