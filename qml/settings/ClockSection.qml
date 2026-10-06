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
            description: "以网络时间为倒计时与走时反解的基准；SNTP 失败时回退 HTTP 授时。"
            checked: app.settings.enableNtpSync
            onToggled: app.settings.enableNtpSync = checked
        }
        Column {
            width: parent.width; spacing: 6
            // 状态行：当前校时结果。关闭时说明后果，而不是只显示「已关闭」。
            Rectangle {
                width: parent.width; height: 3; radius: 2
                color: app.clockInfo.state === "synced" ? root.theme.clockSynced : root.theme.outlineVariant
                visible: app.settings.enableNtpSync
            }
            Text {
                width: parent.width
                text: "校时状态 · " + app.clockInfo.state + "\n" + app.clockInfo.detail
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
                placeholderText: "如 ntp.aliyun.com（留空用默认）"
                Accessible.name: "自定义 NTP 服务器"
            }
            GlassButton {
                theme: root.theme; text: "应用并重新校时"; iconName: "check"
                enabled: ntpServerField.text.trim() !== app.settings.customNtpServer
                onClicked: { app.settings.customNtpServer = ntpServerField.text.trim(); app.refreshClock(); }
            }
            Text {
                width: parent.width
                text: "留空时按内置顺序尝试：ntp.aliyun.com / ntp1.aliyun.com / ntp.tencent.com / pool.ntp.org / time.apple.com。自定义主机将优先尝试。"
                color: root.theme.outline; font.pixelSize: 12; wrapMode: Text.Wrap; lineHeight: 1.4
            }
        }
    }
}
