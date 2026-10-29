import QtQuick

// 地图上的**历史震中**标记（与「当前事件」的红色 X 区分开）。
//
// 为什么要和 `HypocenterCross.qml` 长得不一样：官方震中与推定震中都共用那个 40×40 的
// 米黄打底 + 红描边十字，它代表「这一场地震，地图正在讲它」。历史点是一批背景参照
// （用户设定的最近 N 个 / 最近 N 小时），形状必须一眼分得开，否则同屏几十个红叉会让
// 当前事件淹没在里面。这里用**实心小圆点 + 细描边**，半径随震级略放大（M3 以下 3 px，
// M7 以上 6 px），颜色同样按震级取色（与列表/HUD 的震级色同源，见 Theme.magnitudeColor）。
//
// 刻意不写成 Canvas：同屏可能上百个点，Canvas 每帧各自 getContext + 半透明叠色，
// 在软件后端上是实打实的开销。一个圆角矩形（radius = 半宽）已经足够表达圆点，
// 且能直接吃 Qt 的批处理。
Item {
    id: root
    property Theme theme: Theme {}
    /// 标记载荷（`AppController::historyMarkers` 的元素）。
    property var info: ({})
    /// 已纠偏的屏幕坐标（由 MapView 统一算好注入：纠偏要跨 C++ 边界，不该逐点各调一次）。
    property real markerX: 0
    property real markerY: 0
    /// 指针正停在这个点上（MapView 统一做命中判定）。
    property bool highlighted: false

    readonly property bool active: !!info && info.latitude !== undefined
    readonly property real magnitude: info && info.magnitudeText !== undefined
                                      ? Number(info.magnitudeText) : 0
    /// 点位直径：随震级 2 px 一档，套在 [6, 12] 里。太小就点不到，太大就盖住底图。
    readonly property real dotSize: Math.max(6, Math.min(12, 5 + magnitude * 0.9))
    /// 悬停时略微放大并加一圈高亮环 —— 与测站浮层的「指到才显形」同一手感，
    /// 但不改变点位本身（玩家扫过时不会跳来跳去）。
    readonly property real ringSize: dotSize + (highlighted ? 10 : 6)

    visible: active
    x: markerX - width / 2
    y: markerY - height / 2
    width: ringSize
    height: ringSize
    // 与「当前事件」十字的层级关系：历史点在下（markerLayer 之前声明），
    // 十字与提示卡永远压在上面 —— 否则最近一条震中可能被一个历史点盖住。
    z: 0

    Rectangle {
        objectName: "historyEpicenterRing"
        anchors.centerIn: parent
        width: root.ringSize
        height: root.ringSize
        radius: width / 2
        color: "transparent"
        border.width: 1
        border.color: Qt.rgba(root.theme.hypocenterHalo.r, root.theme.hypocenterHalo.g,
                              root.theme.hypocenterHalo.b, root.highlighted ? 0.95 : 0.35)
    }

    Rectangle {
        objectName: "historyEpicenterDot"
        anchors.centerIn: parent
        width: root.dotSize
        height: root.dotSize
        radius: width / 2
        color: root.theme.magnitudeColor(root.magnitude)
        border.width: 1
        border.color: root.theme.surfaceContainerLow
    }
}
