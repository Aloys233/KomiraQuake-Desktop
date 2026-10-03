import QtQuick
import QtQuick.Effects

// Capture ONLY the supplied map render layer. Never capture an ancestor containing
// this panel, its foreground, or other effects. Padded samples avoid dark blur edges;
// the separate alpha mask supplies real rounded clipping (Item.clip is rectangular).
Item {
    id: root
    objectName: "backdropBlur"
    required property Item sourceItem
    property real cornerRadius: 16
    readonly property int blurRadius: 64
    // Keep the sample margin at least as wide as the full blur kernel.
    readonly property real padding: blurRadius
    clip: true

    // mapToItem() itself has no QML dependency notifications. Read each chain's
    // geometry inputs so the binding invalidates on layout, reparenting, or source
    // movement, without a frame timer or a recursive capture. The two chains need
    // not share a parent (Main's HUD and MapView's toolbar do not).
    //
    // Item.transform is deliberately NOT read: it is a QQmlListProperty with no
    // NOTIFY signal, so it is non-bindable. Reading it can never invalidate the
    // binding and only makes QQmlExpression log "depends on non-bindable
    // properties" on every update. No item in this app uses the transform list,
    // and the bindable inputs below cover layout/scale/rotation changes.
    function transformStamp(item) {
        let stamp = "";
        while (item) {
            stamp += [item.x, item.y, item.width, item.height, item.scale,
                      item.rotation, item.transformOrigin].join(",");
            item = item.parent;
        }
        return stamp;
    }

    ShaderEffectSource {
        id: sample
        objectName: "backdropSample"
        sourceItem: root.sourceItem
        sourceRect: {
            const w = root.width, h = root.height;
            const geometry = root.transformStamp(root) + root.transformStamp(root.sourceItem);
            return root.mapToItem(root.sourceItem,
                                  Qt.rect(-root.padding, -root.padding,
                                          w + 2 * root.padding, h + 2 * root.padding));
        }
        textureSize: Qt.size(Math.ceil((root.width + 2 * root.padding) / 2),
                             Math.ceil((root.height + 2 * root.padding) / 2))
        width: root.width + 2 * root.padding
        height: root.height + 2 * root.padding
        live: true
        recursive: false
        hideSource: false
        visible: false
        smooth: true
    }
    Item {
        id: mask
        width: sample.width
        height: sample.height
        visible: false
        layer.enabled: true
        layer.smooth: true
        Rectangle {
            x: root.padding; y: root.padding
            width: root.width; height: root.height
            radius: root.cornerRadius
            color: "white"
            antialiasing: true
        }
    }
    MultiEffect {
        objectName: "backdropEffect"
        x: -root.padding; y: -root.padding
        width: sample.width; height: sample.height
        source: sample
        autoPaddingEnabled: false
        blurEnabled: true
        blurMax: root.blurRadius
        blur: 1.0
        maskEnabled: true
        maskSource: mask
        maskThresholdMin: 0.0
        maskSpreadAtMin: 0.0
    }
}
