import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 「震源推算」分区：本地震源推算（Beta）的全部可调参数。
//
// 这一页的取向是「让人知道它的证据来自哪里」：不承诺精度，而是把触发阈值、发布门槛、
// 负证据与保留时长摊开，并说明每一项会怎样改变结果。地图叠加开关单独一组，
// 因为它只影响显示，不影响推算本身。
//
// **按区域分卡**：中国 / 日本 / 韩国 / 台湾 各一张卡，各存一套参数。这不是重复排布 ——
// 四片台网的密度差两个数量级（NIED 上千站 vs 台湾目前只有个别 Wolfx 站），一个全局阈值
// 必然对其中至少三片是错的。总开关、保留时长与地图标记是纯展示行为，仍只有全局一份。
Column {
    id: root
    objectName: "settingsPanel-sourceEstimation"
    property Theme theme: Theme {}
    width: parent.width
    spacing: 20

    /// 与 C++ 的 sourceRegions() 同序；label / family 只用于展示，参数一律走 settings 的
    /// 区域重载（键为 `sourceEstimation.<id>.<field>`），QML 不自己拼键。
    readonly property var regionOrder: ["china", "japan", "korea", "taiwan"]

    SettingsSection {
        width: parent.width; theme: root.theme; title: "推算开关"; iconName: "locate-fixed"

        GlassSwitch {
            objectName: "sourceEstimationSwitch"
            width: parent.width; theme: root.theme
            text: "本地震源推算（Beta）"
            description: "用测站强度反推震中、震级与深度；结果只在本机显示。"
            checked: app.settings.sourceEstimationEnabled
            onToggled: app.settings.sourceEstimationEnabled = checked
        }

        GlassSwitch {
            objectName: "showEstimatedEpicenterSwitch"
            width: parent.width; theme: root.theme
            text: "在地图上标出推定震中"
            checked: app.settings.showEstimatedEpicenter
            enabled: app.settings.sourceEstimationEnabled
            onToggled: app.settings.showEstimatedEpicenter = checked
        }

        Text {
            width: parent.width
            visible: app.settings.sourceEstimationEnabled
                     && !app.settings.hasAnySourceEstimationRegion()
            text: "请先在下方打开至少一个区域。"
            wrapMode: Text.Wrap; lineHeight: 1.4
            color: root.theme.severity("WATCH"); font.pixelSize: 12
        }
    }

    // 逐区域参数卡。
    Repeater {
        model: root.regionOrder
        delegate: SettingsSection {
            id: regionCard
            required property string modelData
            width: root.width; theme: root.theme
            title: app.settings.sourceEstimationRegionLabel(regionCard.modelData) + "区域"
            iconName: "globe"
            readonly property bool active: app.settings.sourceEstimationEnabled
                                           && app.settings.isSourceEstimationRegionEnabled(
                                                  regionCard.modelData)

            // 逐区开关：未启用时整区不参与推算（不记账、不算、不发布）。
            GlassSwitch {
                id: regionToggle
                objectName: regionCard.modelData + "RegionSwitch"
                width: parent.width; theme: root.theme
                text: "参与推算"
                description: "只开你关心的台网。"
                // 用 Binding 而不是 `checked:` 字面绑定：用户点按会写入 checked、
                // 从而**断开**字面绑定（QML 破坏性绑定语义），之后设置再变开关也不动，
                // UI 与真实状态就脱节了。
                //
                // value 里必须依赖一个**真实属性**：`isSourceEstimationRegionEnabled()` 是
                // Q_INVOKABLE 函数调用，QML 不跟踪它的依赖，设置变了也不会重算。
                // 所以依赖 `sourceEstimationRegionSignature`（字符串），再用它判断本区。
                checked: false
                Binding {
                    target: regionToggle
                    property: "checked"
                    value: app.settings.sourceEstimationRegionSignature.split(",")
                               .indexOf(regionCard.modelData) >= 0
                    restoreMode: Binding.RestoreBindingOrValue
                }
                enabled: app.settings.sourceEstimationEnabled
                onToggled: app.settings.setSourceEstimationRegionEnabled(regionCard.modelData,
                                                                         checked)
            }

            SettingSlider {
                width: parent.width; theme: root.theme
                title: "触发烈度"
                valueText: Number(app.settings.sourceEstimationTriggerFor(regionCard.modelData))
                    .toFixed(1) + " 度"
                description: "达到该烈度即开始推算。"
                from: 1.0; to: 6.0; stepSize: 0.5
                value: app.settings.sourceEstimationTriggerFor(regionCard.modelData)
                active: regionCard.active
                onMoved: value => app.settings.setSourceEstimationTrigger(regionCard.modelData,
                                                                          value)
            }

            SettingSlider {
                width: parent.width; theme: root.theme
                title: "最少触发测站"
                valueText: app.settings.sourceEstimationMinStationsFor(regionCard.modelData) + " 站"
                description: "低于该站数只显示收集状态。"
                from: 1; to: 16; stepSize: 1
                value: app.settings.sourceEstimationMinStationsFor(regionCard.modelData)
                active: regionCard.active
                onMoved: value => app.settings.setSourceEstimationMinStations(regionCard.modelData,
                                                                              Math.round(value))
            }

            SettingSlider {
                width: parent.width; theme: root.theme
                title: "触发区合并距离"
                valueText: app.settings.sourceEstimationClusterRadiusFor(regionCard.modelData) + " km"
                description: "相距超过该距离的触发区视为两场地震。"
                from: 100; to: 1500; stepSize: 50
                value: app.settings.sourceEstimationClusterRadiusFor(regionCard.modelData)
                active: regionCard.active
                onMoved: value => app.settings.setSourceEstimationClusterRadius(regionCard.modelData,
                                                                                Math.round(value))
            }

            GlassSwitch {
                objectName: regionCard.modelData + "QuietEvidenceSwitch"
                width: parent.width; theme: root.theme
                text: "使用未触发测站作为反证"
                description: "用尚未触发的邻近测站约束结果。"
                checked: app.settings.sourceEstimationQuietEvidenceFor(regionCard.modelData)
                enabled: regionCard.active
                onToggled: app.settings.setSourceEstimationQuietEvidence(regionCard.modelData,
                                                                         checked)
            }

            GlassButton {
                objectName: regionCard.modelData + "ResetButton"
                theme: root.theme; flat: true; iconName: "refresh-cw"
                text: "恢复本区默认参数"
                onClicked: app.settings.resetSourceEstimationRegion(regionCard.modelData)
            }
        }
    }

    SettingsSection {
        width: parent.width; theme: root.theme; title: "结果与保持"; iconName: "activity"

        SettingSlider {
            width: parent.width; theme: root.theme
            title: "结果保留时长"
            valueText: app.settings.sourceEstimationHoldSeconds <= 0
                ? "立即消失"
                : app.settings.sourceEstimationHoldSeconds + " 秒"
            description: "证据消失后结果在地图上保留的时间；0 表示立即清空。"
            from: 0; to: 300; stepSize: 10
            value: app.settings.sourceEstimationHoldSeconds
            active: app.settings.sourceEstimationEnabled
            onMoved: value => app.settings.sourceEstimationHoldSeconds = Math.round(value)
        }
    }
}
