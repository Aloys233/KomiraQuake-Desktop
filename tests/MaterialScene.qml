import QtQuick
import QtQuick.Window
import "qrc:/qt/qml/KomiraQuake"

Window {
    id: window
    width: 520; height: 360; visible: true
    Theme { id: material }
    Item {
        id: mapLayer
        objectName: "fixtureMap"
        // The coordinate test moves this item; anchors.fill would restore its x/y.
        width: window.width; height: window.height
        Rectangle { anchors.fill: parent; color: "#d8e5e7" }
        Repeater {
            model: 65
            Rectangle { required property int index; x: index * 8; width: 4; height: 360; color: "#20383e" }
        }
        Rectangle { x: 260; width: 100; height: parent.height; color: "#f09a40" }
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
            Rectangle {
                objectName: "crispMark"
                x: 20; y: parent.height - 28; width: 32; height: 8
                color: material.textPrimary
            }
        }
    }
}
