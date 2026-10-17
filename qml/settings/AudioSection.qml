import QtQuick

// 「声音」分区：总开关、静音、音量，以及播报内容分轨。
// 生效链条：地震预警 → 警报音效 → 音量；播报分轨只裁剪内容，不改变告警判定。
Column {
    id: root
    objectName: "settingsPanel-audio"
    property Theme theme: Theme {}
    width: parent.width
    spacing: 20

    readonly property bool alertsOn: app.settings.enableWarnings
    readonly property bool audible: alertsOn && !app.settings.isMuted && app.settings.enableSoundAlert

    SettingsSection {
        width: parent.width; theme: root.theme; title: "警报声音"; iconName: "volume-2"

        GlassSwitch {
            objectName: "soundAlertSwitch"
            width: parent.width; theme: root.theme
            text: "警报音效"
            description: !root.alertsOn ? "「地震预警」已关闭，开启本项也不会出声。"
                : app.settings.isMuted ? "「全局静音」已开启，开启本项也不会出声。"
                : "播放预警音效、播报与倒计时提示音。"
            enabled: root.alertsOn && !app.settings.isMuted
            checked: app.settings.enableSoundAlert
            onToggled: app.settings.enableSoundAlert = checked
        }
        GlassSwitch {
            objectName: "muteSwitch"
            width: parent.width; theme: root.theme; text: "全局静音"
            description: "停止声音，保留视觉提醒与系统通知。"
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
    }

    SettingsSection {
        width: parent.width; theme: root.theme; title: "播报内容"; iconName: "list"
        Text {
            width: parent.width
            text: "只裁剪警报声音的内容；关闭某一项不会影响其余播报，也不会影响视觉预警。"
            wrapMode: Text.Wrap; lineHeight: 1.4
            color: root.theme.outline; font.pixelSize: 12
        }
        GlassSwitch {
            objectName: "announceEventSwitch"
            width: parent.width; theme: root.theme
            text: "事件语音"
            description: "首发与终报的语音播报（如「紧急地震速报」）。"
            enabled: root.audible
            checked: app.settings.announceEvent
            onToggled: app.settings.announceEvent = checked
        }
        GlassSwitch {
            objectName: "announceUpdateSwitch"
            width: parent.width; theme: root.theme
            text: "更新报"
            description: "后续修正报的语音提示；关闭后只保留首发与终报。"
            enabled: root.audible
            checked: app.settings.announceUpdate
            onToggled: app.settings.announceUpdate = checked
        }
        GlassSwitch {
            objectName: "announceCountdownSwitch"
            width: parent.width; theme: root.theme
            text: "倒计时与抵达提示"
            description: "每秒倒计时、抵达提示与震中播报；关闭后仍显示屏幕倒计时。"
            enabled: root.audible
            checked: app.settings.announceCountdown
            onToggled: app.settings.announceCountdown = checked
        }
        GlassSwitch {
            objectName: "alarmToneSwitch"
            width: parent.width; theme: root.theme
            text: "警报音"
            description: "高等级警报的警示音与 10 秒内的强震动提示音。"
            enabled: root.audible
            checked: app.settings.enableAlarmTone
            onToggled: app.settings.enableAlarmTone = checked
        }
    }
}
