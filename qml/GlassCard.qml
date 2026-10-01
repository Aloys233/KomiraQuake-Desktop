import QtQuick

Rectangle {
    id: root
    property Theme theme: Theme {}
    property Item backdropSource: null
    property bool interactive: false
    property bool active: false
    property string accessibleName: ""
    readonly property bool hovered: interactive && mouseArea.containsMouse
    readonly property bool pressed: interactive && mouseArea.pressed
    readonly property bool blurActive: !!backdropSource && theme.backgroundBlur
                                      && GraphicsInfo.api !== GraphicsInfo.Software
                                      && GraphicsInfo.api !== GraphicsInfo.Unknown
    signal clicked()
    radius: 16
    color: pressed ? theme.glassCardPressed : hovered || active ? theme.glassCardHover
         : backdropSource ? (blurActive ? theme.backdropTint : theme.backdropFallback) : theme.glassCard
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
        active: root.blurActive && root.visible
        sourceComponent: Component {
            BackdropBlur { sourceItem: root.backdropSource; cornerRadius: root.radius }
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
