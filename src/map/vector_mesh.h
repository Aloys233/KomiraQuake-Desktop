#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/polygon_tessellator.h"
#include "core/vector_map_data.h"

namespace komira {

/// 把一层矢量几何构建成**可直接上传 GPU 的分块三角形网格**。
///
/// ## 为什么分块
///
/// 顶点坐标是「参考层级（z10）世界像素」，量级 0..262144。float32 在 2^18 附近的
/// 一个 ulp 是 0.015625 世界像素 —— 乘以 `2^(zoom-10)` 的屏幕缩放后，实测误差：
///
///   z10 → 0.016 px（可接受）   z14 → 0.25 px（勉强）   z18 → 4.0 px（不可接受）
///
/// 屏幕上一整块地图抖动 4 px 是明显可辨的。因此几何按 [kTileWorldPx] 分块，顶点坐标
/// 减去该块原点后再转 float32：块内坐标上限约 2×4096，z18 的误差降到 0.125 px 以下，
/// 且与世界位置无关（比「按当前缩放重新居中」省掉一次全量重建）。
///
/// ## 为什么填充与描边分开存索引、共用顶点
///
/// 填充是三角化的面（even-odd，洞自动镂空）；描边是沿折线展开的带状四边形。
/// 描边的**宽度必须恒定于屏幕像素**（地图缩小时线不能变粗糊住底图），所以宽度不烘进
/// 顶点坐标，只带外侧单位法线，由 shader 按当前缩放扩张 —— 见 `vector_mesh_material.h`。
///
/// 刻意不依赖 Qt：本文件进 `run_core_tests.sh` 的纯 g++ 编译路径，分块与三角化语义
/// 可以在没有显示环境的 CI 上直接断言。
class VectorMeshBuilder {
public:
    /// 分块边长（参考层级世界像素）。
    ///
    /// 这个值是「三角形膨胀」与「float32 精度」的折中，实测（真实 .kvb，tol=0.5）：
    ///
    ///   tile    world 三角形/无裁剪基线   district 比例   z18 精度误差
    ///   2048          3.71×                 1.18×          0.031 px
    ///   4096          2.36×                 1.09×          0.062 px
    ///   8192          1.68×                 1.04×          0.125 px   ← 选这个
    ///   32768         1.18×                 1.01×          0.500 px
    ///
    /// 每块都要单独裁剪三角化，块越小则跨块面被切得越碎（三角形越多）；块越大则块内
    /// 坐标越大、float32 精度越差。8192 在 z18（最大缩放）下误差 0.125 px，低于可见
    /// 阈值，同时世界层只膨胀 1.68 倍。
    static constexpr double kTileWorldPx = 8192.0;

    /// 一条环的**已投影**坐标（参考层级世界像素，交错 x,y）。
    using ProjectedRing = std::vector<double>;

    /// 一个要素：名称 + 已投影的环（首环外环，其余按 even-odd 处理）。
    struct Feature {
        std::string name;
        std::vector<ProjectedRing> rings;
    };

    /// GPU 顶点：位置（块内局部世界像素）+ 预乘前 RGBA 颜色。
    ///
    /// 字段布局与 `QSGGeometry::ColoredPoint2D`（x, y, r, g, b, a）一致，因此可以直接
    /// 用 Qt **内置**的 `QSGVertexColorMaterial` 绘制，不需要自定义 shader。
    ///
    /// 为什么不用自定义 shader：本项目实测（Xvfb + llvmpipe，GL 4.6 core）下，
    /// 自定义 `QSGMaterialShader` 的 `createShader()` 会被调用、`updateUniformData()`
    /// 也收到正确大小的 uniform 块，但几何始终不出像素 —— 而同一份几何换内置材质
    /// 立刻正常。那条路需要逐版本调试 Qt 的场景图/RHI 内部约定，收益（描边宽度随缩放
    /// 恒定）可以用「按 LOD 档位重建描边几何」以低得多的复杂度换到。
    ///
    /// 描边宽度因此**烘焙进顶点**：宽度按当前缩放的屏幕像素换算成世界像素，所以
    /// 只在缩放跨过 LOD 档位时才需要重建（与填充的简化档同一生命周期）。
    struct Vertex {
        float x = 0.0f;
        float y = 0.0f;
        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;
        uint8_t a = 0;
    };

