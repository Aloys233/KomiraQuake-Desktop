import QtQuick

// 「预警策略」分区：提醒条件。
// 依赖关系显式化：上级开关关闭时，下级控件置灰（enabled: false），
// 而不是留着可点却不生效的控件让人误以为设置已生效。
Column {
    id: root
    objectName: "settingsPanel-warning"
    property Theme theme: Theme {}
    width: parent.width
    spacing: 20

    SettingsSection {
        width: parent.width; theme: root.theme; title: "提醒条件"; iconName: "shield"
        GlassSwitch {
            objectName: "warningsSwitch"
            width: parent.width; theme: root.theme; text: "地震预警"
            description: "总开关。开启后仅按本地烈度过滤决定是否提醒；关闭时地震事件只展示，不产生声音、震动或全屏预警。"
            checked: app.settings.enableWarnings
            onToggled: app.settings.enableWarnings = checked
        }
        Column {
            width: parent.width; spacing: 10
            // Column 没有 active 属性，依赖禁用直接用内置的 enabled（会向下传递）。
            enabled: app.settings.enableWarnings
            opacity: enabled ? 1 : 0.55
            Text {
                text: "烈度标准"
                color: root.theme.textPrimary
                font.pixelSize: 14
            }
            GlassComboBox {
                width: parent.width; theme: root.theme
                model: ["中国烈度 · CSIS", "日本震度 · JMA"]
                Accessible.name: "烈度标准"
                currentIndex: app.settings.intensityStandard
                onActivated: index => app.settings.intensityStandard = index
            }
        }
        SettingSlider {
            width: parent.width; theme: root.theme
            title: "本地烈度过滤"
            valueText: app.settings.localIntensityFilter <= 0
                ? "关闭"
                : app.settings.localIntensityFilter.toFixed(1) + " 度"
            description: "仅当本地预估烈度达到该值及以上时提醒（按上方所选烈度标准的显示档位比较）；设为 0 表示不作筛选。"
            from: 0; to: 8; stepSize: 0.5
            value: app.settings.localIntensityFilter
            active: app.settings.enableWarnings
            onMoved: value => app.settings.localIntensityFilter = value
        }
    }
}
