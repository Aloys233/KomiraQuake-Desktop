import QtQuick
import QtQuick.Controls

Switch {
    id: root
    property Theme theme: Theme {}
    property string description: ""
    implicitWidth: 300
    implicitHeight: Math.max(48, labels.implicitHeight + 12)
    leftPadding: 0; rightPadding: 64; topPadding: 6; bottomPadding: 6
    hoverEnabled: true
    opacity: enabled ? 1 : 0.55
    indicator: Rectangle {
        id: track
        x: root.width - width
        y: (root.height - height) / 2
        implicitWidth: 44; implicitHeight: 26
        radius: 13
        color: root.checked ? root.theme.accent : root.theme.surfaceContainerHigh
        border.width: root.visualFocus ? 2 : 1
        border.color: root.visualFocus ? root.theme.textPrimary : root.checked ? root.theme.accent : root.theme.outlineVariant
        Rectangle {
            width: 18; height: 18; radius: 9
            y: 4
            x: root.checked ? track.width - width - 4 : 4
            color: root.checked ? root.theme.accentForeground : root.theme.outline
            Behavior on x { NumberAnimation { duration: root.theme.motionDuration; easing.type: Easing.OutCubic } }
        }
    }
    contentItem: Column {
        id: labels
        spacing: 5
        Text { width: parent.width; text: root.text; color: root.theme.textPrimary; font.pixelSize: 14; wrapMode: Text.Wrap }
        Text { width: parent.width; visible: root.description.length; text: root.description; color: root.theme.outline; font.pixelSize: 12; wrapMode: Text.Wrap; lineHeight: 1.25 }
    }
}
