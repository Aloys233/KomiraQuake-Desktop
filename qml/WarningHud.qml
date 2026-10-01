import QtQuick
import QtQuick.Layouts

// 桌面端预警卡：替代全屏预警，位于主 HUD 下方，展示本地预计到时、避险指引与操作。
GlassCard {
    id: root
    objectName: "warningHud"
    property var event
    property int countdown: -1
    signal muted()
    signal stopped()
    readonly property bool hasCountdown: countdown >= 0
    readonly property bool arrived: countdown === 0
    readonly property bool cancelled: !!event && event.isCanceled
    readonly property color severityColor: event ? theme.severity(event.levelTag) : theme.outline
    visible: !!event
    implicitWidth: 352
    implicitHeight: content.implicitHeight + 28
    color: theme.glassCard
    border.color: theme.glassBorder
    radius: 16

    Column {
        id: content
        x: 14; y: 14
        width: parent.width - 28
        spacing: 10

        RowLayout {
            width: parent.width
            spacing: 8
            AppIcon { name: "triangle-alert"; size: 15; color: root.severityColor; Layout.alignment: Qt.AlignVCenter }
            Text {
                Layout.fillWidth: true
                text: !root.event ? "" : root.cancelled ? "预警已取消"
                      : !root.hasCountdown ? "本地 S 波到时未知"
                      : root.arrived ? "S 波预计已到达所在区域" : "本地 S 波预计到达"
                color: root.severityColor
                font.pixelSize: 13
                font.weight: Font.Medium
                elide: Text.ElideRight
            }
            Text { text: root.event ? "第 " + root.event.reportNum + " 报" : ""; color: root.theme.outline; font.pixelSize: 11 }
        }

        RowLayout {
            width: parent.width
            spacing: 10
            Text {
                visible: root.hasCountdown && !root.arrived
                text: root.countdown.toString()
                color: root.severityColor
                font.family: root.theme.numberFamily
                font.pixelSize: 44
                font.weight: Font.Bold
                Layout.alignment: Qt.AlignVCenter
            }
            Text {
                Layout.fillWidth: true
                text: !root.event ? "" : root.cancelled ? "已取消 · 不再提供到时预测"
                      : !root.hasCountdown ? "定位或走时数据不可用，无法估算本地到时"
                      : root.arrived ? "预计已到达 · 请继续避险，到达不代表危险解除"
                      : "秒后到达 · 请立即避险"
                color: root.theme.outline
                font.pixelSize: 12
                wrapMode: Text.Wrap
                Layout.alignment: Qt.AlignVCenter
            }
        }

        RowLayout {
            width: parent.width
            spacing: 8
            GlassButton { Layout.fillWidth: true; theme: root.theme; text: "静音本次"; iconName: "volume-x"; flat: true; onClicked: root.muted() }
            GlassButton { Layout.fillWidth: true; theme: root.theme; text: "停止本次提醒"; iconName: "bell-off"; danger: true; onClicked: root.stopped() }
        }
    }
}
