import QtQuick
import QtQuick.Controls

ComboBox {
    id: root
    property Theme theme: Theme {}
    implicitWidth: 220
    implicitHeight: 40
    leftPadding: 12
    rightPadding: 36
    hoverEnabled: true
    opacity: enabled ? 1 : 0.55
    background: Rectangle {
        radius: 10
        color: root.theme.glassInput
        border.width: root.visualFocus ? 2 : 1
        border.color: root.visualFocus ? root.theme.accent : root.hovered ? root.theme.glassBorderHover : root.theme.glassBorder
    }
    contentItem: Text {
        text: root.displayText
        color: root.theme.textPrimary
        font.pixelSize: 13
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    // Replace Basic's indicator, rather than drawing a second arrow in contentItem.
    indicator: AppIcon {
        objectName: "comboIndicator"
        x: root.width - width - 12
        y: (root.height - height) / 2
        size: 16
        name: "chevron-down"
        color: root.theme.outline
    }
    popup: Popup {
        y: root.height + 6
        width: root.width
        implicitHeight: Math.min(280, contentItem.implicitHeight + 12)
        padding: 6
        background: Rectangle { radius: 12; color: root.theme.surfaceContainerLow; border.width: 1; border.color: root.theme.glassBorderHover }
        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: root.popup.visible ? root.delegateModel : null
            currentIndex: root.highlightedIndex
            ScrollIndicator.vertical: ScrollIndicator {}
        }
    }
    delegate: ItemDelegate {
        id: entry
        width: root.width - 12
        height: 40
        highlighted: root.highlightedIndex === index
        background: Rectangle { radius: 7; color: entry.highlighted || entry.hovered ? root.theme.accentContainer : "transparent" }
        contentItem: Text {
            text: modelData
            color: entry.highlighted ? root.theme.accent : root.theme.textPrimary
            font.pixelSize: 13
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
    }
}
