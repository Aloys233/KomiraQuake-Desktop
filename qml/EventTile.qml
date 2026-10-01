import QtQuick
import QtQuick.Layouts

// 地震事件卡片。布局对齐 kanameishi 的列表项：
// 左侧烈度色块，右侧自上而下为「震中 / 发震时刻 / 震级·深度·距离」。无震级图标、无分割线。
GlassCard {
    id: root
    property var event
    readonly property bool isFocused: root.event ? app.isMapFocused(root.event.id) : false
    implicitHeight: 92
    radius: 12
    interactive: true
    active: isFocused
    accessibleName: event ? event.location + "，震级 " + event.magnitudeText + "，点击在地图查看" : "地震事件"
    onClicked: if (event) app.toggleMapFocus(event.id)

    Row {
        x: 14; y: 14
        width: parent.width - 28
        spacing: 12

        IntensityBadge {
            badgeSize: 60
            label: root.event ? root.event.intensityLabel : "预估烈度"
            value: root.event ? root.event.intensity : "--"
            source: root.event ? root.event.sourceTag : ""
            badgeColor: root.event ? root.event.intensityColor : root.theme.surfaceContainer
            textColor: root.theme.on(badgeColor)
        }

        Column {
            width: parent.width - 60 - 12
            spacing: 5

            RowLayout {
                width: parent.width
                spacing: 6
                Rectangle {
                    visible: root.event ? root.event.isActive : false
                    width: 6; height: 6; radius: 3
                    color: root.theme.accent
                    Layout.alignment: Qt.AlignVCenter
                }
                Text {
                    Layout.fillWidth: true
                    text: root.event ? root.event.location : ""
                    color: root.theme.textPrimary
                    font.pixelSize: 15
                    elide: Text.ElideRight
                }
                AppIcon { visible: root.isFocused; name: "map-pin"; size: 13; color: root.theme.accent; Layout.alignment: Qt.AlignVCenter }
            }

            // 发震时刻写全，置于中部；来源标注靠右。
            RowLayout {
                width: parent.width
                spacing: 8
                Text {
                    Layout.fillWidth: true
                    text: root.event ? root.event.timeText + "  UTC+8" : ""
                    color: root.theme.outline
                    font.family: root.theme.numberFamily
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                Text {
                    text: root.event ? root.event.sourceTag : ""
                    color: root.theme.outline
                    font.pixelSize: 11
                }
            }

            RowLayout {
                width: parent.width
                spacing: 8
                Text {
                    text: root.event ? "M " + root.event.magnitudeText : "M --"
                    color: root.theme.textPrimary
                    font.pixelSize: 15
                    font.weight: Font.Medium
                    font.family: root.theme.numberFamily
                }
                Text {
                    Layout.fillWidth: true
                    text: root.event ? "深度 " + root.event.depthText + " km"
                          + (root.event.hasDistance ? " · 距你 " + root.event.distanceText + " km" : " · 距离未知") : ""
                    color: root.theme.outline
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
            }
        }
    }
}
