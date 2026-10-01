import QtQuick

// One section rhythm for settings and detail pages. Content remains solid, never
// recursively blurred; only the map-facing panels use a sampled backdrop.
GlassCard {
    id: root
    default property alias sectionContent: content.data
    property string title: ""
    property string iconName: "sliders-horizontal"
    implicitHeight: stack.implicitHeight + 40
    Column {
        id: stack
        x: 20; y: 20
        width: parent.width - 40
        spacing: 20
        Row {
            spacing: 10
            AppIcon { anchors.verticalCenter: parent.verticalCenter; name: root.iconName; size: 18; color: root.theme.accent }
            Text { text: root.title; color: root.theme.textPrimary; font.pixelSize: 16; font.weight: Font.Medium }
        }
        Rectangle { width: parent.width; height: 1; color: root.theme.glassBorder }
        Column { id: content; width: parent.width; spacing: 18 }
    }
}
