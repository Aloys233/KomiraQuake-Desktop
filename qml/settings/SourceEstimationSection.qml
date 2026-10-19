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
    /// 每个区域一句话说清「为什么它的默认值不一样」。
    readonly property var regionHints: ({
        china: "PancakesSeis / Wolfx SeisJS 的中国烈度台网，站间距大，默认 6 站、合并半径 250 km。",
        japan: "NIED Kmoni + S-Net 的計測震度台网，上千站；默认 5 站（对齐 kanameishi 的"
             + "反演下限），压噪声靠邻域联动，不是抬高站数。",
        korea: "KMA PEWS 的 MMI 台网，约 260 站；默认 5 站、合并半径 200 km（半岛尺度）。",
        taiwan: "CWA 台网尚未接入，目前只有 Wolfx SeisJS 的个别测站；默认门槛 1 站，"
              + "给的是「单站粗解」——卡片上的精度只会是 D。接入后把它调高即可。"
    })

    SettingsSection {
        width: parent.width; theme: root.theme; title: "推算开关"; iconName: "locate-fixed"

        GlassSwitch {
            objectName: "sourceEstimationSwitch"
            width: parent.width; theme: root.theme
            text: "本地震源推算（Beta）"
            description: "只用实时测站强度反推震中、震级与深度，不依赖任何预警报文。"
                         + "结果仅在本地展示，不产生提醒、不进入事件列表与历史记录。"
                         + "四个区域各自独立推算，互不覆盖。"
            checked: app.settings.sourceEstimationEnabled
            onToggled: app.settings.sourceEstimationEnabled = checked
        }

        GlassSwitch {
            objectName: "showEstimatedEpicenterSwitch"
            width: parent.width; theme: root.theme
            text: "在地图上标出推定震中"
            description: "每个出解的区域各画一个空心方块与不确定度圈，并在下方标注区域名；"
                         + "与官方震中的红叉区分。"
            checked: app.settings.showEstimatedEpicenter
            enabled: app.settings.sourceEstimationEnabled
            onToggled: app.settings.showEstimatedEpicenter = checked
        }

        Text {
            width: parent.width
            visible: app.settings.sourceEstimationEnabled
                     && !app.settings.hasAnySourceEstimationRegion()
            text: "总开关已打开，但还没有选择任何区域 —— 请在下方区域卡里打开「参与推算」。"
            wrapMode: Text.Wrap; lineHeight: 1.4
            color: root.theme.severity("WATCH"); font.pixelSize: 12
        }

        Text {
            width: parent.width
            text: "为什么叫 Beta：强度场只能约束「哪里更近」，缺少 P/S 到时与震相信息，"
                  + "因此深度只做离散候选（浅源优先），震级也会随波前扫过逐步修正。"
                  + "单侧覆盖（测站只在震中一侧）时位置偏差可达数十公里 —— 卡片右下角的"
                  + "「精度 S/A/B/C/D」就是质量等级，地图上的圈是水平不确定度；"
                  + "卡片不承诺公里级位置，也不把等级解释为概率。"
            wrapMode: Text.Wrap; lineHeight: 1.45
            color: root.theme.outline; font.pixelSize: 12
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
                description: "只开你关心的台网。未启用的区域完全停摆：不记账、不推算、"
                             + "不在卡片/地图上出现。默认全部关闭。"
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

            Text {
                width: parent.width
                text: root.regionHints[regionCard.modelData] || ""
                wrapMode: Text.Wrap; lineHeight: 1.4
                color: root.theme.outline; font.pixelSize: 12
            }

            SettingSlider {
                width: parent.width; theme: root.theme
                title: "触发烈度"
                valueText: Number(app.settings.sourceEstimationTriggerFor(regionCard.modelData))
                    .toFixed(1) + " 度"
                description: "测站烈度达到该值即视为已触发。调低会更早起算、更容易被常态噪声带动；"
                             + "调高则更稳，但会推迟首次给出震源的时间。"
                             + "这里填的是该区主导制式的刻度（中国 / 台湾为中国烈度、日本为計測震度、"
                             + "韩国为 MMI），不会跨制式比较。"
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
                description: "低于这个数量只显示「收集中」，不给震源。站数越少，解的自由度越大，"
                             + "位置越容易被个别测站的场地效应带偏。可设到 1 站：此时只给一个"
                             + "「触发站附近」的粗解（单站解，误差可达百公里级）。"
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
                description: "相距超过这个距离的两片触发区不会被当成同一场地震。"
                             + "调大能照顾稀疏台网（一场地震的触发站隔得较远），"
                             + "但两地同时发生地震时更容易被合成一个位于中间的、并不存在的震中；"
                             + "调小则相反。"
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
                description: "把邻近但尚未越过阈值的测站当作「预测值不能超过门槛」的约束。"
                             + "开启后单侧覆盖的位置偏差明显减小；关闭则只按已触发的站拟合。"
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
            description: "震动过后证据消散，结果在地图与卡片上继续保留的时间；设为 0 则证据一消失就清空。"
                         + "所有区域共用这一个时长。"
            from: 0; to: 300; stepSize: 10
            value: app.settings.sourceEstimationHoldSeconds
            active: app.settings.sourceEstimationEnabled
            onMoved: value => app.settings.sourceEstimationHoldSeconds = Math.round(value)
        }
    }
}
