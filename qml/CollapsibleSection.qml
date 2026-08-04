import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import PropertiesPanel 1.0

Column {
    id: root

    property string title: "Section"
    property bool expanded: true
    property bool sectionVisible: true
    default property alias content: contentLoader.sourceComponent

    signal contentReady()

    visible: sectionVisible
    width: parent ? parent.width : 200

    Rectangle {
        id: header
        width: parent.width
        height: 28
        color: headerMouse.containsMouse ? Qt.lighter(PropertiesPanelController.headerColor, 1.1)
                                         : PropertiesPanelController.headerColor
        border.color: PropertiesPanelController.borderColor
        border.width: 1

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 6
            anchors.rightMargin: 6
            spacing: 4

            Text {
                text: root.expanded ? "\u25BC" : "\u25B6"
                color: PropertiesPanelController.textColor
                font.pixelSize: 10
            }

            Text {
                text: root.title
                color: PropertiesPanelController.textColor
                font.pixelSize: 12
                font.bold: true
                Layout.fillWidth: true
            }
        }

        MouseArea {
            id: headerMouse
            anchors.fill: parent
            hoverEnabled: true
            activeFocusOnTab: true
            onClicked: root.expanded = !root.expanded
            Keys.onReturnPressed: root.expanded = !root.expanded
            Keys.onSpacePressed: root.expanded = !root.expanded
        }
    }

    Loader {
        id: contentLoader
        width: parent.width
        // Defer activation to the next event-loop turn. Synchronous Loader
        // startup while a parent component is still finalizing (e.g. expanding
        // a section during a binding cascade) can SIGSEGV — see PropertiesPanel
        // Component.onCompleted comment.
        active: loadActive
        visible: root.expanded
        property bool loadActive: false
        onLoaded: root.contentReady()
    }

    property int _loadGeneration: 0

    onExpandedChanged: {
        if (root.expanded) {
            const gen = ++root._loadGeneration
            Qt.callLater(function() {
                if (root.expanded && gen === root._loadGeneration)
                    contentLoader.loadActive = true
            })
        } else {
            ++root._loadGeneration
            contentLoader.loadActive = false
        }
    }

    Component.onCompleted: {
        if (root.expanded) {
            const gen = ++root._loadGeneration
            Qt.callLater(function() {
                if (root.expanded && gen === root._loadGeneration)
                    contentLoader.loadActive = true
            })
        }
    }
}
