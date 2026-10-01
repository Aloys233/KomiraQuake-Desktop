import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Solid safety surface: no decorative pulse and no blur over urgent information.
Rectangle {
    id: root
    objectName: "warningOverlay"
    property Theme theme: Theme {}
    property var event
    property int countdown: -1
    signal dismissed()
    signal muted()
    signal stopped()
    readonly property bool hasCountdown: countdown >= 0
    readonly property bool arrived: countdown === 0
    readonly property color severityColor: event ? theme.severity(event.levelTag) : theme.surfaceContainerLow
    readonly property color foreground: theme.on(severityColor)
    color: severityColor
    MouseArea { anchors.fill: parent }
    ScrollView {
        anchors.fill: parent
        contentWidth: availableWidth
        clip: true
        Column {
            width: Math.min(760, parent.width - 40)
            x: (parent.width - width) / 2
            topPadding: 28
            bottomPadding: 32
            spacing: 22
            RowLayout {
                width: parent.width
                AppIcon { name: "triangle-alert"; size: 24; color: root.foreground }
                Text { Layout.fillWidth: true; text: root.event && root.event.levelTag === "CRITICAL" ? "严重地震预警" : "地震预警"; color: root.foreground; font.pixelSize: 22; font.weight: Font.Medium }
                Text { text: root.event ? "第 " + root.event.reportNum + " 报" : ""; color: root.foreground; font.pixelSize: 13 }
            }
            GlassCard {
                width: parent.width
                theme: root.theme
                radius: 24
                implicitHeight: warningContent.implicitHeight + 40
                Column {
                    id: warningContent
                    width: parent.width - 40
                    x: 20; y: 20
                    spacing: 18
                    Text {
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        text: !root.hasCountdown ? "本地 S 波到时未知" : root.arrived ? "S 波预计已到达所在区域" : "本地 S 波预计到达"
                        color: root.theme.textPrimary
                        font.pixelSize: 20
                        wrapMode: Text.Wrap
                    }
                    Text {
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        visible: root.hasCountdown && !root.arrived
                        text: root.countdown.toString()
                        font.family: root.theme.numberFamily
                        font.pixelSize: 104
                        font.weight: Font.Black
                        color: root.severityColor
                    }
                    Text {
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        text: !root.hasCountdown ? "定位或走时数据不可用，无法估算本地到时" : root.arrived ? "请继续避险，预计到达不代表危险解除" : "秒后到达 · 请立即避险"
                        color: root.theme.outline
                        font.pixelSize: 13
                        wrapMode: Text.Wrap
                    }
                    Rectangle { width: parent.width; height: 1; color: root.theme.glassBorder }
                    Text { width: parent.width; text: root.event ? root.event.location : ""; color: root.theme.textPrimary; font.pixelSize: 24; wrapMode: Text.Wrap }
                    Text {
                        width: parent.width
                        text: root.event ? "M " + root.event.magnitudeText + " · 深度 " + root.event.depthText + " km" + (root.event.hasDistance ? " · 距你 " + root.event.distanceText + " km" : " · 本地距离未知") : ""
                        color: root.theme.outline
                        font.pixelSize: 14
                        wrapMode: Text.Wrap
                    }
                    RowLayout {
                        width: parent.width
                        IntensityBadge { badgeSize: 64; label: root.event ? root.event.intensityLabel : "预估烈度"; value: root.event ? root.event.intensity : "--"; badgeColor: root.event ? root.event.intensityColor : root.theme.surfaceContainer; textColor: root.theme.on(badgeColor) }
                        Text {
                            Layout.fillWidth: true
                            text: root.event ? (root.event.intensityIsLocal ? "本地预估烈度" : "来源最大烈度") + "\n" + root.event.sourceTag : ""
                            color: root.theme.outline
                            font.pixelSize: 13
                            lineHeight: 1.6
                            wrapMode: Text.Wrap
                        }
                    }
                    Rectangle { width: parent.width; height: 1; color: root.theme.glassBorder }
                    RowLayout {
                        width: parent.width
                        AppIcon { name: "shield"; size: 24; color: root.theme.textPrimary }
                        Text { Layout.fillWidth: true; text: "伏地 · 遮挡 · 抓牢"; color: root.theme.textPrimary; font.pixelSize: 22; wrapMode: Text.Wrap }
                    }
                    Text { width: parent.width; text: "远离窗户与坠落物。预计到达或提醒结束，不代表危险解除。"; color: root.theme.outline; font.pixelSize: 13; wrapMode: Text.Wrap; lineHeight: 1.4 }
                    Flow {
                        width: parent.width
                        spacing: 10
                        GlassButton { theme: root.theme; text: "收起全屏"; iconName: "chevron-down"; onClicked: root.dismissed() }
                        GlassButton { theme: root.theme; text: "静音本次"; iconName: "volume-x"; onClicked: root.muted() }
                        GlassButton { theme: root.theme; text: "停止本次提醒"; iconName: "bell-off"; danger: true; onClicked: root.stopped() }
                    }
                    Text { width: parent.width; text: "收起保留提醒；静音保留视觉倒计时；停止不代表安全。"; color: root.theme.outline; font.pixelSize: 11; wrapMode: Text.Wrap }
                }
            }
        }
    }
}
