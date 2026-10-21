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
                objectName: root.objectName.length > 0 ? root.objectName + "-value" : ""
                text: root.valueText
                color: root.theme.textPrimary
                font.family: root.theme.numberFamily
                font.pixelSize: 14
            }
        }

        GlassSlider {
            id: control
            objectName: root.objectName.length > 0 ? root.objectName + "-control" : ""
            Layout.fillWidth: true
            theme: root.theme
            enabled: root.active
            from: root.from
            to: root.to
            stepSize: root.stepSize
            Accessible.name: root.title
            // 拖动时逐帧提交，但**每次只写这一个键**：AppController 现在按项接线
            // （见 wireSettings()），音量滑块只改 QAudioOutput 音量，不再连带重推
            // 数据源、重算全表烈度、重建测站元数据。SettingsStore::set() 自身
            // 对「值没变」也会早退，因此同一档位重复拖动不产生任何写入。
            //
            // `moved()` 是无参信号（见 QtQuick.Controls 的 Slider），所以这里不能写成
            // `onMoved: value => ...`：那个参数名会**遮蔽** Slider 自己的 `value` 属性，
            // 处理器体里读到的就是被遮蔽的那个（恒为 undefined）。
            onMoved: root.moved(control.value)
        }

        // 滑块值**单向**由 root.value（即设置项）驱动。必须用 Binding 而不是字面
        // `value: root.value`：用户拖动会向 control.value 写值，QML 的破坏性绑定语义
        // 会**断开**字面绑定，此后设置再变滑块也不动（表现为「拖完滑块，数值文本不变」
        // 「恢复默认后滑块停在上次手拖的位置」）。RestoreBindingOrValue 让 root.value
        // 一变就重新夺回控制权。
        Binding {
            target: control
            property: "value"
            value: root.value
            restoreMode: Binding.RestoreBindingOrValue
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
    /// 当前值，由调用方绑定到设置项。拖动只经 `moved` 回报、**不回写本属性**：
    /// 回写会断开调用方的绑定，设置项就不再是唯一真值（同一个值存在两份、还可能与
    /// 设置里的实际值不同）。
    property real value: 0
    signal moved(real value)
}
