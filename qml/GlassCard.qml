import QtQuick

Rectangle {
    id: root
    property Theme theme: Theme {}
    property Item backdropSource: null
    /// 背景是否逐帧重采样（见 BackdropBlur.live）。地图卡片只在相机移动时打开。
    property bool backdropLive: true
    property bool interactive: false
    property bool active: false
    property string accessibleName: ""
    /// 玻璃底衬的**不透明度倍率**（1 = 主题本色）。堆叠里靠后的卡片用它把底衬调薄一点，
    /// 让底下被模糊的地图颜色真正透出来 —— 底衬太厚时，64 px 的模糊在 9 px 的纸边上
    /// 只剩一片平均值，看上去就是「纯色」，不像玻璃。
    property real tintOpacity: 1
    readonly property bool hovered: interactive && mouseArea.containsMouse
    readonly property bool pressed: interactive && mouseArea.pressed
    readonly property bool windowVisible: Window.window ? Window.window.visible : true
    readonly property bool blurSupported: GraphicsInfo.api !== GraphicsInfo.Software
                                          && GraphicsInfo.api !== GraphicsInfo.Unknown
    readonly property bool blurActive: blurSupported && theme.backgroundBlur && !!backdropSource
                                      && backdropSource.width > 0 && backdropSource.height > 0
                                      && windowVisible
    signal clicked()
    radius: 16
    /// 主题给的玻璃底衬，按 `tintOpacity` 缩放透明度（其余分支保持本色）。
    readonly property color tintedBackdrop: {
        const base = theme.backdropTint;
        return Qt.rgba(base.r, base.g, base.b, base.a * Math.max(0, Math.min(1, root.tintOpacity)));
    }
    color: pressed ? theme.glassCardPressed : hovered || active ? theme.glassCardHover
         : backdropSource ? (blurActive ? tintedBackdrop : theme.backdropFallback) : theme.glassCard
    border.width: activeFocus ? 2 : 1
    border.color: active || activeFocus ? theme.accent : hovered ? theme.glassBorderHover : theme.glassBorder
    activeFocusOnTab: interactive
    Accessible.role: interactive ? Accessible.Button : Accessible.Pane
    Accessible.name: accessibleName
    Accessible.onPressAction: if (interactive) clicked()
    Keys.onSpacePressed: if (interactive) clicked()
    Keys.onReturnPressed: if (interactive) clicked()

    // Negative-z blur lies under the material tint, not over the text.
    Loader {
        anchors.fill: parent
        z: -1
        active: root.blurActive && root.visible && root.windowVisible
        sourceComponent: Component {
            BackdropBlur {
                sourceItem: root.backdropSource
                cornerRadius: root.radius
                live: root.backdropLive
            }
        }
    }
    MouseArea {
        id: mouseArea
        anchors.fill: parent
        enabled: root.interactive
        hoverEnabled: root.interactive
        cursorShape: root.interactive ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: { root.forceActiveFocus(); root.clicked(); }
    }
}
