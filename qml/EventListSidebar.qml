import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    objectName: "eventSidebar"
    property Theme theme: Theme {}
    property bool expanded: true
    property bool compact: false
    property bool hasLocation: false
    signal toggleRequested()
    readonly property int panelWidth: compact ? parent.width : 400
    property real collapse: expanded ? 0 : 1
    Behavior on collapse { NumberAnimation { duration: root.compact ? 0 : root.theme.motionDuration; easing.type: Easing.OutCubic } }
    width: panelWidth * (1 - collapse)
    clip: true
    property string searchQuery: ""
    property string activeFilter: "ALL"
    /// 非空 = 右侧面板显示该事件的详情，而不是列表。
    property string detailEventId: ""
    readonly property bool detailOpen: detailEventId.length > 0
    function openDetail(id) {
        // 先让控制器装载详情；条目在被点到之前刚好被目录裁掉时，不要再切到一个空面板。
        app.showEventDetail(id)
        root.detailEventId = (app.eventDetail && app.eventDetail.id) ? id : ""
    }
    function closeDetail() {
        root.detailEventId = ""
        app.closeEventDetail()
    }
    // 控制器裁剪目录时可能主动关掉详情（事件已不存在）：列表要及时回来。
    Connections {
        target: app
        function onEventDetailChanged() {
            if (root.detailEventId.length
                && (!app.eventDetail || !app.eventDetail.id))
                root.detailEventId = ""
        }
    }
    function updateFilter() {
        app.eventModel.setFilter(root.activeFilter, root.searchQuery)
    }
    // 「500 km 内」依赖基准位置：无定位时这个筛选项不可选，避免点出一个永远空的列表。
    function selectFilter(id) {
        if (id === "WITHIN_500" && !root.hasLocation) return
        root.activeFilter = id
    }
    onHasLocationChanged: {
        // 定位丢失后旧筛选没有意义：退回「全部」，否则列表会一直空着且看不出原因。
        if (!root.hasLocation && root.activeFilter === "WITHIN_500") root.activeFilter = "ALL"
    }
    onActiveFilterChanged: updateFilter()
    onSearchQueryChanged: updateFilter()
    Component.onCompleted: updateFilter()
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
            visible: !root.detailOpen
            RowLayout {
                width: parent.width
                GlassButton { visible: root.compact; theme: root.theme; iconName: "arrow-left"; accessibleName: "返回地图"; flat: true; onClicked: root.toggleRequested() }
                Column {
                    Layout.fillWidth: true
                    spacing: 5
                    Text { text: "地震事件"; font.pixelSize: 22; font.weight: Font.Medium; color: root.theme.textPrimary }
                    Text { text: "历史地震目录"; font.pixelSize: 11; color: root.theme.outline }
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
                        // 无定位时「500 km 内」不可选：行上本来就没有距离，选它只会得到空列表。
                        enabled: modelData.id !== "WITHIN_500" || root.hasLocation
                        implicitWidth: modelData.id === "WITHIN_500" ? 90 : 64
                        implicitHeight: 34
                        onClicked: root.selectFilter(modelData.id)
                    }
                }
            }
            RowLayout {
                width: parent.width
                Text { Layout.fillWidth: true; text: "事件记录"; font.pixelSize: 11; color: root.theme.outline }
                Text { text: eventList.count + " 条"; font.pixelSize: 11; color: root.theme.outline }
            }
            ListView {
                id: eventList
                width: parent.width
                height: Math.max(0, column.height - y)
                spacing: 10
                clip: true
                reuseItems: true
                model: app.eventModel
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                delegate: EventTile {
                    width: ListView.view.width
                    theme: root.theme
                    onDetailRequested: root.openDetail(eventId)
                }
                Column {
                    anchors.centerIn: parent
                    width: parent.width - 32
                    spacing: 16
                    visible: eventList.count === 0
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
        EventDetailPanel {
            anchors.fill: parent
            anchors.margins: 20
            visible: root.detailOpen
            theme: root.theme
            detail: app.eventDetail
            onBack: root.closeDetail()
        }
    }
}
