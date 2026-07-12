import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import PropertiesPanel 1.0

Window {
    id: dialog
    title: "Remove Bone"
    width: 360
    height: 200
    minimumWidth: 320
    flags: Qt.Dialog
    modality: Qt.ApplicationModal
    color: PropertiesPanelController.panelColor

    property string boneName: ""

    signal promoteChildrenRequested()
    signal removeSubtreeRequested()
    signal cancelled()

    function openForBone(name) {
        dialog.boneName = name || ""
        dialog.show()
        dialog.raise()
        dialog.requestActivate()
        keyCapture.forceActiveFocus()
    }

    Item {
        id: keyCapture
        anchors.fill: parent
        focus: true
        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_Escape) {
                dialog.cancelled()
                dialog.close()
                event.accepted = true
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            font.pixelSize: 12
            color: PropertiesPanelController.textColor
            text: dialog.boneName.length > 0
                ? "Remove bone \"" + dialog.boneName + "\"?"
                : "Remove the selected bone?"
        }

        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            font.pixelSize: 10
            opacity: 0.85
            color: PropertiesPanelController.textColor
            text: "Promote children re-parents child bones to this bone's parent. "
                + "Remove subtree deletes this bone and all descendants. "
                + "Vertex weights transfer to the parent when possible."
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Item { Layout.fillWidth: true }

            Rectangle {
                Layout.preferredWidth: promoteBtn.implicitWidth + 16
                height: 28
                radius: 3
                color: promoteMa.containsMouse
                    ? Qt.lighter(PropertiesPanelController.headerColor, 1.15)
                    : PropertiesPanelController.headerColor
                border.color: PropertiesPanelController.borderColor
                Text {
                    id: promoteBtn
                    anchors.centerIn: parent
                    text: "Promote children"
                    font.pixelSize: 10
                    color: PropertiesPanelController.textColor
                }
                MouseArea {
                    id: promoteMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        dialog.promoteChildrenRequested()
                        dialog.close()
                    }
                }
            }

            Rectangle {
                Layout.preferredWidth: subtreeBtn.implicitWidth + 16
                height: 28
                radius: 3
                color: subtreeMa.containsMouse
                    ? Qt.lighter(PropertiesPanelController.highlightColor, 1.1)
                    : PropertiesPanelController.highlightColor
                border.color: PropertiesPanelController.borderColor
                Text {
                    id: subtreeBtn
                    anchors.centerIn: parent
                    text: "Remove subtree"
                    font.pixelSize: 10
                    color: "white"
                }
                MouseArea {
                    id: subtreeMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        dialog.removeSubtreeRequested()
                        dialog.close()
                    }
                }
            }

            Rectangle {
                Layout.preferredWidth: 64
                height: 28
                radius: 3
                color: cancelMa.containsMouse
                    ? Qt.lighter(PropertiesPanelController.panelColor, 1.08)
                    : PropertiesPanelController.panelColor
                border.color: PropertiesPanelController.borderColor
                Text {
                    anchors.centerIn: parent
                    text: "Cancel"
                    font.pixelSize: 10
                    color: PropertiesPanelController.textColor
                }
                MouseArea {
                    id: cancelMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        dialog.cancelled()
                        dialog.close()
                    }
                }
            }
        }
    }
}
