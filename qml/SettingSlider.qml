import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 滑块行：标题行右侧内联当前数值，下方是滑块与说明。
// 数值与滑块同处一个视觉单元，避免「数值写在别处、控件在别处」的对不上感。
// 注意：这里用 `active` 而非 `enabled`——Item 已有内置的 enabled 属性，
// 重新声明会触发 "overrides a member of the base object" 警告。
Item {
    id: root
    property Theme theme: Theme {}
    property string title: ""
    property string description: ""
    /// 标题行右侧显示的当前值，由调用方格式化（如 "50%"、"3.5 度"）。
    property string valueText: ""
    /// false 时整体置灰（依赖项被上级开关关闭）。
    property bool active: true

    implicitHeight: stack.implicitHeight
    opacity: active ? 1 : 0.55

    ColumnLayout {
        id: stack
        width: parent.width
        spacing: 6

        RowLayout {
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: root.title
                color: root.theme.textPrimary
                font.pixelSize: 14
                elide: Text.ElideRight
            }
            Text {
                text: root.valueText
                color: root.theme.textPrimary
                font.family: root.theme.numberFamily
                font.pixelSize: 14
            }
        }

        GlassSlider {
            Layout.fillWidth: true
            theme: root.theme
            enabled: root.active
            from: root.from
            to: root.to
            stepSize: root.stepSize
            value: root.value
            Accessible.name: root.title
            onMoved: root.moved(value)
        }

        Text {
            Layout.fillWidth: true
            visible: text.length > 0
            text: root.description
            wrapMode: Text.Wrap
            lineHeight: 1.4
            color: root.theme.outline
            font.pixelSize: 12
        }
    }

    property real from: 0
    property real to: 1
    property real stepSize: 0.05
    /// 由 GlassSlider 的 onMoved 回写，避免与 `value` 形成绑定环。
    property real value: 0
    signal moved(real value)
}
