import QtQuick
import QtQuick.Window
import "qrc:/qt/qml/KomiraQuake"

// StationStack 的测试夹具：不启动任何网络源，直接给容器喂合成数据。
// `testSources` 由 C++ 侧 setProperty 替换，用来验排序/展开/收回；`testReduceMotion`
// 用来验「关闭动画时不再有过渡」。
Window {
    id: window
    width: 900; height: 480; visible: true
    // 只在本夹具里用：把 Theme 的 reduceMotion 暴露成可写，避免去碰 SettingsStore。
    property bool testReduceMotion: false
    property var testSources: [
        { id: "kmoni", name: "NIED", caption: "当前最大震度", standard: "jma",
          hasData: false, raw: -3, text: "--" },
        { id: "snet", name: "S-Net", caption: "当前最大震度", standard: "jma",
          hasData: false, raw: -3, text: "--" },
        { id: "kma_station", name: "KMA", caption: "当前最大烈度", standard: "kma",
          hasData: false, raw: -3, text: "--" },
        { id: "pancakes_seis", name: "PancakesSeis", caption: "当前最大烈度", standard: "csis",
          hasData: false, raw: -1, text: "--" },
        { id: "wolfx_seis", name: "WolfxSeisJS", caption: "当前最大烈度", standard: "csis",
          hasData: false, raw: -1, text: "--" }
    ]
    Theme { id: material; reduceMotion: window.testReduceMotion }
    // 背景要有粗细不一的图案：纯色底上模糊看不出来，就无法用取证图核对
    // 「展开/收回时模糊是否跟着卡片一起被拖走」。
    Item {
        id: mapLayer
        objectName: "fixtureMap"
        anchors.fill: parent
        Rectangle { anchors.fill: parent; color: "#d8e5e7" }
        Repeater {
            model: 120
            Rectangle {
                required property int index
                x: index * 8
                width: (index % 3) + 1
                height: window.height
                color: index % 2 ? "#20383e" : "#f09a40"
                opacity: index % 5 ? 0.55 : 0.9
            }
        }
        Rectangle { x: 0; y: 300; width: window.width; height: 40; color: "#3b6ea5" }
    }
    StationStack {
        objectName: "fixtureStack"
        anchors.right: parent.right
        anchors.rightMargin: 16
        anchors.top: parent.top
        anchors.topMargin: 16
        theme: material
        backdropSource: mapLayer
        backdropLive: false
        sources: window.testSources
    }
}
