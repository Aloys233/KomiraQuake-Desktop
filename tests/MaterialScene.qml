import QtQuick
import QtQuick.Window
import "qrc:/qt/qml/KomiraQuake"

Window {
    width: 520; height: 360; visible: true
    Theme { id: material }
    Item {
        id: mapLayer
        objectName: "fixtureMap"
        anchors.fill: parent
        Rectangle { anchors.fill: parent; color: "#d8e5e7" }
        Repeater {
            model: 65
            Rectangle { required property int index; x: index * 8; width: 4; height: 360; color: "#20383e" }
        }
    }
    Item {
        id: movingParent
        objectName: "movingParent"
        x: 20; y: 20
        GlassCard {
            objectName: "fixtureCard"
            x: 80; y: 70; width: 280; height: 180
            theme: material
            backdropSource: mapLayer
            Text { objectName: "crispText"; anchors.centerIn: parent; text: "清晰文字 · 123456"; font.pixelSize: 20; color: material.textPrimary }
        }
    }
}
