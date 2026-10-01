import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    objectName: "eventSidebar"
    property Theme theme: Theme {}
    property bool expanded: true
    property bool compact: false
    property var events
    property bool hasLocation: false
    signal toggleRequested()
    readonly property int panelWidth: compact ? parent.width : 400
    property real collapse: expanded ? 0 : 1
    Behavior on collapse { NumberAnimation { duration: root.compact ? 0 : root.theme.motionDuration; easing.type: Easing.OutCubic } }
    width: panelWidth * (1 - collapse)
    clip: true
    property string searchQuery: ""
    property string activeFilter: "ALL"
    readonly property var filteredEvents: {
        if (!events) return [];
        let list = events.slice();
        if (activeFilter === "WITHIN_500") list = list.filter(e => e.hasDistance && e.distance <= 500);
        else if (activeFilter === "M4_PLUS") list = list.filter(e => e.magnitude >= 4.0);
        else if (activeFilter === "RECENT_24H") {
            const now = Date.now();
            list = list.filter(e => (now - e.timestamp) <= 24 * 3600 * 1000);
        }
        if (searchQuery.trim().length) {
            const q = searchQuery.trim().toLowerCase();
            list = list.filter(e => (e.location && e.location.toLowerCase().indexOf(q) >= 0) || (e.source && e.source.toLowerCase().indexOf(q) >= 0));
        }
        return list;
    }
    Rectangle {
        anchors.top: parent.top; anchors.bottom: parent.bottom; anchors.right: parent.right
        anchors.rightMargin: -root.panelWidth * root.collapse
        width: root.panelWidth
        color: root.theme.surface
        Column {
            id: column
            anchors.fill: parent
            anchors.margins: 20
            spacing: 14
            RowLayout {
                width: parent.width
                GlassButton { visible: root.compact; theme: root.theme; iconName: "arrow-left"; accessibleName: "返回地图"; flat: true; onClicked: root.toggleRequested() }
                Column {
                    Layout.fillWidth: true
                    spacing: 5
                    Text { text: "地震事件"; font.pixelSize: 22; font.weight: Font.Medium; color: root.theme.textPrimary }
                    Text { text: "实时速报与历史目录"; font.pixelSize: 11; color: root.theme.outline }
                }
                GlassButton { theme: root.theme; iconName: "refresh-cw"; accessibleName: "刷新事件"; flat: true; onClicked: app.refreshCatalog() }
            }
            GlassTextField {
                objectName: "eventSearch"
                theme: root.theme
                width: parent.width
                leftPadding: 36
                placeholderText: "搜索地名或数据源"
                onTextChanged: root.searchQuery = text
                AppIcon { x: 12; anchors.verticalCenter: parent.verticalCenter; name: "search"; size: 16; color: root.theme.outline }
            }
            Flow {
                width: parent.width
                spacing: 6
                Repeater {
                    model: [{id:"ALL",label:"全部"}, {id:"WITHIN_500",label:"500 km 内"}, {id:"M4_PLUS",label:"M4+"}, {id:"RECENT_24H",label:"近 24h"}]
                    GlassButton {
                        required property var modelData
                        theme: root.theme
                        text: modelData.label
                        primary: root.activeFilter === modelData.id
                        flat: !primary
                        implicitWidth: modelData.id === "WITHIN_500" ? 90 : 64
                        implicitHeight: 34
                        onClicked: root.activeFilter = modelData.id
                    }
                }
            }
            RowLayout {
                width: parent.width
                Text { Layout.fillWidth: true; text: "事件记录"; font.pixelSize: 11; color: root.theme.outline }
                Text { text: root.filteredEvents.length + " 条"; font.pixelSize: 11; color: root.theme.outline }
            }
            ListView {
                width: parent.width
                height: Math.max(0, column.height - y)
                spacing: 10
                clip: true
                model: root.filteredEvents
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                delegate: EventTile { width: ListView.view.width; theme: root.theme; event: modelData }
                Column {
                    anchors.centerIn: parent
                    width: parent.width - 32
                    spacing: 16
                    visible: !root.filteredEvents.length
                    AppIcon { anchors.horizontalCenter: parent.horizontalCenter; name: "radio"; size: 32; color: root.theme.outline }
                    Text {
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                        text: root.searchQuery.length ? "未找到匹配的事件" : "暂无符合条件的地震事件\n数据源持续守候中"
                        color: root.theme.outline
                        font.pixelSize: 13
                        lineHeight: 1.5
                    }
                }
            }
        }
    }
}
