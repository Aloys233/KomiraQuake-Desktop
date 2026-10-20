import QtQuick
import QtQuick.Controls

// 单个区域的推定震中标记。
//
// 与官方震中的红叉刻意区分：官方用「X 十字」，这里用「菱形 + 中心点 + 不确定度圈」，
// 形状不同就不会被误读成已发布震中。
//
// 每个区域一个实例（MapView 里固定四个）：不能用 Repeater 生成，因为 Repeater 的
// delegate 没有 QObject 父对象，`findChild` / objectName 定位不到，GUI 回归与可访问性
// 都依赖名字稳定。区域本来就是固定集合，四个实例反而更简单。
Item {
    id: root
    property Theme theme: Theme {}
    /// 该区域的标记载荷（`SourceEstimationService::marker()` 的 markers 元素）。
    property var info: ({})
    /// 区域 id（`china` / `japan` / `korea` / `taiwan`）。
    property string regionId: ""
    /// 是否为「信息最强的那一区」：它用裸名 `sourceEstimateMarker`，兼容旧调用与测试。
    property bool primary: false

    readonly property bool active: !!info && info.visible === true
    readonly property real lat: info && info.latitude !== undefined ? info.latitude : 0
    readonly property real lon: info && info.longitude !== undefined ? info.longitude : 0
    /// 水平不确定度（km），0 表示不画圈。
    readonly property real km: info && info.uncertaintyKm !== undefined ? info.uncertaintyKm : 0
    /// 当前缩放下每公里对应多少像素（由 MapView 注入）。
    property real kmPerPixel: 0
    /// 已纠偏的坐标（由 MapView 统一算好后注入）。旧实现让每个 x/y 绑定各自调用
    /// `shiftCoord()`，四次标记共 8 次跨语言调用 + 8 个临时对象。
    property var shifted: ({ lat: 0, lon: 0 })
    /// 不确定度圈半径（像素）；太小的圈会退化成点，绘制时按阈值忽略。
    readonly property real radiusPx: Math.max(0, km * kmPerPixel)

    objectName: primary ? "sourceEstimateMarker" : "sourceEstimateMarker-" + regionId
    visible: active
    width: 56
    height: 56

    Rectangle {
        anchors.centerIn: parent
        width: Math.max(0, 2 * root.radiusPx)
        height: width
        radius: width / 2
        visible: width > 10
        color: "transparent"
        border.width: 1.5
        border.color: root.theme.accent
        opacity: 0.7
    }
    // 白色外描边打底：测站方标底色跨度很大（深蓝→红），没有中性描边时菱角会糊进去。
    Rectangle {
        anchors.centerIn: parent
        width: 18; height: 18
        color: "transparent"
        border.width: 4
        border.color: "#FFFFFF"
        rotation: 45
        opacity: 0.92
    }
    Rectangle {
        anchors.centerIn: parent
        width: 18; height: 18
        color: "transparent"
        border.width: 2
        border.color: root.theme.accent
        rotation: 45
    }
    Rectangle {
        anchors.centerIn: parent
        width: 5; height: 5; radius: 2.5
        color: root.theme.accent
    }
    // 区域名直接标在菱形下方：两三个标记同屏时，只靠悬停分不清谁是谁。
    Text {
        anchors.top: parent.bottom
        anchors.topMargin: -12
        anchors.horizontalCenter: parent.horizontalCenter
        text: root.info && root.info.label !== undefined ? String(root.info.label) : ""
        color: root.theme.accent
        font.pixelSize: 9
        font.weight: Font.Medium
        style: Text.Outline
        styleColor: "#FFFFFF"
    }
    ToolTip.visible: hover.hovered
    ToolTip.delay: 400
    ToolTip.text: {
        if (!root.active) return "";
        const parts = ["本地推算震中（非官方）"]
        if (root.info.label !== undefined) parts.push(String(root.info.label))
        // 与卡片右下角同口径：S/A/B/C/D 叫「精度」，不叫「质量」。
        if (root.info.grade !== undefined) parts.push("精度 " + root.info.grade)
        if (root.info.magnitude !== undefined)
            parts.push("M " + Number(root.info.magnitude).toFixed(1))
        if (root.info.depthKm !== undefined) parts.push("深 " + Math.round(root.info.depthKm) + " km")
        if (root.info.uncertaintyKm !== undefined)
            parts.push("±" + Math.round(root.info.uncertaintyKm) + " km")
        return parts.join(" · ");
    }
    HoverHandler { id: hover }
}
