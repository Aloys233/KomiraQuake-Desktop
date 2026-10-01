import QtQuick

// Local Lucide SVGs, rasterized and tinted natively on GPU and software renderers.
Image {
    id: root
    property string name: ""
    property color color: "#191C1D"
    property real size: 20
    width: size
    height: size
    source: name.length ? "image://icons/" + name + "/" + color.toString().replace("#", "") : ""
    // Image requests the effective device-pixel size from the provider itself.
    sourceSize: Qt.size(Math.ceil(width), Math.ceil(height))
    fillMode: Image.PreserveAspectFit
    smooth: true
    Accessible.ignored: true
}
