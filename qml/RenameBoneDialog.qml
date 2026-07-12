import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import PropertiesPanel 1.0

Window {
    id: dialog
    title: "Rename Bone"
    width: 360
    height: 160
    minimumWidth: 320
    flags: Qt.Dialog
    modality: Qt.ApplicationModal
    color: PropertiesPanelController.panelColor

    property string boneName: ""

    signal renameRequested(string newName)
    signal cancelled()

    function openForBone(name) {
        dialog.boneName = name || ""
        nameInput.text = name || ""
        dialog.show()
        dialog.raise()
        dialog.requestActivate()
        nameInput.forceActiveFocus()
        nameInput.selectAll()
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
            } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                submitRename()
                event.accepted = true
            }
        }
    }

    function submitRename() {
        const trimmed = nameInput.text.trim()
        if (trimmed.length === 0 || trimmed === dialog.boneName)
            return
        dialog.renameRequested(trimmed)
        dialog.close()
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
                ? "Rename bone \"" + dialog.boneName + "\":"
                : "Rename the selected bone:"
        }

        Rectangle {
            Layout.fillWidth: true
            height: 28
            radius: 3
            color: PropertiesPanelController.inputColor
            border.color: PropertiesPanelController.borderColor

            TextInput {
                id: nameInput
                anchors.fill: parent
                anchors.margins: 6
                color: PropertiesPanelController.textColor
                font.pixelSize: 11
                clip: true
                verticalAlignment: TextInput.AlignVCenter
                selectByMouse: true
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Item { Layout.fillWidth: true }

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

            Rectangle {
                Layout.preferredWidth: 72
                height: 28
                radius: 3
                color: renameMa.containsMouse && renameMa.enabled
                    ? Qt.lighter(PropertiesPanelController.highlightColor, 1.1)
                    : PropertiesPanelController.highlightColor
                border.color: PropertiesPanelController.borderColor
                opacity: renameMa.enabled ? 1.0 : 0.45
                Text {
                    anchors.centerIn: parent
                    text: "Rename"
                    font.pixelSize: 10
                    color: "white"
                }
                MouseArea {
                    id: renameMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    enabled: nameInput.text.trim().length > 0
                            && nameInput.text.trim() !== dialog.boneName
                    onClicked: dialog.submitRename()
                }
            }
        }
    }
}
