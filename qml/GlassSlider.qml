import QtQuick
import QtQuick.Controls

Slider {
    id: root
    property Theme theme: Theme {}
    implicitWidth: 240; implicitHeight: 36
    hoverEnabled: true
    opacity: enabled ? 1 : 0.55
    background: Rectangle {
        x: root.leftPadding
        y: root.topPadding + root.availableHeight / 2 - height / 2
        width: root.availableWidth; height: 4; radius: 2
        color: root.theme.surfaceContainerHigh
        Rectangle { width: root.visualPosition * parent.width; height: 4; radius: 2; color: root.theme.accent }
    }
    handle: Rectangle {
        x: root.leftPadding + root.visualPosition * (root.availableWidth - width)
        y: root.topPadding + root.availableHeight / 2 - height / 2
        implicitWidth: 20; implicitHeight: 20; radius: 10
        color: root.theme.accent
        border.width: root.visualFocus ? 3 : 2
        border.color: root.visualFocus ? root.theme.textPrimary : root.theme.surfaceContainerLow
    }
}
