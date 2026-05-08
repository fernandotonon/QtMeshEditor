import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import EditorMode 1.0
import PropertiesPanel 1.0

Rectangle {
    id: root
    color: PropertiesPanelController.headerColor
    implicitHeight: 38

    property var modes: [
        { label: "Object", mode: EditorModeController.ObjectMode, tip: "Object mode" },
        { label: "Edit", mode: EditorModeController.EditMode, tip: "Edit mesh components" },
        { label: "Animation", mode: EditorModeController.AnimationMode, tip: "Animation tools" },
        { label: "Material", mode: EditorModeController.MaterialMode, tip: "Material tools" },
        { label: "Validation", mode: EditorModeController.ValidationMode, tip: "Mesh validation" }
    ]

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        spacing: 10

        Row {
            Layout.alignment: Qt.AlignVCenter
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

                    ToolTip.text: modelData.tip
                    ToolTip.visible: modeMouse.containsMouse
                }
            }
        }

        Text {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            text: EditorModeController.statusText
            color: PropertiesPanelController.textColor
            elide: Text.ElideRight
            font.pixelSize: 11
            opacity: 0.82
        }

        Button {
            Layout.alignment: Qt.AlignVCenter
            text: EditModeController.editModeActive ? "Exit Edit" : "Edit"
            enabled: EditModeController.editModeActive || EditorModeController.editModeAvailable
            implicitHeight: 26
            implicitWidth: 76
            font.pixelSize: 11
            onClicked: EditorModeController.toggleObjectEditMode()
            ToolTip.text: "Toggle Edit mode (Tab)"
            ToolTip.visible: hovered
        }
    }
}
