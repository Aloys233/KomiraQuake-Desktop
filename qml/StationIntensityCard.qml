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
    /// 烈度制式：`jma`（計測震度，NIED・S-Net）/ `csis`（中国地震烈度，Pancakes・WolfxSeisJS）
    /// / `kma`（韩国 MMI，KMA PEWS）。
    property string standard: "jma"
    /// `{ hasData, raw, text, standard, stationCount, frameTime }`，来自 app.stationSources。
    property var maxInfo: ({})
    readonly property bool hasData: !!maxInfo && maxInfo.hasData === true
    readonly property real raw: (maxInfo && maxInfo.raw !== undefined) ? maxInfo.raw : -3
    readonly property string valueText: hasData ? (maxInfo.text || "--") : "--"

    // 三个测站源使用同一宽度，避免标题长度导致右上角卡片跳宽。
    // 90 而不是 84：标题 11px 下 PancakesSeis/WolfxSeisJS 需要约 71px，
    // 84 的内宽只有 70px，分数缩放下会挤到字母粘连。
    readonly property real cardWidth: 90
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
            // 不显式指定 Google Sans：继承应用字体，中文才会落到内嵌的思源黑体。
            // 10px 在 1.3/1.5 分数缩放下 i/s 容易粘连（「i 看起来像 ii」），11px 明显更稳。
            font.pixelSize: 11
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
            font.pixelSize: 8
        }
        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: root.valueText
            color: root.hasData ? root.theme.stationTextColorFor(root.raw, root.standard)
                                : root.theme.outline
            font.pixelSize: 17
            font.weight: Font.Bold
        }
    }
}
