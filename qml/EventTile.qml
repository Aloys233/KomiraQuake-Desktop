import QtQuick
import QtQuick.Layouts

// 地震事件卡片。布局对齐 kanameishi 的列表项：
// 左侧烈度色块，右侧自上而下为「震中 / 发震时刻 / 震级·深度·距离」。无震级图标、无分割线。
// 整块内容在卡片内垂直居中；同行内的数字与说明文字按基线对齐并使用同一字体族。
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

    RowLayout {
        x: 14
        width: parent.width - 28
        anchors.verticalCenter: parent.verticalCenter
        spacing: 12

        // 列表固定展示「震源最大烈度」（震中当地量），本地预估烈度只出现在 HUD / 全屏预警。
        IntensityBadge {
            Layout.alignment: Qt.AlignVCenter
            badgeSize: 60
            label: root.event ? root.event.listIntensityLabel : "最大烈度"
            value: root.event ? root.event.listIntensity : "--"
            source: root.event ? root.event.sourceTag : ""
            badgeColor: root.event ? root.event.listIntensityColor : root.theme.surfaceContainer
            textColor: root.theme.on(badgeColor)
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            spacing: 5

            RowLayout {
                Layout.fillWidth: true
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

            // 发震时刻独占一行：不参与任何伸缩，始终完整显示。
            Text {
                Layout.fillWidth: true
                text: root.event ? root.event.timeText + "  UTC+8" : ""
                color: root.theme.outline
                font.family: root.theme.numberFamily
                font.pixelSize: 12
                elide: Text.ElideRight
            }

            // 震级与深度：同一字体族并按基线对齐；右侧为数据源标注。
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Text {
                    Layout.alignment: Qt.AlignBaseline
                    text: root.event ? "M " + root.event.magnitudeText : "M --"
                    color: root.theme.textPrimary
                    font.pixelSize: 15
                    font.weight: Font.Medium
                    font.family: root.theme.numberFamily
                }
                Text {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignBaseline
                    text: root.event ? "深度 " + root.event.depthText + " km" : ""
                    color: root.theme.outline
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                Text {
                    Layout.alignment: Qt.AlignBaseline
                    text: root.event ? root.event.sourceTag : ""
                    color: root.theme.outline
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }
            }
        }
    }
}
