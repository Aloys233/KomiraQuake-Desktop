import QtQuick
import QtQuick.Layouts

// 测站悬停浮层。上千个测站由 C++ 一次批量绘制，没有逐站 Item，因此这里只有**一个**
// 复用实例：指针停下后向 StationMapItem.stationAt() 要一次命中结果，再按结果摆位。
//
// 材质：与其它地图玻璃卡一致（BackdropBlur → 半透明底衬 → 细边框 → 清晰前景）。
//
// 性能约束（改动前先读）：
// - 模糊源**必须** live=false：live 的 ShaderEffectSource 会让 SceneGraph 永不空闲，
//   窗口静止时也以 ~86 fps 重采样整棵地图子树（实测约 25% CPU）。浮层只在指针停稳后
//   出现，由 BackdropBlur 内部的 500 ms 补帧定时器低频刷新即可；指针移动时浮层本来
//   就会收起/重排，不需要逐帧模糊。
// - 模糊源只取地图渲染层（backdropSource），绝不包含浮层自身，否则会递归捕获。
// - 不逐像素命中：stationAt() 是 O(n) 纯算术（缓存的投影 Y），鼠标每动一次算一遍，
//   2500 站也远低于一帧预算。
// - 悬停去抖：指针停止 90 ms 后才查询并显示，扫过地图时不会闪出一串浮层。
Item {
    id: root
    objectName: "stationTooltip"
    /// 命中的测站：`{ found, index, name, area, code, type, source, lat, lon, raw,
    /// csis, kma, standard, intensity }`。
    property var info: ({})
    property Theme theme: Theme {}
    /// 模糊采样源：MapView 的地图渲染层。为空（如测试/独立预览）时退回不透明玻璃底色。
    property Item backdropSource: null
    /// 浮层锚点（地图坐标，逻辑像素）。
    property point anchor: Qt.point(0, 0)
    /// 视口尺寸：用于把浮层夹在视口内。
    property size viewport: Qt.size(0, 0)
    readonly property bool hasInfo: !!info && info.found === true
    readonly property string nameText: {
        if (!hasInfo) return "";
        const name = info.name || "";
        return name.length ? name : (info.area || info.code || "测站");
    }
    readonly property string subtitleText: {
        if (!hasInfo) return "";
        // 站名缺失（KMA 上游只给坐标）时用坐标兜底，避免出现「名称 = 坐标」的重复行。
        return (info.area && info.area !== nameText) ? info.area : ""
    }
    readonly property string codeText: {
        if (!hasInfo) return "";
        const parts = [];
        if (info.code && info.code.length) parts.push(info.code);
        if (info.type && info.type.length && info.type !== info.code) parts.push(info.type);
        return parts.join(" · ");
    }
    // 与地图标签 / 事件信息块同一套坐标格式（`Theme::coordText`，3 位小数 + 半球）。
    readonly property string coordinateText: hasInfo
        ? theme.coordText(Number(info.lat), Number(info.lon), 3) : ""
    readonly property color valueColor: {
        if (!hasInfo) return theme.textPrimary;
        return theme.stationTextColorFor(info.raw, info.standard);
    }
    readonly property string standardLabel: {
        if (!hasInfo) return "";
        if (info.standard === "csis") return "中国烈度";
        if (info.standard === "kma") return "韩国 MMI";
        return "計測震度";
    }

    // 布局尺寸先声明，摆位依赖它；浮层宽度固定，避免长站名让卡片抖动。
    readonly property real cardWidth: 216
    width: cardWidth
    height: body.implicitHeight + 20
    // 不做淡入淡出：显示期间指针可能继续移动并改写 info，过渡反而会让内容「糊一下」。
    // 去抖（90 ms）已经保证了不会沿路闪出一串浮层。
    visible: hasInfo

    x: {
        if (!hasInfo) return 0;
        const margin = 12;
        const gap = 18;
        let nx = root.anchor.x + gap;
        // 右侧放不下就翻到指针左侧；两侧都放不下时贴住右边界。
        if (nx + root.width + margin > root.viewport.width)
            nx = root.anchor.x - gap - root.width;
        return Math.max(margin, Math.min(nx, root.viewport.width - root.width - margin));
    }
    y: {
        if (!hasInfo) return 0;
        const margin = 12;
        let ny = root.anchor.y - root.height / 2;
        return Math.max(margin, Math.min(ny, root.viewport.height - root.height - margin));
    }
    z: 20
    // 浮层本身不接收鼠标：否则指针一进入浮层就会打断地图的悬停跟踪。
    enabled: false

    readonly property bool blurSupported: GraphicsInfo.api !== GraphicsInfo.Software
                                          && GraphicsInfo.api !== GraphicsInfo.Unknown
    readonly property bool windowVisible: Window.window ? Window.window.visible : true
    readonly property bool blurActive: blurSupported && theme.backgroundBlur && !!backdropSource
                                      && backdropSource.width > 0 && backdropSource.height > 0
                                      && windowVisible
    readonly property int cornerRadius: 12

    // 与 GlassCard 同构：负 z 的模糊在底衬之下、文字之上不会糊到前景。
    Loader {
        anchors.fill: parent
        z: -1
        active: root.blurActive && root.visible
        sourceComponent: Component {
            BackdropBlur {
                sourceItem: root.backdropSource
                cornerRadius: root.cornerRadius
                // 固定低频补帧：live 会让场景图永不空闲（见文件头注释）。
                live: false
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: root.cornerRadius
        // 有模糊时用半透明主题底衬；不支持模糊（软件后端/关闭模糊）时退回不透明卡片色。
        color: root.blurActive ? root.theme.backdropTint : root.theme.glassCard
        border.width: 1
        border.color: root.theme.glassBorder
    }

    ColumnLayout {
        id: body
        x: 12
        y: 10
        width: root.width - 24
        spacing: 5

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            // 烈度色块：与地图上的方标同色系，一眼能对上刚指的那个站。
            Rectangle {
                Layout.preferredWidth: 34
                Layout.preferredHeight: 34
                radius: 9
                color: root.hasInfo
                       ? root.theme.stationFillColorFor(root.info.raw, root.info.standard)
                       : root.theme.surfaceContainer
                Text {
                    anchors.centerIn: parent
                    text: root.hasInfo ? root.info.intensity : "--"
                    color: root.valueColor
                    font.family: root.theme.numberFamily
                    font.pixelSize: 16
                    font.weight: Font.Bold
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                Text {
                    Layout.fillWidth: true
                    text: root.nameText
                    color: root.theme.textPrimary
                    font.pixelSize: 13
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                    maximumLineCount: 1
                }
                Text {
                    Layout.fillWidth: true
                    visible: root.subtitleText.length > 0
                    text: root.subtitleText
                    color: root.theme.outline
                    font.pixelSize: 11
                    elide: Text.ElideRight
                    maximumLineCount: 1
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: root.theme.glassBorder
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            Text {
                text: root.standardLabel
                color: root.theme.outline
                font.pixelSize: 11
            }
            Item { Layout.fillWidth: true }
            Text {
                text: root.hasInfo ? root.info.source : ""
                color: root.theme.outline
                font.pixelSize: 11
                elide: Text.ElideRight
            }
        }
        Text {
            Layout.fillWidth: true
            visible: root.codeText.length > 0
            text: root.codeText
            color: root.theme.outline
            font.family: root.theme.numberFamily
            font.pixelSize: 11
            elide: Text.ElideRight
        }
        Text {
            Layout.fillWidth: true
            text: root.coordinateText
            color: root.theme.outline
            font.family: root.theme.numberFamily
            font.pixelSize: 11
        }
    }
}
