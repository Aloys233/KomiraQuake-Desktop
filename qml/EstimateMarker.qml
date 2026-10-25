import QtQuick
import QtQuick.Controls

// 单个区域的推定震中标记。
//
// **形状与官方震中完全一致**（用户要求「震源推算的震中换成和其他震中一样的 X」）：
// 同一个 `HypocenterCross.qml`、同一组颜色（米黄打底 + 红描边）。两条渠道共用一份画法，
// 就不会出现「改了一边、另一边还是菱形」的漂移。
//
// 形状一致之后，区分官方/推定不再靠图形，而是靠文字：推定震中旁边常驻一块标签
// （首行写区域与精度、第 N 报），官方震中只有悬停时的 `EpicenterInfo` 小块。
// 不确定度圈仍然保留 —— 它是「这条解有多粗糙」的唯一几何表达，不是震中符号本身。
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
    // 哪个台网、多强、多深。用户还要求「浮动的文字加上经纬度，等等，现在有点简洁了」，
    // 所以标签从两行扩到四行：身份 → 坐标 → 震级深度站数 → 不确定度与残差诊断。
    //
    // 位置**不在这里算**：标签会互相压住，也可能伸出视口，必须四个标记一起排。
    // MapView 统一做「四方向候选 + 视口夹取 + 碰撞避让」，把结果写进 labelPlacement
    // （相对标记中心的左上角偏移，null 表示这次不显示）。
    property bool labelEnabled: false
    property var labelPlacement: null
    readonly property bool labelShown: labelEnabled && active && labelPlacement !== null
    /// 固定宽度而不是按内容撑开：宽度参与碰撞避让的矩形计算，跟着文本长度每帧抖动
    /// 会让标签在本该放得下的位置反复换边。超出部分省略。
    readonly property real labelWidth: 208
    /// 全部文本行的排布高度（与下面 Column 的 spacing 对齐）。
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
    /// 经纬度：印的是**载荷里的推定坐标**（与卡片、报文同源），不是 GCJ-02 纠偏后的值 ——
    /// 纠偏只决定「画在底图的哪个像素」，把纠偏后的数字印出来会和别处对不上。
    /// 与测站悬停浮层同一套格式（`StationTooltip`：3 位小数 + N/E）。
    readonly property string labelCoord: active
        ? root.theme.coordText(info.latitude, info.longitude, 3) : ""
    readonly property string labelMeta: {
        if (!active) return "";
        const parts = [];
        if (info.magnitude !== undefined) parts.push("M " + Number(info.magnitude).toFixed(1));
        if (info.depthKm !== undefined) parts.push("深 " + Math.round(Number(info.depthKm)) + " km");
        if (info.stationCount !== undefined && Number(info.stationCount) > 0)
            parts.push(String(info.stationCount) + " 站");
        return parts.join(" · ");
    }
    /// 诊断行：不确定度一定要写（用户点名要 ±km），再按解算路径补一个口径：
    ///   · 到时路径（日本区）→ P/S 相位数 + 到时残差（秒）；
    ///   · 强度路径 → 强度残差 RMS + 方位角最大空隙（覆盖越偏，空隙越大）。
    readonly property string labelDetail: {
        if (!active) return "";
        const parts = [];
        if (info.uncertaintyKm !== undefined && Number(info.uncertaintyKm) > 0)
            parts.push("±" + Math.round(Number(info.uncertaintyKm)) + " km");
        if (info.usesArrivalTimes === true) {
            // 只写有值的相位：强度路径的 S 恒为 0，写出来是噪声。
            const p = info.pPhases !== undefined ? Number(info.pPhases) : 0;
            const s = info.sPhases !== undefined ? Number(info.sPhases) : 0;
            if (p > 0 && s > 0) parts.push("P " + p + " · S " + s);
            else if (p > 0) parts.push("P " + p + " 相");
            else if (s > 0) parts.push("S " + s + " 相");
            if (info.rms !== undefined && isFinite(Number(info.rms)))
                parts.push("残差 " + Number(info.rms).toFixed(2) + " s");
        } else {
            if (info.rms !== undefined && isFinite(Number(info.rms)))
                parts.push("残差 " + Number(info.rms).toFixed(2));
            if (info.azimuthGap !== undefined && isFinite(Number(info.azimuthGap)))
                parts.push("空隙 " + Math.round(Number(info.azimuthGap)) + "°");
        }
        return parts.join(" · ");
    }
    /// 发震时刻：推算出来的（日本区到时解 / 其余区反推）。与卡片同一份文本。
    readonly property string labelOrigin: {
        if (!active) return "";
        const text = info.originUtc8 !== undefined ? String(info.originUtc8) : "";
        return text.length > 0 ? text + " UTC+8" : "";
    }

    objectName: primary ? "sourceEstimateMarker" : "sourceEstimateMarker-" + regionId
    visible: active
    width: 40
    height: 40

    // 不确定度圈在最底下：它不是震中符号，只表示「解可能落在这个半径内」。
    // 刻意用主题强调色（青）而不是十字的红：红色已经属于「震中」这个符号本身，
    // 圈再用红会让「解有多粗糙」被读成标记的一部分。
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
    // 与官方震中同款：10 px 米黄打底 + 6 px 红描边的 X 十字。
    HypocenterCross {
        objectName: root.primary ? "sourceEstimateCross"
                                 : "sourceEstimateCross-" + root.regionId
        anchors.centerIn: parent
        theme: root.theme
    }
    // 区域名直接标在十字下方：两三个标记同屏时，只靠悬停分不清谁是谁。
    // 常驻标签出现时它已经被标签首行取代，再画一次就是叠字。
    Text {
        anchors.top: parent.bottom
        anchors.topMargin: -10
        anchors.horizontalCenter: parent.horizontalCenter
        visible: !root.labelShown
        text: root.info && root.info.label !== undefined ? String(root.info.label) : ""
        color: root.theme.accent
        font.pixelSize: 9
        font.weight: Font.Medium
        style: Text.Outline
        styleColor: "#FFFFFF"
    }
    // 标签底衬：四行小字直接压在测站方标上（日本区一片 3/4/5 的方标）根本读不出来。
    // 刻意**不用** BackdropBlur —— 四个模糊卡片会让场景图静止时也持续重采样；
    // 一层半透明的主题色圆角矩形足以把字从底图里"抬"出来，代价几乎为零。
    Rectangle {
        objectName: "sourceEstimateLabelBackdrop-" + root.regionId
        visible: root.labelShown
        x: labelColumn.x - 6
        y: labelColumn.y - 4
        width: labelColumn.width + 12
        height: labelColumn.height + 8
        radius: 7
        // 深色主题下浅色地图/测站方标会从半透明底衬里透出来，所以深色比浅色更不透明。
        color: root.theme.dark ? "#D9101819" : "#DCF8FCFC"
        border.width: 1
        border.color: root.theme.dark ? "#55666A" : "#C6D6D9"
    }
    // 常驻标签：白字黑描边 / 黑字白描边（随主题反转），不做背景模糊 —— 四个模糊卡片
    // 会让场景图在静止时也持续重采样，代价与「一眼可读的四行字」不相称。
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
            objectName: "sourceEstimateLabelCoord-" + root.regionId
            width: parent.width
            visible: text.length > 0
            text: root.labelCoord
            horizontalAlignment: Text.AlignHCenter
            color: root.theme.textPrimary
            font.family: root.theme.numberFamily
            font.pixelSize: 11
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
        Text {
            objectName: "sourceEstimateLabelDetail-" + root.regionId
            width: parent.width
            visible: text.length > 0
            text: root.labelDetail
            horizontalAlignment: Text.AlignHCenter
            color: root.theme.textPrimary
            opacity: 0.8
            font.family: root.theme.numberFamily
            font.pixelSize: 10
            elide: Text.ElideRight
            style: Text.Outline
            styleColor: root.theme.dark ? "#000000" : "#FFFFFF"
        }
        Text {
            objectName: "sourceEstimateLabelOrigin-" + root.regionId
            width: parent.width
            visible: text.length > 0
            text: root.labelOrigin
            horizontalAlignment: Text.AlignHCenter
            color: root.theme.textPrimary
            opacity: 0.7
            font.family: root.theme.numberFamily
            font.pixelSize: 10
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
        const coord = root.theme.coordText(root.info.latitude, root.info.longitude, 3);
        if (coord.length > 0) parts.push(coord)
        if (root.info.magnitude !== undefined)
            parts.push("M " + Number(root.info.magnitude).toFixed(1))
        if (root.info.depthKm !== undefined) parts.push("深 " + Math.round(root.info.depthKm) + " km")
        if (root.info.uncertaintyKm !== undefined)
            parts.push("±" + Math.round(root.info.uncertaintyKm) + " km")
        return parts.join(" · ");
    }
    HoverHandler { id: hover }
}
