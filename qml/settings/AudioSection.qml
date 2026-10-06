import QtQuick

// 「警报提醒」分区。
// 生效链条：地震预警 → 警报音效 → 音量。任一环关闭，下游全部置灰，
// 并在说明里写明「被谁挡住」，避免用户反复试探哪个开关没生效。
// 系统通知与窗口置顶属于视觉提醒，只受「地震预警」总开关约束，不受全局静音影响。
Column {
    id: root
    objectName: "settingsPanel-audio"
    property Theme theme: Theme {}
    width: parent.width
    spacing: 20

    readonly property bool alertsOn: app.settings.enableWarnings
    readonly property bool audible: alertsOn && !app.settings.isMuted && app.settings.enableSoundAlert

    SettingsSection {
        width: parent.width; theme: root.theme; title: "警报提醒"; iconName: "volume-2"

        GlassSwitch {
            objectName: "soundAlertSwitch"
            width: parent.width; theme: root.theme
            text: "警报音效"
            description: !root.alertsOn ? "「地震预警」已关闭，开启本项也不会出声。"
                : app.settings.isMuted ? "「全局静音」已开启，开启本项也不会出声。"
                : "播放预警音效与倒计时提示音。"
            enabled: root.alertsOn && !app.settings.isMuted
            checked: app.settings.enableSoundAlert
            onToggled: app.settings.enableSoundAlert = checked
        }
        GlassSwitch {
            objectName: "muteSwitch"
            width: parent.width; theme: root.theme; text: "全局静音"
            description: "停止声音，保留视觉提醒。"
            checked: app.settings.isMuted
            onToggled: app.settings.isMuted = checked
        }
        SettingSlider {
            width: parent.width; theme: root.theme
            title: "警报音量"
            valueText: Math.round(app.settings.alertVolume * 100) + "%"
            from: 0; to: 1; stepSize: 0.05
            value: app.settings.alertVolume
            active: root.audible
            onMoved: value => app.settings.alertVolume = value
        }
        // 全局静音只停声音（见上），系统通知与置顶不受它影响，仍需单独关闭。
        GlassSwitch {
            objectName: "desktopNotificationSwitch"
            width: parent.width; theme: root.theme
            text: "系统通知"
            description: !root.alertsOn ? "「地震预警」已关闭，开启本项也不会提醒。"
                : "预警时弹出系统托盘通知气泡，点击可打开主窗口。"
            enabled: root.alertsOn
            checked: app.settings.enableDesktopNotification
            onToggled: app.settings.enableDesktopNotification = checked
        }
        GlassSwitch {
            objectName: "windowRaiseSwitch"
            width: parent.width; theme: root.theme
            text: "预警时弹出主窗口"
            description: !root.alertsOn ? "「地震预警」已关闭，开启本项也不会弹出窗口。"
                : "预警时把主窗口拉到前台。会抢占当前窗口焦点，打游戏或工作时建议保持关闭。"
            enabled: root.alertsOn
            checked: app.settings.enableWindowRaise
            onToggled: app.settings.enableWindowRaise = checked
        }
    }
}
