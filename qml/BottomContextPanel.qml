import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import EditorMode 1.0
import PropertiesPanel 1.0
import AnimationControl 1.0

Rectangle {
    id: root
    color: PropertiesPanelController.panelColor
    border.color: PropertiesPanelController.borderColor

    property bool expanded: true
    property int contentMode: EditorModeController.currentMode

    // Set from C++ (mainwindow.cpp) so the Material Editor button can trigger
    // the QAction directly. Declared explicitly so the QML compiler resolves
    // the identifier locally instead of relying on an ambient context value.
    property var materialEditorAction: null

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            height: 28
            color: PropertiesPanelController.headerColor

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                spacing: 8

                Text {
                    Layout.fillWidth: true
                    text: EditorModeController.modeName + " Context"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 12
                    font.bold: true
                    elide: Text.ElideRight
                }

                Button {
                    implicitWidth: 28
                    implicitHeight: 22
                    text: root.expanded ? "-" : "+"
                    onClicked: root.expanded = !root.expanded
                }
            }
        }

        Loader {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.expanded
            sourceComponent: {
                if (root.contentMode === EditorModeController.EditMode) return editSummary
                if (root.contentMode === EditorModeController.AnimationMode) return animationSummary
                if (root.contentMode === EditorModeController.MaterialMode) return materialSummary
                if (root.contentMode === EditorModeController.ValidationMode) return validationSummary
                return objectSummary
            }
        }
    }

    Component {
        id: objectSummary
        RowLayout {
            anchors.fill: parent
            anchors.margins: 10
            spacing: 18

            SummaryText { label: "Selection"; value: PropertiesPanelController.hasSelection ? PropertiesPanelController.selectionName : "None" }
            SummaryText { label: "Scene Nodes"; value: PropertiesPanelController.sceneTreeModel ? PropertiesPanelController.sceneTreeModel.rowCount() : 0 }
            SummaryText { label: "Primitive"; value: PropertiesPanelController.hasPrimitive ? PropertiesPanelController.primitiveType : "No" }
            Item { Layout.fillWidth: true }
        }
    }

    Component {
        id: editSummary
        RowLayout {
            anchors.fill: parent
            anchors.margins: 10
            spacing: 18

            SummaryText { label: "Vertices"; value: EditModeController.vertexCount }
            SummaryText { label: "Triangles"; value: EditModeController.triangleCount }
            SummaryText { label: "Selected V/E/F"; value: EditModeController.selectedVertexCount + " / " + EditModeController.selectedEdgeCount + " / " + EditModeController.selectedFaceCount }
            SummaryText { label: "Warnings"; value: EditModeController.hasValidationWarnings ? EditModeController.degenerateTriangleCount : "None" }
            Item { Layout.fillWidth: true }
        }
    }

    Component {
        id: animationSummary
        RowLayout {
            anchors.fill: parent
            anchors.margins: 10
            spacing: 14

            SummaryText { label: "Animated"; value: PropertiesPanelController.hasAnimations ? "Available" : "None" }
            SummaryText { label: "Timeline"; value: AnimationControlController.hasAnimation ? AnimationControlController.selectedAnimation : "No clip" }
            Button {
                text: PropertiesPanelController.playing ? "Pause" : "Play"
                enabled: PropertiesPanelController.hasAnimations
                onClicked: PropertiesPanelController.playing = !PropertiesPanelController.playing
            }
            Item { Layout.fillWidth: true }
        }
    }

    Component {
        id: materialSummary
        RowLayout {
            anchors.fill: parent
            anchors.margins: 10
            spacing: 14

            SummaryText { label: "Selection"; value: PropertiesPanelController.hasSelection ? PropertiesPanelController.selectionName : "None" }
            Button {
                text: "Material Editor"
                onClicked: if (materialEditorAction) materialEditorAction.trigger()
            }
            SummaryText { label: "Presets"; value: PropertiesPanelController.hasSelection ? "Available" : "Select an object" }
            Item { Layout.fillWidth: true }
        }
    }

    Component {
        id: validationSummary
        RowLayout {
            anchors.fill: parent
            anchors.margins: 10
            spacing: 14

            SummaryText { label: "Selection"; value: MeshValidator.hasSelection ? PropertiesPanelController.selectionName : "None" }
            SummaryText { label: "Findings"; value: MeshValidator.validated ? MeshValidator.issues.length : "Not run" }
            Button {
                text: MeshValidator.validating ? "Validating" : "Validate"
                enabled: MeshValidator.hasSelection && !MeshValidator.validating
                onClicked: MeshValidator.validate()
            }
            Button {
                text: "Fix All"
                enabled: MeshValidator.hasFixableIssues
                onClicked: MeshValidator.fixAll()
            }
            Item { Layout.fillWidth: true }
        }
    }

    component SummaryText: ColumnLayout {
        required property string label
        required property var value
        spacing: 2

        Text {
            text: label
            color: PropertiesPanelController.textColor
            opacity: 0.62
            font.pixelSize: 10
        }
        Text {
            text: String(value)
            color: PropertiesPanelController.textColor
            font.pixelSize: 12
            font.bold: true
            elide: Text.ElideRight
            Layout.maximumWidth: 220
        }
    }
}
