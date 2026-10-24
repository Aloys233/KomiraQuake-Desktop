import QtQuick
import QtQuick.Layouts
import QtQuick.Effects

// 地震事件卡片。布局对齐 kanameishi 的列表项：
// 左侧烈度色块，右侧自上而下为「震中 / 发震时刻 / 震级·深度·距离」。无震级图标、无分割线。
// 整块内容在卡片内垂直居中；同行内的数字与说明文字按基线对齐并使用同一字体族。
//
// 数据来源是 EventListModel 的**逐列 role**（不是一张 QVariantMap）：delegate 只声明
// 自己真正需要的字段，`model.event` 那种「整张 map 复制进 JS 对象」的开销不再存在，
// 单行更新也只触发 `dataChanged(该行)`，其余 delegate 不受影响。
GlassCard {
    id: root
    // required property 而非默认值：漏绑会在创建时报错，而不是静默显示空白卡片。
    // 角色名统一带 event 前缀，避免与 Item/GlassCard 的内置属性撞名。
    required property string eventId
    required property string eventLocation
    required property string eventMagnitudeText
    required property string eventDepthText
    required property string eventTimeText
    required property string eventSourceTag
    required property var eventSourceTags
    required property string eventIntensity
    required property string eventIntensityLabel
    required property string eventIntensityColor
    required property bool eventActive
    required property bool eventFocused
    required property string eventDistanceText

    // 来源独立成最后一行；不能和深度共用一行，否则第二个来源会把关键数字挤掉。
    implicitHeight: 96 + sourceFlow.height
    radius: 12
    // 卡片本身不再是操作：悬停后浮出「地图显示 / 查看详情」两个按钮。
    // interactive 仍打开，只是为了沿用 GlassCard 的悬停配色与焦点框。
    interactive: true
    active: eventFocused
    signal detailRequested()
    Accessible.name: eventLocation + "，震级 " + eventMagnitudeText + "，悬停后可在地图查看或打开详情"
    onClicked: {}
    // 覆盖整张卡片的悬停检测：移到按钮上时 HoverHandler 仍然保持 hovered，
    // 否则鼠标一离开卡片主体，按钮就会在点下去之前消失。
    readonly property bool actionsVisible: cardHover.hovered || root.activeFocus
    // 软件后端没有可用的图层效果，退回不透明遮罩（否则浮层会完全透明、正文穿出来）。
    readonly property bool blurSupported: GraphicsInfo.api !== GraphicsInfo.Software
                                          && GraphicsInfo.api !== GraphicsInfo.Unknown

    HoverHandler { id: cardHover }

    Component {
        id: cardBlur
        MultiEffect {
            blurEnabled: true
            // 0.7 会把烈度徽章糊成一团色块；0.45 仍明显是毛玻璃，但还能看出原来的布局。
            blur: 0.45
            blurMax: 24
            saturation: 0.9
        }
    }

    // 悬停浮层：挡住下方正文，避免「看得到但点不到」；离开即消失。
    Rectangle {
        id: actionOverlay
        anchors.fill: parent
        // 必须压在正文之上：与正文同级时，后声明的 RowLayout 会盖住浮层。
        z: 5
        radius: root.radius
        // 有 GPU 时把正文模糊成毛玻璃，再用很淡的主题色压一层；软件后端退回不透明底色。
        color: root.blurSupported
               ? Qt.rgba(root.theme.surface.r, root.theme.surface.g,
                         root.theme.surface.b, root.theme.dark ? 0.34 : 0.42)
               : root.theme.surfaceContainerLow
        visible: root.actionsVisible
        Row {
            anchors.centerIn: parent
            spacing: 10
            GlassButton {
                objectName: "eventMapAction"
                theme: root.theme
                text: "地图显示"
                iconName: "map-pin"
                primary: true
                implicitHeight: 36
                onClicked: app.toggleMapFocus(root.eventId)
            }
            GlassButton {
                objectName: "eventDetailAction"
                theme: root.theme
                text: "查看详情"
                iconName: "info"
                implicitHeight: 36
                onClicked: root.detailRequested()
            }
        }
    }

    RowLayout {
        id: contentRow
        x: 14
        width: parent.width - 28
        // 悬停时把正文交给 MultiEffect 模糊；原始 RowLayout 仍在原处，只作为效果源。
        layer.enabled: root.actionsVisible && root.blurSupported
        layer.effect: root.blurSupported ? cardBlur : null
        anchors.verticalCenter: parent.verticalCenter
        spacing: 12

        // 列表固定展示「震源最大烈度」（震中当地量），本地预估烈度只出现在 HUD / 全屏预警。
        IntensityBadge {
            Layout.alignment: Qt.AlignVCenter
            badgeSize: 60
            label: root.eventIntensityLabel
            value: root.eventIntensity
            source: root.eventSourceTag
            badgeColor: root.eventIntensityColor
            textColor: root.theme.on(badgeColor)
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            spacing: 5

            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Rectangle {
                    visible: root.eventActive
                    width: 6; height: 6; radius: 3
                    color: root.theme.accent
                    Layout.alignment: Qt.AlignVCenter
                }
                Text {
                    Layout.fillWidth: true
                    text: root.eventLocation
                    color: root.theme.textPrimary
                    font.pixelSize: 15
                    elide: Text.ElideRight
                }
                AppIcon { visible: root.eventFocused; name: "map-pin"; size: 13; color: root.theme.accent; Layout.alignment: Qt.AlignVCenter }
            }

            // 信息层级：地点 → 震级/深度（扫列表时最先抓的数字）→ 发震时刻（细体）
            // → 来源。时间不再紧跟地点，避免抢走震级的视觉权重。
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Text {
                    Layout.alignment: Qt.AlignBaseline
                    text: "M " + root.eventMagnitudeText
                    color: root.theme.textPrimary
                    font.pixelSize: 17
                    font.weight: Font.Medium
                    font.family: root.theme.numberFamily
                }
                Text {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignBaseline
                    text: "深度 " + root.eventDepthText + " km"
                    color: root.theme.outline
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                Text {
                    Layout.alignment: Qt.AlignBaseline
                    visible: root.eventDistanceText.length > 0
                    text: "距您 " + root.eventDistanceText + " km"
                    color: root.theme.outline
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }
            }

            Text {
                Layout.fillWidth: true
                text: root.eventTimeText + "  UTC+8"
                color: root.theme.outline
                font.family: root.theme.numberFamily
                font.pixelSize: 11
                elide: Text.ElideRight
            }

            // 一个事件可能同时来自 Wolfx / Pancakes 等聚合源。每个来源单独成胶囊，
            // Flow 在窄侧栏自动换行，卡片高度随行数增加，不会覆盖时间或深度。
            Flow {
                id: sourceFlow
                Layout.fillWidth: true
                spacing: 4
                height: Math.max(18, childrenRect.height)

                Repeater {
                    model: root.eventSourceTags && root.eventSourceTags.length
                           ? root.eventSourceTags : [root.eventSourceTag]
                    delegate: Rectangle {
                        required property string modelData
                        height: 18
                        width: Math.min(sourceLabel.implicitWidth, 132) + 14
                        radius: 5
                        color: root.theme.surfaceContainerHigh
                        border.width: 1
                        border.color: root.theme.outlineVariant

                        Text {
                            id: sourceLabel
                            anchors.centerIn: parent
                            text: modelData
                            color: root.theme.outline
                            font.pixelSize: 10
                            elide: Text.ElideRight
                            width: parent.width - 14
                        }
                    }
                }
            }
        }
    }
}
