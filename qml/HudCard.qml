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
    implicitHeight: mainRow.implicitHeight + 28 + (hasPager ? pager.implicitHeight + 8 : 0)
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
                    text: !root.event ? "" : root.event.isCanceled ? "已取消的事件" : root.event.ended ? "本次提醒已结束"
                          : !root.active ? (root.selected ? "已选目录事件" : "最近目录事件")
                          : root.event.levelTag === "CRITICAL" ? "严重地震预警" : "地震预警"
                    color: root.active && root.event ? root.theme.severity(root.event.levelTag) : root.theme.outline
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                Text { text: root.event ? "第 " + root.event.reportNum + " 报" : ""; color: root.theme.outline; font.pixelSize: 11 }
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

            RowLayout {
                width: parent.width
                spacing: 8
                Text {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignBaseline
                    text: root.event ? root.event.timeText + "  UTC+8" : ""
                    color: root.theme.outline
                    font.family: root.theme.numberFamily
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                Text {
                    Layout.alignment: Qt.AlignBaseline
                    text: root.event ? root.event.sourceTag : ""
                    color: root.theme.outline
                    font.pixelSize: 11
                }
            }

            Text {
                width: parent.width
                text: root.event ? "M " + root.event.magnitudeText + " · 深度 " + root.event.depthText + " km"
                      + (root.event.hasDistance ? " · 距你 " + root.event.distanceText + " km" : " · 本地距离未知") : ""
                color: root.theme.outline
                font.pixelSize: 12
                elide: Text.ElideRight
            }
        }
    }

    RowLayout {
        id: pager
        visible: root.hasPager
        x: 14; y: 14 + mainRow.implicitHeight + 8
        width: parent.width - 28
        spacing: 8
        GlassButton {
            theme: root.theme; flat: true; iconName: "chevron-left"
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
            theme: root.theme; flat: true; iconName: "chevron-right"
            accessibleName: "下一个事件"; onClicked: root.nextPage()
        }
    }
}
