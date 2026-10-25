import QtQuick

// 震中下方的浮动信息块（对齐 kanameishi 的震中浮层）：只放「看一眼就够」的几行
// —— 震中地名 / 发震时刻 / M·深度·烈度 / 经纬度 / 数据源，详细内容仍在左侧 HUD 与事件列表里。
//
// 默认不显示：指针悬停到震中十字上才出现（由 MapView 传入 shown）。地图上同时有
// 波前、测站、纠偏标记，常驻的文字标签会一直压在这些内容上面。
//
// 定位由 MapView 传入的 anchorX/anchorY（震中屏幕坐标）决定；默认挂在十字下方，
// 贴近视口底缘时自动翻到上方，左右也会夹在视口内 —— 否则跨 ±180° 或震中在屏幕
// 边缘时，信息块会被 mapView 的 clip 裁掉一半。
//
// 刻意不铺满整条宽度：它是一个"标签"而不是卡片，固定 220 px 宽、超出省略。
// 视觉沿用玻璃卡片的背景模糊（BackdropBlur），与地图上其它浮层同一套材质。
Item {
    id: root
    objectName: "epicenterInfo"
    property Theme theme: Theme {}
    /// 指针正悬停在震中十字上（见 MapView.epicenterHover）。
    property bool shown: false
    /// 背景模糊的采样源（MapView 传 mapRenderLayer）。
    property Item backdropSource: null
    /// 相机移动时才逐帧重采样，静止时用静态快照（见 BackdropBlur.live）。
    property bool backdropLive: false
    /// MapView 的 `event`（QVariantMap）。空对象表示没有要标注的震中。
    property var event: null
    property real anchorX: 0
    property real anchorY: 0
    property real viewportWidth: 0
    property real viewportHeight: 0

    readonly property bool hasData: !!event && event.id !== undefined
    readonly property string placeText: {
        if (!hasData) return "";
        const place = event.location !== undefined ? String(event.location) : "";
        return place.length ? place : "未知震源";
    }
    /// 与 HUD 同口径：M / 深度 / 烈度（本地有定位时是预估烈度，否则是源报最大烈度）。
    readonly property string metaText: {
        if (!hasData) return "";
        const parts = [];
        if (event.magnitudeText !== undefined)
            parts.push("M " + String(event.magnitudeText));
        if (event.depthText !== undefined)
            parts.push("深度 " + String(event.depthText) + " km");
        const value = event.intensity !== undefined ? String(event.intensity) : "";
        if (value.length > 0 && value !== "--") {
            const label = event.intensityLabel !== undefined
                ? String(event.intensityLabel) : "烈度";
            parts.push(label + " " + value);
        }
        return parts.join(" · ");
    }
    /// 发震时刻（UTC+8）：与 HUD / 事件列表同源，独占一行保证完整可见。
    readonly property string timeText: hasData && event.timeText !== undefined
        ? String(event.timeText) : ""
    /// 震中经纬度：此前这块浮层只有地名/时刻/震级/来源，看不出震中到底在哪一个点；
    /// 详情面板能读到的坐标，地图上贴着十字的地方也应该读得到。3 位小数 ≈ 100 m，
    /// 与测站悬停浮层同一套格式（`Theme::coordText`）。
    readonly property string coordText: hasData
        ? theme.coordText(Number(event.latitude), Number(event.longitude), 3) : ""
    /// 数据源 + 报数/情报态：次要信息，压缩到一行。
    readonly property string sourceText: {
        if (!hasData) return "";
        const tag = event.sourceTag !== undefined ? String(event.sourceTag) : "";
        // 报数文案由 AppController 成型：情报没有「第 N 报」语义（见 toMap）。
        const tail = event.reportNumText !== undefined ? String(event.reportNumText) : "";
        return tail.length ? (tag.length ? tag + " · " + tail : tail) : tag;
    }
    /// 默认挂在十字下方；下方剩余空间不够时翻到上方（避免贴底被裁）。
    readonly property bool below: anchorY + 26 + height <= viewportHeight - 8

    width: 220
    height: content.implicitHeight + 12
    visible: hasData && viewportWidth > 0 && shown
    x: Math.max(8, Math.min(viewportWidth - width - 8, anchorX - width / 2))
    y: below ? anchorY + 26 : anchorY - height - 26

    // 玻璃卡片材质：背景模糊 + 主题 tint / 边框。默认不接收鼠标事件（interactive
    // 为 false），悬停判定在地图上由 MapView 的震中热区负责，不在这里再截一次。
    GlassCard {
        anchors.fill: parent
        radius: 10
        theme: root.theme
        backdropSource: root.backdropSource
        backdropLive: root.backdropLive
    }

    Column {
        id: content
        x: 9; y: 6
        width: parent.width - 18
        spacing: 1

        Text {
            objectName: "epicenterInfoPlace"
            width: parent.width
            text: root.placeText
            color: root.theme.textPrimary
            font.pixelSize: 12
            font.weight: Font.DemiBold
            elide: Text.ElideRight
        }
        Text {
            objectName: "epicenterInfoTime"
            width: parent.width
            visible: text.length > 0
            text: root.timeText + (root.timeText.length ? "  UTC+8" : "")
            color: root.theme.outline
            font.family: root.theme.numberFamily
            font.pixelSize: 11
            elide: Text.ElideRight
        }
        Text {
            objectName: "epicenterInfoMeta"
            width: parent.width
            visible: text.length > 0
            text: root.metaText
            color: root.theme.outline
            font.family: root.theme.numberFamily
            font.pixelSize: 11
            elide: Text.ElideRight
        }
        Text {
            objectName: "epicenterInfoCoord"
            width: parent.width
            visible: text.length > 0
            text: root.coordText
            color: root.theme.outline
            font.family: root.theme.numberFamily
            font.pixelSize: 11
            elide: Text.ElideRight
        }
        Text {
            objectName: "epicenterInfoSource"
            width: parent.width
            visible: text.length > 0
            text: root.sourceText
            color: root.theme.outline
            opacity: 0.75
            font.pixelSize: 10
            elide: Text.ElideRight
        }
    }
}
