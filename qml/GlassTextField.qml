import QtQuick
import QtQuick.Controls

TextField {
    id: root
    property Theme theme: Theme {}
    implicitHeight: 40
    leftPadding: 12; rightPadding: 12
    verticalAlignment: TextInput.AlignVCenter
    font.pixelSize: 13
    color: theme.textPrimary
    placeholderTextColor: theme.outline
    selectByMouse: true
    selectionColor: theme.accentContainer
    selectedTextColor: theme.accent
    opacity: enabled ? 1 : 0.55
    background: Rectangle {
        radius: 10
        color: root.theme.glassInput
        border.width: root.activeFocus ? 2 : 1
        border.color: root.activeFocus ? root.theme.accent : root.hovered ? root.theme.glassBorderHover : root.theme.glassBorder
    }
}
