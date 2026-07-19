import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import PropertiesPanel 1.0

Window {
    id: dialog
    title: "Split Bone"
    width: 380
    height: 180
    minimumWidth: 340
    flags: Qt.Dialog
    modality: Qt.ApplicationModal
    color: PropertiesPanelController.panelColor

    property string boneName: ""
    property real fraction: 0.5

    signal splitRequested(real t)
    signal cancelled()

    function openForBone(name) {
        dialog.boneName = name || ""
        dialog.fraction = 0.5
        fracSlider.value = 0.5
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
                ? "Split \"" + dialog.boneName + "\" along its axis:"
                : "Split the selected bone along its axis:"
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Text {
                text: "Fraction"
                color: PropertiesPanelController.textColor
                font.pixelSize: 12
            }
            Slider {
                id: fracSlider
                Layout.fillWidth: true
                from: 0.05
                to: 0.95
                value: 0.5
                onValueChanged: dialog.fraction = value
            }
            Text {
                text: dialog.fraction.toFixed(2)
                color: PropertiesPanelController.textColor
                font.pixelSize: 12
                Layout.preferredWidth: 36
            }
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
                color: PropertiesPanelController.highlightColor
                Text {
                    id: okLabel
                    anchors.centerIn: parent
                    text: "Split"
                    color: "white"
                    font.pixelSize: 12
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        dialog.splitRequested(dialog.fraction)
                        dialog.close()
                    }
                }
            }
        }
    }
}
