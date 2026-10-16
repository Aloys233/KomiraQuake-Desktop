import QtQuick
import QtQuick.Layouts

// 地图右上角「测站源当前最大值」小卡。样式对齐 flutterRhythmQuake 的 StationDashboard 小卡：
// 窄竖版（源名 + 横线 + 小标签 + 大号数值），没有大方块徽章。
// 数据来自各实时测站通道（NIED Kmoni / PancakesSeis / WolfxSeisJS）。仅显示，不参与预警。
GlassCard {
    id: root
    objectName: "stationIntensityCard"
    /// 卡头源名（如 NIED / PancakesSeis / WolfxSeisJS）。
    property string sourceName: "NIED"
    /// 数值说明（如 当前最大震度）。
    property string caption: "当前最大震度"
    /// 烈度制式：`jma`（計測震度，NIED）/ `csis`（中国地震烈度，Pancakes・WolfxSeisJS）。
    property string standard: "jma"
    /// `{ hasData, raw, text, standard, stationCount, frameTime }`，来自 app.stationSources。
    property var maxInfo: ({})
    readonly property bool hasData: !!maxInfo && maxInfo.hasData === true
    readonly property real raw: (maxInfo && maxInfo.raw !== undefined) ? maxInfo.raw : -3
    readonly property string valueText: hasData ? (maxInfo.text || "--") : "--"

    // 三个测站源使用同一宽度，避免标题长度导致右上角卡片跳宽。
    readonly property real cardWidth: 84
    implicitWidth: cardWidth
    Layout.minimumWidth: cardWidth
    Layout.preferredWidth: cardWidth
    Layout.maximumWidth: cardWidth
    implicitHeight: body.implicitHeight + 14
    radius: 10

    ColumnLayout {
        id: body
        anchors.fill: parent
        anchors.margins: 7
        spacing: 2

        Text {
            id: titleText
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: root.sourceName
            color: root.theme.textPrimary
            font.family: root.theme.fontFamily
            font.pixelSize: 10
            font.weight: Font.Bold
        }
        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            width: 14; height: 2; radius: 1
            color: root.theme.textPrimary
            opacity: 0.45
        }
        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: root.caption
            color: root.theme.outline
            font.family: root.theme.fontFamily
            font.pixelSize: 8
        }
        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: root.valueText
            color: root.hasData ? root.theme.stationTextColorFor(root.raw, root.standard)
                                : root.theme.outline
            font.family: root.theme.fontFamily
            font.pixelSize: 17
            font.weight: Font.Bold
        }
    }
}
