import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import PropertiesPanel 1.0

Column {
    id: root

    property string title: "Section"
    property bool expanded: true
    property bool sectionVisible: true
    default property alias content: contentLoader.sourceComponent

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
            onClicked: root.expanded = !root.expanded
        }
    }

    Loader {
        id: contentLoader
        width: parent.width
        active: root.expanded
        visible: root.expanded
    }
}
