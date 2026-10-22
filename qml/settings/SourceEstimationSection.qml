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
//
// **默认只显示日本区**：只有它的算法（P/S 到时反演，§17.6）在真实台网上标定过；
// 中国 / 韩国 / 台湾 的强度场路径与默认参数仍在标定中，只对开发者模式开放
// （判据在 `SettingsStore::isSourceEstimationRegionAvailable`，服务层用同一份判据）。
//
// **绑定纪律**：所有逐区数值一律读 `app.settings.sourceEstimationRegionParams` 这个
// QVariantMap 快照，**不要**直接调 `sourceEstimationTriggerFor(region)` 之类的
// Q_INVOKABLE。QML 只跟踪属性读取、不跟踪函数调用 —— 绑在函数调用上的绑定在函数返回值
// 变化时不会重算，表现就是「开关点开了，下面滑块还是灰的；拖动滑块数值文本不变」。
Column {
    id: root
    objectName: "settingsPanel-sourceEstimation"
    property Theme theme: Theme {}
    width: parent.width
    spacing: 20

    /// 四个区域的参数与状态快照（含 label / trigger / minStations / quietEvidence /
    /// clusterRadius / enabled / available）。见 SettingsStore 的同名 Q_PROPERTY。
    readonly property var regionParams: app.settings.sourceEstimationRegionParams
    /// 与 C++ 的 sourceRegions() 同序。参数一律走上面的快照，QML 不自己拼键。
    readonly property var regionOrder: ["china", "japan", "korea", "taiwan"]

    SettingsSection {
        width: parent.width; theme: root.theme; title: "推算开关"; iconName: "locate-fixed"

        GlassSwitch {
            id: masterToggle
            objectName: "sourceEstimationSwitch"
            width: parent.width; theme: root.theme
            text: "本地震源推算（Beta）"
            description: "用测站强度反推震中、震级与深度；结果只在本机显示。"
            checked: false
            // 用 Binding 而不是 `checked:` 字面绑定：用户点按会写入 checked、
            // 从而**断开**字面绑定（QML 破坏性绑定语义），之后设置再变开关也不动。
            Binding {
                target: masterToggle
                property: "checked"
                value: app.settings.sourceEstimationEnabled
                restoreMode: Binding.RestoreBindingOrValue
            }
            onToggled: app.settings.sourceEstimationEnabled = checked
        }

        GlassSwitch {
            id: epicenterToggle
            objectName: "showEstimatedEpicenterSwitch"
            width: parent.width; theme: root.theme
            text: "在地图上标出推定震中"
            checked: false
            Binding {
                target: epicenterToggle
                property: "checked"
                value: app.settings.showEstimatedEpicenter
                restoreMode: Binding.RestoreBindingOrValue
            }
            enabled: app.settings.sourceEstimationEnabled
            onToggled: app.settings.showEstimatedEpicenter = checked
        }

        GlassSwitch {
            id: labelToggle
            objectName: "showEstimateLabelSwitch"
            width: parent.width; theme: root.theme
            text: "显示推定震中标签"
            description: "在标记旁常驻「区域 · 精度 · 报数」与震级深度，重叠时自动换边。"
            checked: false
            Binding {
                target: labelToggle
                property: "checked"
                value: app.settings.showEstimateLabel
                restoreMode: Binding.RestoreBindingOrValue
            }
            // 标记本身关掉时标签无处可挂：跟着灰掉，避免「开关开着却什么也看不见」。
            enabled: app.settings.sourceEstimationEnabled
                     && app.settings.showEstimatedEpicenter
            onToggled: app.settings.showEstimateLabel = checked
        }

        Text {
            width: parent.width
            // 依赖 `sourceEstimationRegionActive` 这个 Q_PROPERTY，而不是
            // `hasAnySourceEstimationRegion()`（Q_INVOKABLE，绑上去不会重算）。
            visible: app.settings.sourceEstimationEnabled
                     && !app.settings.sourceEstimationRegionActive
            text: "请先在下方打开至少一个区域。"
            wrapMode: Text.Wrap; lineHeight: 1.4
            color: root.theme.severity("WATCH"); font.pixelSize: 12
        }

        Text {
            width: parent.width
            visible: !app.settings.developerMode
            text: "中国 / 韩国 / 台湾区域仍在标定，开启开发者模式后可配置。"
            wrapMode: Text.Wrap; lineHeight: 1.4
            color: root.theme.outline; font.pixelSize: 12
        }
    }

    // 逐区域参数卡。
    Repeater {
        model: root.regionOrder
        delegate: SettingsSection {
            id: regionCard
            required property string modelData
            objectName: regionCard.modelData + "RegionCard"
            /// 本区快照；缺项时给空对象，绑定取到的就是 undefined 而不是报错。
            readonly property var info: root.regionParams
                                        && root.regionParams[regionCard.modelData]
                                        ? root.regionParams[regionCard.modelData] : ({})
            /// 本区是否开放（非开发者模式下只有日本区）。未开放整卡不显示。
            readonly property bool available: info.available === true
            /// 本区是否正在生效（总开关 × 本区开关；可用性已并入快照的 enabled）。
            readonly property bool active: app.settings.sourceEstimationEnabled
                                           && info.enabled === true
            width: root.width; theme: root.theme
            title: (info.label !== undefined ? info.label : "") + "区域"
            iconName: "globe"
            visible: available
            height: available ? implicitHeight : 0

            // 逐区开关：未启用时整区不参与推算（不记账、不算、不发布）。
            GlassSwitch {
                id: regionToggle
                objectName: regionCard.modelData + "RegionSwitch"
                width: parent.width; theme: root.theme
                text: "参与推算"
                description: "只开你关心的台网。"
                // 见文件头「绑定纪律」：这里读快照（Q_PROPERTY 依赖），
                // 不调 isSourceEstimationRegionEnabled()（Q_INVOKABLE，不产生依赖）。
                checked: false
                Binding {
                    target: regionToggle
                    property: "checked"
                    value: regionCard.info.enabled === true
                    restoreMode: Binding.RestoreBindingOrValue
                }
                enabled: app.settings.sourceEstimationEnabled
                onToggled: app.settings.setSourceEstimationRegionEnabled(regionCard.modelData,
                                                                         checked)
            }

            SettingSlider {
                objectName: regionCard.modelData + "TriggerSlider"
                width: parent.width; theme: root.theme
                title: "触发烈度"
                valueText: Number(regionCard.info.trigger).toFixed(1) + " 度"
                description: "达到该烈度即开始推算。"
                from: 1.0; to: 6.0; stepSize: 0.5
                value: regionCard.info.trigger
                active: regionCard.active
                onMoved: value => app.settings.setSourceEstimationTrigger(regionCard.modelData,
                                                                          value)
            }

            SettingSlider {
                objectName: regionCard.modelData + "MinStationsSlider"
                width: parent.width; theme: root.theme
                title: "最少触发测站"
                valueText: regionCard.info.minStations + " 站"
                description: "低于该站数只显示收集状态。"
                from: 1; to: 16; stepSize: 1
                value: regionCard.info.minStations
                active: regionCard.active
                onMoved: value => app.settings.setSourceEstimationMinStations(regionCard.modelData,
                                                                              Math.round(value))
            }

            SettingSlider {
                objectName: regionCard.modelData + "ClusterRadiusSlider"
                width: parent.width; theme: root.theme
                title: "触发区合并距离"
                valueText: regionCard.info.clusterRadius + " km"
                description: "相距超过该距离的触发区视为两场地震。"
                from: 100; to: 1500; stepSize: 50
                value: regionCard.info.clusterRadius
                active: regionCard.active
                onMoved: value => app.settings.setSourceEstimationClusterRadius(regionCard.modelData,
                                                                                Math.round(value))
            }

            GlassSwitch {
                id: quietToggle
                objectName: regionCard.modelData + "QuietEvidenceSwitch"
                width: parent.width; theme: root.theme
                text: "使用未触发测站作为反证"
                description: "用尚未触发的邻近测站约束结果。"
                checked: false
                Binding {
                    target: quietToggle
                    property: "checked"
                    value: regionCard.info.quietEvidence === true
                    restoreMode: Binding.RestoreBindingOrValue
                }
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
            objectName: "holdSecondsSlider"
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
