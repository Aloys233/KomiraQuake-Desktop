import QtQuick
import QtQuick.Layouts

// 桌面端预警卡：替代全屏预警，位于**主 HUD 右侧**，展示本地预计到时与操作。
//
// 尺寸取舍：倒计时是这一屏最该被一眼读到的数字，但它不该抢走地图的横向空间。
// 因此卡片收窄到 264 px、数字降到 32 px，并把两个操作键压成紧凑的一行（图标 + 两字标签），
// 完整措辞保留在 Accessible.name 里 —— 屏幕阅读器与自动化测试仍看到「静音本次 / 停止本次提醒」。
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
    implicitWidth: 264
    implicitHeight: content.implicitHeight + 24
    // 与主 HUD 一致：采样地图作为毛玻璃底，文字落在材质之上。
    color: backdropSource ? (blurActive ? theme.backdropTint : theme.backdropFallback) : theme.glassCard
    border.color: theme.glassBorder
    radius: 14

    Column {
        id: content
        x: 12; y: 12
        width: parent.width - 24
        spacing: 8

        RowLayout {
            width: parent.width
            spacing: 6
            AppIcon { name: "triangle-alert"; size: 13; color: root.severityColor; Layout.alignment: Qt.AlignVCenter }
            Text {
                Layout.fillWidth: true
                text: !root.event ? "" : root.cancelled ? "预警已取消"
                      : !root.hasCountdown ? "本地 S 波到时未知"
                      : root.arrived ? "S 波预计已到达" : "本地 S 波预计到达"
                color: root.severityColor
                font.pixelSize: 12
                font.weight: Font.Medium
                elide: Text.ElideRight
            }
            Text {
                text: root.event && root.event.reportNumText !== undefined
                      ? String(root.event.reportNumText) : ""
                color: root.theme.outline; font.pixelSize: 10
            }
        }

        RowLayout {
            width: parent.width
            spacing: 8
            Text {
                visible: root.hasCountdown && !root.arrived
                text: root.countdown.toString()
                color: root.severityColor
                font.family: root.theme.numberFamily
                font.pixelSize: 32
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
                font.pixelSize: 11
                wrapMode: Text.Wrap
                Layout.alignment: Qt.AlignVCenter
            }
        }

        RowLayout {
            width: parent.width
            spacing: 6
            GlassButton {
                Layout.fillWidth: true
                theme: root.theme; text: "静音"; iconName: "volume-x"; flat: true
                implicitHeight: 30; customRadius: 8
                accessibleName: "静音本次"
                onClicked: root.muted()
            }
            GlassButton {
                Layout.fillWidth: true
                theme: root.theme; text: "停止"; iconName: "bell-off"; danger: true
                implicitHeight: 30; customRadius: 8
                accessibleName: "停止本次提醒"
                onClicked: root.stopped()
            }
        }
    }
}
