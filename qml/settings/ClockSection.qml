import QtQuick

// 「时间校准」分区：SNTP 开关、当前校时状态、自定义服务器。
Column {
    id: root
    objectName: "settingsPanel-clock"
    property Theme theme: Theme {}
    width: parent.width
    spacing: 20

    SettingsSection {
        width: parent.width; theme: root.theme; title: "时间校准"; iconName: "clock"
        GlassSwitch {
            objectName: "ntpSwitch"
            width: parent.width; theme: root.theme; text: "网络校时 (SNTP)"
            checked: app.settings.enableNtpSync
            onToggled: app.settings.enableNtpSync = checked
        }
        Column {
            width: parent.width; spacing: 6
            // 状态行：当前校时结果。`state` 是本地化文案（已校准 / 校准过期 / 本地时钟），
            // 配色必须看布尔 `synced`，否则对比一个永远不成立的字符串，色条恒为灰。
            Rectangle {
                width: parent.width; height: 3; radius: 2
                color: app.clockInfo.synced ? root.theme.clockSynced : root.theme.outlineVariant
                visible: app.settings.enableNtpSync
            }
            Text {
                width: parent.width
                // 已校准时只报状态；未校准才补充原因（等待授时 / 使用本地时钟）。
                text: "校时状态 · " + app.clockInfo.state
                      + (app.clockInfo.synced ? "" : "\n" + app.clockInfo.detail)
                color: root.theme.outline; font.pixelSize: 12; wrapMode: Text.Wrap; lineHeight: 1.5
            }
        }
        Column {
            width: parent.width; spacing: 6
            enabled: app.settings.enableNtpSync
            opacity: enabled ? 1 : 0.55
            Text {
                text: "自定义 NTP 服务器"
                color: root.theme.textPrimary; font.pixelSize: 14
            }
            GlassTextField {
                id: ntpServerField; objectName: "customNtpField"
                width: parent.width; theme: root.theme
                text: app.settings.customNtpServer
                placeholderText: "留空使用默认服务器"
                Accessible.name: "自定义 NTP 服务器"
            }
            GlassButton {
                theme: root.theme; text: "应用并重新校时"; iconName: "check"
                enabled: ntpServerField.text.trim() !== app.settings.customNtpServer
                onClicked: { app.settings.customNtpServer = ntpServerField.text.trim(); app.refreshClock(); }
            }
        }
    }
}