    /// 一个分块的几何。
    struct Tile {
        /// 块原点（参考层级世界像素）。绘制时该块的变换要减去它。
        double originX = 0.0;
        double originY = 0.0;
        std::vector<Vertex> vertices;
        /// 填充索引（偶数下标为无填充色的纯几何；颜色在顶点里）。
        std::vector<uint32_t> fillIndices;
        std::vector<uint32_t> strokeIndices;
        /// 区县着色：要素下标 -> (起始填充索引, 索引数)。区县层每个着色区县单独一段，
        /// 因为它们的颜色各不相同。非区县层为空（整层一个颜色）。
        struct ShadedRun {
            int featureIndex = 0;
            uint32_t firstIndex = 0;
            uint32_t indexCount = 0;
        };
        std::vector<ShadedRun> shadedRuns;

        std::size_t bytes() const {
            return vertices.size() * sizeof(Vertex) +
                   (fillIndices.size() + strokeIndices.size()) * sizeof(uint32_t);
        }
        bool empty() const { return fillIndices.empty() && strokeIndices.empty(); }
    };

    /// 一层的完整几何（多块）。
    struct Layer {
        struct FeatureBounds {
            double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
        };
        std::vector<Tile> tiles;
        /// 各要素的包围盒（参考层级世界像素），供视口裁剪。与输入 features 下标对齐。
        std::vector<FeatureBounds> featureBounds;
        std::size_t vertexCount = 0;
        std::size_t fillTriangleCount = 0;
        std::size_t strokeTriangleCount = 0;

        std::size_t bytes() const {
            std::size_t total = 0;
            for (const Tile& t : tiles) total += t.bytes();
            return total;
        }
        bool empty() const { return tiles.empty(); }
    };
    using LayerPtr = std::shared_ptr<const Layer>;

    /// 构建样式。颜色与描边宽度**烘焙进顶点**，所以样式变化要重建。
    ///
    /// 描边宽度是屏幕像素，构建时按 `lodToleranceRef` 对应的缩放换算成世界像素
    ///（调用方通过 `worldPerScreenPixel` 给出当前比例）。这样描边在某个 LOD 档内
    /// 视觉宽度恒定；跨档重建时重新换算（实测 district 层重建约 20 ms，且只在
    /// 缩放跨档时发生）。
    struct Style {
        /// 填充色；alpha 为 0 表示不画填充（描边层/透明层）。
        uint8_t fillR = 0, fillG = 0, fillB = 0, fillA = 0;
        /// 描边色；alpha 为 0 表示不画描边。
        uint8_t strokeR = 0, strokeG = 0, strokeB = 0, strokeA = 0;
        /// 描边宽度（屏幕像素）。
        double strokeWidthPx = 1.0;
        /// 一个世界像素对应多少屏幕像素（= 2^(zoom-kRefZoom) * dpr）。
        /// 用来把 strokeWidthPx 换算成世界像素宽。
        double screenPerWorldPx = 1.0;
    };

    /// 从 .kvb 构建一层。
    static LayerPtr build(const VectorMapData& data, const std::string& layerName, bool gcjShift,
                          double lodToleranceRef, const Style& style);

    /// 从**已投影环**构建（`build` 去掉数据加载与投影之后的核心）。
    /// 测试用它直接喂几何，断言分块/三角化/描边。
    static LayerPtr buildFromFeatures(const std::vector<Feature>& features, double lodToleranceRef,
                                      const Style& style);
};

} // namespace komira
