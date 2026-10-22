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

    // ---- 常驻文字标签 -----------------------------------------------------
    // 参考 kanameishi 的「推定震源」浮层：标记只有形状，两三区同屏时看不出谁是
    // 哪个台网、多强、多深。文字常驻，悬停 ToolTip 仍保留完整的诊断字段。
    //
    // 位置**不在这里算**：标签会互相压住，也可能伸出视口，必须四个标记一起排。
    // MapView 统一做「四方向候选 + 视口夹取 + 碰撞避让」，把结果写进 labelPlacement
    // （相对标记中心的左上角偏移，null 表示这次不显示）。
    property bool labelEnabled: false
    property var labelPlacement: null
    readonly property bool labelShown: labelEnabled && active && labelPlacement !== null
    /// 固定宽度而不是按内容撑开：宽度参与碰撞避让的矩形计算，跟着文本长度每帧抖动
    /// 会让标签在本该放得下的位置反复换边。超出部分省略。
    readonly property real labelWidth: 176
    /// 两行文本的排布高度（与下面 Column 的 spacing 对齐）。
    readonly property real labelHeight: labelColumn.implicitHeight
    readonly property string labelTitle: {
        if (!active) return "";
        const parts = [];
        if (info.label !== undefined) parts.push(String(info.label));
        if (info.grade !== undefined) parts.push("精度 " + String(info.grade));
        // 报数只在「解变了」时才增一，是判断结果是否在刷新的最快线索。
        if (info.reportNum !== undefined && Number(info.reportNum) > 0)
            parts.push("第 " + String(info.reportNum) + " 报");
        return parts.join(" · ");
    }
    readonly property string labelMeta: {
        if (!active) return "";
        const parts = [];
        if (info.magnitude !== undefined) parts.push("M " + Number(info.magnitude).toFixed(1));
        if (info.depthKm !== undefined) parts.push("深 " + Math.round(Number(info.depthKm)) + " km");
        if (info.stationCount !== undefined && Number(info.stationCount) > 0)
            parts.push(String(info.stationCount) + " 站");
        return parts.join(" · ");
    }

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
    // 常驻标签出现时它已经被标签首行取代，再画一次就是叠字。
    Text {
        anchors.top: parent.bottom
        anchors.topMargin: -12
        anchors.horizontalCenter: parent.horizontalCenter
        visible: !root.labelShown
        text: root.info && root.info.label !== undefined ? String(root.info.label) : ""
        color: root.theme.accent
        font.pixelSize: 9
        font.weight: Font.Medium
        style: Text.Outline
        styleColor: "#FFFFFF"
    }
    // 常驻标签：白字黑描边 / 黑字白描边（随主题反转），不做背景模糊 —— 四个模糊卡片
    // 会让场景图在静止时也持续重采样，代价与「一眼可读的两行字」不相称。
    Column {
        id: labelColumn
        objectName: "sourceEstimateLabel-" + root.regionId
        visible: root.labelShown
        x: root.width / 2 + (root.labelPlacement ? root.labelPlacement.dx : 0)
        y: root.height / 2 + (root.labelPlacement ? root.labelPlacement.dy : 0)
        width: root.labelWidth
        spacing: 1
        Text {
            objectName: "sourceEstimateLabelTitle-" + root.regionId
            width: parent.width
            text: root.labelTitle
            horizontalAlignment: Text.AlignHCenter
            color: root.theme.textPrimary
            font.pixelSize: 12
            font.weight: Font.DemiBold
            elide: Text.ElideRight
            style: Text.Outline
            styleColor: root.theme.dark ? "#000000" : "#FFFFFF"
        }
        Text {
            objectName: "sourceEstimateLabelMeta-" + root.regionId
            width: parent.width
            text: root.labelMeta
            horizontalAlignment: Text.AlignHCenter
            color: root.theme.textPrimary
            font.family: root.theme.numberFamily
            font.pixelSize: 11
            elide: Text.ElideRight
            style: Text.Outline
            styleColor: root.theme.dark ? "#000000" : "#FFFFFF"
        }
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
