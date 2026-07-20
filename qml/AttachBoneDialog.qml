import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import PropertiesPanel 1.0
import MaterialEditorQML 1.0

Window {
    id: dialog
    title: "Attach Bone to Entity"
    width: 400
    height: 200
    minimumWidth: 340
    flags: Qt.Dialog
    modality: Qt.ApplicationModal
    color: PropertiesPanelController.panelColor

    property string boneName: ""
    property var targetEntities: []

    signal attachRequested(string dstEntityName)
    signal cancelled()

    function openForBone(name, targets) {
        dialog.boneName = name || ""
        dialog.targetEntities = targets || []
        var names = []
        for (var i = 0; i < dialog.targetEntities.length; i++)
            names.push(dialog.targetEntities[i].name)
        targetCombo.model = names
        targetCombo.currentIndex = names.length > 0 ? 0 : -1
        dialog.show()
        dialog.raise()
        dialog.requestActivate()
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
                ? "Copy \"" + dialog.boneName + "\" (and descendants) onto another entity's skeleton (rig only):"
                : "Attach the selected bone onto another entity's skeleton:"
        }

        ThemedComboBox {
            id: targetCombo
            Layout.fillWidth: true
            Layout.preferredHeight: 24
            font.pixelSize: 11
            displayText: currentIndex >= 0 ? currentText : "(no other entities)"
        }

        Item { Layout.fillHeight: true }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: 8
            Rectangle {
                width: cancelLabel.implicitWidth + 20
                height: 28
                radius: 3
                color: PropertiesPanelController.headerColor
                border.color: PropertiesPanelController.borderColor
                Text {
                    id: cancelLabel
                    anchors.centerIn: parent
                    text: "Cancel"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 12
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: { dialog.cancelled(); dialog.close() }
                }
            }
            Rectangle {
                width: okLabel.implicitWidth + 20
                height: 28
                radius: 3
                opacity: targetCombo.currentIndex >= 0 ? 1.0 : 0.45
                color: PropertiesPanelController.highlightColor
                Text {
                    id: okLabel
                    anchors.centerIn: parent
                    text: "Attach"
                    color: "white"
                    font.pixelSize: 12
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: targetCombo.currentIndex >= 0
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        dialog.attachRequested(targetCombo.currentText)
                        dialog.close()
                    }
                }
            }
        }
    }
}
