import QtQuick
import QtQuick.Layouts

Item {
    id: root
    property Theme theme: Theme {}
    property bool isSuccess: true
    property string message: ""
    property bool showing: false
    function show(msg, success = true) { message = msg; isSuccess = success; showing = true; hideTimer.restart(); }
    anchors.horizontalCenter: parent.horizontalCenter
    anchors.bottom: parent.bottom
    anchors.bottomMargin: 28
    width: Math.min(parent.width - 32, 520)
    height: card.height
    z: 9999
    visible: opacity > 0
    opacity: showing ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: root.theme.motionDuration } }
    Timer { id: hideTimer; interval: 2600; onTriggered: root.showing = false }
    GlassCard {
        id: card
        theme: root.theme
        width: parent.width
        height: row.implicitHeight + 28
        radius: 14
        border.color: root.isSuccess ? root.theme.accentBorder : root.theme.severity("CRITICAL")
        RowLayout {
            id: row
            x: 16; y: 14; width: parent.width - 32
            spacing: 10
            AppIcon { name: root.isSuccess ? "circle-check" : "circle-alert"; color: root.isSuccess ? root.theme.accent : root.theme.severity("CRITICAL"); size: 18 }
            Text { Layout.fillWidth: true; text: root.message; color: root.theme.textPrimary; font.pixelSize: 13; wrapMode: Text.Wrap }
        }
    }
}
