import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import EditorMode 1.0
import PropertiesPanel 1.0

Rectangle {
    id: root
    color: PropertiesPanelController.headerColor
    implicitHeight: 38

    property var modes: EditorModeController.availableModes

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        spacing: 10

        // Spacer first so mode buttons sit flush to the right of the toolbar row
        Item {
            Layout.fillWidth: true
        }

        Row {
            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
            spacing: 2

            Repeater {
                model: root.modes

                Rectangle {
                    width: Math.max(64, modeText.implicitWidth + 20)
                    height: 26
                    radius: 4
                    border.width: 1
                    border.color: EditorModeController.currentMode === modelData.mode
                        ? PropertiesPanelController.highlightColor
                        : PropertiesPanelController.borderColor
                    color: EditorModeController.currentMode === modelData.mode
                        ? PropertiesPanelController.highlightColor
                        : modeMouse.containsMouse
                          ? Qt.lighter(PropertiesPanelController.panelColor, 1.2)
                          : PropertiesPanelController.panelColor

                    Text {
                        id: modeText
                        anchors.centerIn: parent
                        text: modelData.label
                        color: PropertiesPanelController.textColor
                        font.pixelSize: 11
                        font.bold: EditorModeController.currentMode === modelData.mode
                    }

                    MouseArea {
                        id: modeMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: EditorModeController.requestMode(modelData.mode)
                    }
                }
            }
        }
    }
}
