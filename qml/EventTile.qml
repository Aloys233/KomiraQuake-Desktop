import QtQuick
import QtQuick.Layouts

// 地震事件卡片。布局对齐 kanameishi 的列表项：
// 左侧烈度色块，右侧自上而下为「震中 / 发震时刻 / 震级·深度·距离」。无震级图标、无分割线。
// 整块内容在卡片内垂直居中；同行内的数字与说明文字按基线对齐并使用同一字体族。
//
// 数据来源是 EventListModel 的**逐列 role**（不是一张 QVariantMap）：delegate 只声明
// 自己真正需要的字段，`model.event` 那种「整张 map 复制进 JS 对象」的开销不再存在，
// 单行更新也只触发 `dataChanged(该行)`，其余 delegate 不受影响。
GlassCard {
    id: root
    // required property 而非默认值：漏绑会在创建时报错，而不是静默显示空白卡片。
    // 角色名统一带 event 前缀，避免与 Item/GlassCard 的内置属性撞名。
    required property string eventId
    required property string eventLocation
    required property string eventMagnitudeText
    required property string eventDepthText
    required property string eventTimeText
    required property string eventSourceTag
    required property string eventIntensity
    required property string eventIntensityLabel
    required property string eventIntensityColor
    required property bool eventActive
    required property bool eventFocused

    implicitHeight: 92
    radius: 12
    interactive: true
    active: eventFocused
    accessibleName: eventLocation + "，震级 " + eventMagnitudeText + "，点击在地图查看"
    onClicked: app.toggleMapFocus(eventId)

    RowLayout {
        x: 14
        width: parent.width - 28
        anchors.verticalCenter: parent.verticalCenter
        spacing: 12

        // 列表固定展示「震源最大烈度」（震中当地量），本地预估烈度只出现在 HUD / 全屏预警。
        IntensityBadge {
            Layout.alignment: Qt.AlignVCenter
            badgeSize: 60
            label: root.eventIntensityLabel
            value: root.eventIntensity
            source: root.eventSourceTag
            badgeColor: root.eventIntensityColor
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
                    visible: root.eventActive
                    width: 6; height: 6; radius: 3
                    color: root.theme.accent
                    Layout.alignment: Qt.AlignVCenter
                }
                Text {
                    Layout.fillWidth: true
                    text: root.eventLocation
                    color: root.theme.textPrimary
                    font.pixelSize: 15
                    elide: Text.ElideRight
                }
                AppIcon { visible: root.eventFocused; name: "map-pin"; size: 13; color: root.theme.accent; Layout.alignment: Qt.AlignVCenter }
            }

            // 发震时刻独占一行：不参与任何伸缩，始终完整显示。
            Text {
                Layout.fillWidth: true
                text: root.eventTimeText + "  UTC+8"
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
                    text: "M " + root.eventMagnitudeText
                    color: root.theme.textPrimary
                    font.pixelSize: 15
                    font.weight: Font.Medium
                    font.family: root.theme.numberFamily
                }
                Text {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignBaseline
                    text: "深度 " + root.eventDepthText + " km"
                    color: root.theme.outline
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                Text {
                    Layout.alignment: Qt.AlignBaseline
                    text: root.eventSourceTag
                    color: root.theme.outline
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }
            }
        }
    }
}
