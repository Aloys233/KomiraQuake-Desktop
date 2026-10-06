import QtQuick
import QtQuick.Layouts

// 地图 HUD。布局对齐 kanameishi 的信息块：左侧烈度色块，右侧「状态 / 震中 / 发震时刻 / 震级·深度·距离」。
GlassCard {
    id: root
    objectName: "hudCard"
    property var event
    property bool selected: false
    /// 多事件分页：当前页（0 基）与总数；>1 时显示左右切换。
    property int pageIndex: 0
    property int pageCount: 0
    signal prevPage()
    signal nextPage()
    readonly property bool hasPager: pageCount > 1
    visible: !!event
    implicitWidth: 352
    implicitHeight: mainRow.implicitHeight + 28 + (hasPager ? pager.implicitHeight + 6 : 0)
    // Active is seismic state, not a selected surface. Retain the frosted material.
    color: backdropSource ? (blurActive ? theme.backdropTint : theme.backdropFallback) : theme.glassCard
    border.color: theme.glassBorder
    radius: 16

    RowLayout {
        id: mainRow
        x: 14; y: 14
        width: parent.width - 28
        spacing: 12

        IntensityBadge {
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: 72
            Layout.preferredHeight: 72
            badgeSize: 72
            label: root.event ? root.event.intensityLabel : "预估烈度"
            standard: root.event && root.event.intensityIsLocal ? (app.settings.intensityStandard === 1 ? "JMA" : "中国烈度") : "来源标准"
            source: root.event ? root.event.sourceTag : ""
            value: root.event ? root.event.intensity : "--"
            badgeColor: root.event ? root.event.intensityColor : root.theme.surfaceContainer
            textColor: root.theme.on(badgeColor)
        }

        Column {
            Layout.fillWidth: true
            spacing: 4

            RowLayout {
                width: parent.width
                spacing: 6
                AppIcon {
                    name: root.active ? "radio" : "clock"; size: 14
                    color: root.active && root.event ? root.theme.severity(root.event.levelTag) : root.theme.outline
                    Layout.alignment: Qt.AlignVCenter
                }
                Text {
                    Layout.fillWidth: true
                    // 报文展示名（对齐 kanameishi 的 titleText），不再硬编码「地震预警」。
                    text: root.event ? root.event.source : ""
                    color: root.active && root.event ? root.theme.severity(root.event.levelTag) : root.theme.outline
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                Text {
                    text: root.event ? (root.event.isCanceled ? "取消报" : "第 " + root.event.reportNum + " 报") : ""
                    color: root.theme.outline; font.pixelSize: 11
                }
            }

            Text {
                width: parent.width
                text: root.event ? root.event.location : ""
                color: root.theme.textPrimary
                font.pixelSize: 19
                font.weight: Font.Medium
                wrapMode: Text.Wrap
                maximumLineCount: 2
                elide: Text.ElideRight
            }

            // 发震时刻独占一行：不参与任何伸缩，始终完整显示。
            Text {
                width: parent.width
                text: root.event ? root.event.timeText + "  UTC+8" : ""
                color: root.theme.outline
                font.family: root.theme.numberFamily
                font.pixelSize: 12
                elide: Text.ElideRight
            }

            RowLayout {
                width: parent.width
                spacing: 8
                Text {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignBaseline
                    text: root.event ? "M " + root.event.magnitudeText + " · 深度 " + root.event.depthText + " km" : ""
                    color: root.theme.outline
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                // 数据源标注（提供方·机构）：次要信息，放在震级/深度行右侧。
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

    // 多事件分页：紧凑一行，避免占用过多纵向空间。
    RowLayout {
        id: pager
        objectName: "hudPager"
        visible: root.hasPager
        x: 14; y: 14 + mainRow.implicitHeight + 6
        width: parent.width - 28
        spacing: 4
        GlassButton {
            theme: root.theme; flat: true; iconName: "chevron-left"; customRadius: 8
            implicitWidth: 24; implicitHeight: 24
            accessibleName: "上一个事件"; onClicked: root.prevPage()
        }
        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: (root.pageIndex + 1) + " / " + root.pageCount
            color: root.theme.outline
            font.family: root.theme.numberFamily
            font.pixelSize: 11
        }
        GlassButton {
            theme: root.theme; flat: true; iconName: "chevron-right"; customRadius: 8
            implicitWidth: 24; implicitHeight: 24
            accessibleName: "下一个事件"; onClicked: root.nextPage()
        }
    }
}
