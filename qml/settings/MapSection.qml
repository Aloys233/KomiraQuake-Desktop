import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 「地图与图层」分区：底图与叠加层。把原先混在「界面与地图」里的地图项独立出来，
// 图层开关按实际渲染内容逐项列出，避免出现「开了却不知道开了什么」。
Column {
    id: root
    objectName: "settingsPanel-map"
    property Theme theme: Theme {}
    width: parent.width
    spacing: 20

    SettingsSection {
        width: parent.width; theme: root.theme; title: "底图"; iconName: "map"
        Column {
            width: parent.width; spacing: 10
            GlassComboBox {
                objectName: "basemapCombo"
                width: parent.width; theme: root.theme; Accessible.name: "底图"
                model: ["高德标准 · GCJ-02", "Petal · GCJ-02", "OpenStreetMap · WGS-84", "矢量 · WGS-84", "自定义底图"]
                readonly property var ids: ["amap_vector", "petal", "osm", "vector", "custom"]
                // 未知 id（旧版残留，如已下线的卫星图）回落到第一项而不是空白。
                currentIndex: Math.max(0, ids.indexOf(app.settings.basemapId))
                onActivated: index => app.settings.basemapId = ids[index]
            }
        }
        Column {
            width: parent.width; spacing: 10
            visible: app.settings.basemapId === "custom"
            GlassTextField {
                width: parent.width; theme: root.theme
                placeholderText: "瓦片 URL 模板：{z}/{x}/{y}"
                Accessible.name: "自定义瓦片 URL"
                text: app.settings.customBasemapUrl
                onEditingFinished: app.settings.customBasemapUrl = text
            }
            GlassComboBox {
                width: parent.width; theme: root.theme; Accessible.name: "自定义底图坐标系"
                model: ["WGS-84", "GCJ-02"]
                currentIndex: app.settings.customBasemapDatum
                onActivated: index => app.settings.customBasemapDatum = index
            }
        }
    }

    SettingsSection {
        width: parent.width; theme: root.theme; title: "叠加图层"; iconName: "layers"
        // 栅格底图上叠加矢量层：选纯矢量底图时本就画矢量，无需再叠一层。
        GlassSwitch {
            objectName: "vectorOverlaySwitch"
            width: parent.width; theme: root.theme
            visible: app.settings.basemapId !== "vector"
            text: "叠加矢量层"
            description: "叠加省界与区县边界。"
            checked: app.settings.vectorOverlay
            onToggled: app.settings.vectorOverlay = checked
        }
        GlassSwitch {
            objectName: "provinceBorderSwitch"
            width: parent.width; theme: root.theme
            text: "省界描边"
            checked: app.settings.showProvinceBorders
            enabled: app.settings.basemapId === "vector" || app.settings.vectorOverlay
            onToggled: app.settings.showProvinceBorders = checked
        }
        GlassSwitch {
            objectName: "stationLayerSwitch"
            width: parent.width; theme: root.theme
            text: "测站强度图层"
            description: "在地图上显示实时测站强度。"
            checked: app.settings.showStationLayer
            onToggled: app.settings.showStationLayer = checked
        }
        GlassSwitch {
            objectName: "wavefrontSwitch"
            width: parent.width; theme: root.theme
            text: "P/S 波前圆"
            description: "显示地震波扩散范围。"
            checked: app.settings.showWavefront
            onToggled: app.settings.showWavefront = checked
        }

        // ---- 波前何时消失 -------------------------------------------------------
        // 三种口径共用同一条渐隐曲线（前 95% 不透明、最后 5% 归零），差别只在
        // 「走到哪算消失」。三个数值各存一份、互不改写：切回上次用过的口径时，
        // 那一项调好的数值还在（见 SettingsStore::waveFade*）。
        Column {
            width: parent.width; spacing: 10
            enabled: app.settings.showWavefront
            Text {
                text: "波前消失条件"
                color: root.theme.textPrimary
                font.pixelSize: 14
            }
            GlassComboBox {
                id: fadeModeCombo
                objectName: "waveFadeModeCombo"
                width: parent.width; theme: root.theme
                Accessible.name: "波前消失条件"
                model: ["按烈度（可感边界）", "按距离", "按时间"]
                readonly property var ids: ["intensity", "distance", "time"]
                // 用 Binding 而不是字面绑定：用户选择会写入 currentIndex，字面绑定会被
                // 断开，此后「恢复默认」改了模式下拉框也不动（与 SettingSlider 同一个坑）。
                Binding {
                    target: fadeModeCombo
                    property: "currentIndex"
                    value: Math.max(0, fadeModeCombo.ids.indexOf(app.settings.waveFadeMode))
                    restoreMode: Binding.RestoreBindingOrValue
                }
                onActivated: index => app.settings.waveFadeMode = ids[index]
            }
            Text {
                width: parent.width
                text: "三种方式的淡出手感一致，只是边界不同；数值分别保存，切回来不用重调。"
                wrapMode: Text.Wrap; lineHeight: 1.4
                color: root.theme.outline; font.pixelSize: 12
            }
        }

        SettingSlider {
            objectName: "waveFadeIntensitySlider"
            width: parent.width; theme: root.theme
            title: "消失烈度"
            valueText: app.settings.waveFadeIntensity.toFixed(1) + " 度"
            description: "波前扫到该烈度处开始淡出，再走 5% 归零。1.0 = 可感下限。"
            from: 0.5; to: 6.0; stepSize: 0.5
            value: app.settings.waveFadeIntensity
            active: app.settings.showWavefront && app.settings.waveFadeMode === "intensity"
            onMoved: value => app.settings.waveFadeIntensity = value
        }

        SettingSlider {
            objectName: "waveFadeDistanceSlider"
            width: parent.width; theme: root.theme
            title: "消失距离"
            valueText: app.settings.waveFadeDistanceKm + " km"
            description: "波前半径到达该距离后淡出，与震级无关。"
            from: 50; to: 5000; stepSize: 50
            value: app.settings.waveFadeDistanceKm
            active: app.settings.showWavefront && app.settings.waveFadeMode === "distance"
            onMoved: value => app.settings.waveFadeDistanceKm = Math.round(value)
        }

        SettingSlider {
            objectName: "waveFadeSecondsSlider"
            width: parent.width; theme: root.theme
            title: "消失时间"
            valueText: app.settings.waveFadeSeconds + " 秒"
            description: "发震后经过该时长，P 与 S 波前同时淡出。不跟随半径。"
            from: 30; to: 3600; stepSize: 30
            value: app.settings.waveFadeSeconds
            active: app.settings.showWavefront && app.settings.waveFadeMode === "time"
            onMoved: value => app.settings.waveFadeSeconds = Math.round(value)
        }
    }

    // ---- 历史震中 ------------------------------------------------------------
    // 地图在「最近一次事件」的红叉之外，还能把历史目录里的地震逐个画出来。
    // 两种口径（个数 / 时长）**各存一份、互不改写**：切回上次用过的口径时数值还在，
    // 与波前消失条件同一套约定。默认「最近 1 个」= 外观与改动前完全一致
    // （那一个就是当前事件本身，历史层不会重复画）。
    SettingsSection {
        width: parent.width; theme: root.theme; title: "历史震中"; iconName: "map-pin"
        GlassSwitch {
            objectName: "historyEpicenterSwitch"
            width: parent.width; theme: root.theme
            text: "在地图上显示历史震中"
            description: "除当前事件外，把最近的地震也标在地图上。默认只显示最近一个。"
            checked: app.settings.showHistoryEpicenter
            onToggled: app.settings.showHistoryEpicenter = checked
        }

        Column {
            width: parent.width; spacing: 10
            enabled: app.settings.showHistoryEpicenter
            Text {
                text: "显示方式"
                color: root.theme.textPrimary
                font.pixelSize: 14
            }
            GlassComboBox {
                id: historyModeCombo
                objectName: "historyEpicenterModeCombo"
                width: parent.width; theme: root.theme
                Accessible.name: "历史震中显示方式"
                model: ["按数量", "按时间"]
                readonly property var ids: ["count", "time"]
                // 与波前口径下拉同理：用 Binding 而不是字面绑定，用户选择写 currentIndex
                // 会断开字面绑定，「恢复默认」就推不动它了。
                Binding {
                    target: historyModeCombo
                    property: "currentIndex"
                    value: Math.max(0, historyModeCombo.ids.indexOf(app.settings.historyEpicenterMode))
                    restoreMode: Binding.RestoreBindingOrValue
                }
                onActivated: index => app.settings.historyEpicenterMode = ids[index]
            }
        }

        SettingSlider {
            objectName: "historyEpicenterCountSlider"
            width: parent.width; theme: root.theme
            title: "显示个数"
            valueText: app.settings.historyEpicenterCount + " 个"
            description: "包含当前事件在内，地图上一共几个震中。1 = 与改动前一样。"
            from: 1; to: 50; stepSize: 1
            value: app.settings.historyEpicenterCount
            active: app.settings.showHistoryEpicenter && app.settings.historyEpicenterMode === "count"
            onMoved: value => app.settings.historyEpicenterCount = Math.round(value)
        }

        SettingSlider {
            objectName: "historyEpicenterHoursSlider"
            width: parent.width; theme: root.theme
            title: "显示时长"
            valueText: app.settings.historyEpicenterHours + " 小时"
            description: "该时长内发生过的地震都标出来，条数不设上限。"
            from: 1; to: 168; stepSize: 1
            value: app.settings.historyEpicenterHours
            active: app.settings.showHistoryEpicenter && app.settings.historyEpicenterMode === "time"
            onMoved: value => app.settings.historyEpicenterHours = Math.round(value)
        }
    }

    SettingsSection {
        width: parent.width; theme: root.theme; title: "测站卡片"; iconName: "activity"
        // 右上角那组「各源当前最大值」卡的形态。两种形态显示的信息完全一样，
        // 差别只是占多大地盘：堆叠 = 只露前排 + 一张纸条边，悬停才摊开；
        // 平铺 = 每张都完整可读，但整排常年横着占掉地图右上角。
        GlassSwitch {
            objectName: "stackStationCardsSwitch"
            width: parent.width; theme: root.theme
            text: "测站卡片堆叠"
            description: "开启后右上角各数据源叠成一摞，鼠标移上去展开；关闭则始终平铺成一排。"
            checked: app.settings.stackStationCards
            onToggled: app.settings.stackStationCards = checked
        }
    }
}
