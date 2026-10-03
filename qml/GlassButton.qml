import QtQuick
import QtQuick.Controls

AbstractButton {
    id: root
    property Theme theme: Theme {}
    property bool primary: false
    property bool danger: false
    property bool flat: false
    property string iconName: ""
    property string accessibleName: text
    property int customRadius: 10
    readonly property color foregroundColor: !enabled ? theme.outline : primary ? theme.accentForeground
                                              : danger ? theme.severity("CRITICAL") : theme.textPrimary
    readonly property color backgroundColor: !enabled ? theme.surfaceContainer : primary ? (hovered || down ? theme.accentHover : theme.accent)
                                              : down ? theme.glassCardPressed : hovered ? theme.glassCardHover
                                              : flat ? "transparent" : danger ? theme.container("CRITICAL") : theme.surfaceContainer
    implicitWidth: text.length ? Math.max(80, contentRow.implicitWidth + 28) : 40
    implicitHeight: 40
    padding: 0
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    Accessible.name: accessibleName
    ToolTip.visible: hovered && !text.length && accessibleName.length
    ToolTip.text: accessibleName
    ToolTip.delay: 550
    HoverHandler { cursorShape: root.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor }
    background: Rectangle {
        radius: root.customRadius
        color: root.backgroundColor
        border.width: root.visualFocus ? 2 : 1
        border.color: root.visualFocus ? root.theme.accent : root.primary ? root.backgroundColor
                      : root.flat ? "transparent" : root.theme.glassBorder
    }
    contentItem: Item {
        Row {
            id: contentRow
            anchors.centerIn: parent
            spacing: 8
            AppIcon { anchors.verticalCenter: parent.verticalCenter; visible: root.iconName.length; name: root.iconName; color: root.foregroundColor; size: 18 }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: root.text.length
                text: root.text
                color: root.foregroundColor
                font.pixelSize: 13
                font.weight: Font.Normal
            }
        }
    }
}
