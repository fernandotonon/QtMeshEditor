import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import EditorMode 1.0
import PropertiesPanel 1.0
import AnimationControl 1.0
import AssetBrowser 1.0

Rectangle {
    id: root
    color: PropertiesPanelController.panelColor
    border.color: PropertiesPanelController.borderColor

    property bool expanded: true
    property int contentMode: EditorModeController.currentMode
    property var materialEditorAction: null
    property var bottomToolHost: null
    readonly property string currentSummaryObjectName: summaryLoader.item ? summaryLoader.item.objectName : ""

    function revealBottomTool(toolId) {
        if (bottomToolHost && bottomToolHost.revealBottomTool)
            bottomToolHost.revealBottomTool(toolId)
    }

    function issueSummary() {
        if (!MeshValidator.validated)
            return "Not run"
        return String(MeshValidator.issues.length)
    }

    function fileSummary() {
        return String(AssetBrowserController.files.length)
    }

    function rootFolderName() {
        var path = AssetBrowserController.rootPath
        if (!path || path.length === 0)
            return "Not set"
        var parts = path.split(/[\\/]/)
        for (var i = parts.length - 1; i >= 0; --i) {
            if (parts[i] && parts[i].length > 0)
                return parts[i]
        }
        return path
    }

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

                ToolButton {
                    objectName: "bottomContextExpandButton"
                    implicitWidth: 28
                    implicitHeight: 22
                    text: root.expanded ? "-" : "+"
                    onClicked: root.expanded = !root.expanded
                }
            }
        }

        Loader {
            id: summaryLoader
            objectName: "bottomContextSummaryLoader"
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
        ColumnLayout {
            objectName: "objectSummaryRoot"
            anchors.fill: parent
            anchors.margins: 10
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                spacing: 18

                SummaryText { label: "Selection"; value: PropertiesPanelController.hasSelection ? PropertiesPanelController.selectionName : "None" }
                SummaryText { label: "Transform Target"; value: PropertiesPanelController.transformTargetLabel }
                SummaryText { label: "Scene Nodes"; value: PropertiesPanelController.sceneTreeModel ? PropertiesPanelController.sceneTreeModel.rowCount() : 0 }
                SummaryText { label: "Primitive"; value: PropertiesPanelController.hasPrimitive ? PropertiesPanelController.primitiveType : "No" }
                Item { Layout.fillWidth: true }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                ActionButton {
                    objectName: "objectContextButton"
                    text: "Context"
                    onClicked: root.revealBottomTool("context")
                }
                ActionButton {
                    objectName: "objectAssetsButton"
                    text: "Asset Browser"
                    onClicked: root.revealBottomTool("assetBrowser")
                }
                Item { Layout.fillWidth: true }
            }
        }
    }

    Component {
        id: editSummary
        ColumnLayout {
            objectName: "editSummaryRoot"
            anchors.fill: parent
            anchors.margins: 10
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                spacing: 18

                SummaryText { label: "Scope"; value: "Mesh Geometry" }
                SummaryText { label: "Vertices"; value: EditModeController.vertexCount }
                SummaryText { label: "Triangles"; value: EditModeController.triangleCount }
                SummaryText { label: "SubMeshes"; value: EditModeController.subMeshCount }
                SummaryText { label: "Selected V/E/F"; value: EditModeController.selectedVertexCount + " / " + EditModeController.selectedEdgeCount + " / " + EditModeController.selectedFaceCount }
                SummaryText { label: "Warnings"; value: EditModeController.hasValidationWarnings ? EditModeController.degenerateTriangleCount : "None" }
                Item { Layout.fillWidth: true }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                ActionButton {
                    objectName: "editContextButton"
                    text: "Context"
                    onClicked: root.revealBottomTool("context")
                }
                ActionButton {
                    objectName: "editValidationButton"
                    text: "Validation"
                    onClicked: EditorModeController.requestMode(EditorModeController.ValidationMode)
                }
                Item { Layout.fillWidth: true }
            }
        }
    }

    Component {
        id: animationSummary
        ColumnLayout {
            objectName: "animationSummaryRoot"
            anchors.fill: parent
            anchors.margins: 10
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                spacing: 18

                SummaryText { label: "Animated"; value: PropertiesPanelController.hasAnimations ? "Available" : "None" }
                SummaryText { label: "Clip"; value: AnimationControlController.hasAnimation ? AnimationControlController.selectedAnimation : "No clip" }
                SummaryText { label: "Bone"; value: AnimationControlController.selectedBone.length > 0 ? AnimationControlController.selectedBone : "None" }
                SummaryText { label: "Timeline"; value: AnimationControlController.sliderValue / 1000.0 }
                Item { Layout.fillWidth: true }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                ActionButton {
                    objectName: "animationPlayButton"
                    text: PropertiesPanelController.playing ? "Pause" : "Play"
                    enabled: PropertiesPanelController.hasAnimations
                    onClicked: PropertiesPanelController.playing = !PropertiesPanelController.playing
                }
                ActionButton {
                    objectName: "animationDopeSheetButton"
                    text: "Dope Sheet"
                    onClicked: root.revealBottomTool("dopeSheet")
                }
                ActionButton {
                    objectName: "animationCurveEditorButton"
                    text: "Curve Editor"
                    onClicked: root.revealBottomTool("curveEditor")
                }
                Item { Layout.fillWidth: true }
            }
        }
    }

    Component {
        id: materialSummary
        ColumnLayout {
            objectName: "materialSummaryRoot"
            anchors.fill: parent
            anchors.margins: 10
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                spacing: 18

                SummaryText { label: "Selection"; value: PropertiesPanelController.hasSelection ? PropertiesPanelController.selectionName : "None" }
                SummaryText { label: "Asset Root"; value: root.rootFolderName() }
                SummaryText { label: "Files"; value: root.fileSummary() }
                Item { Layout.fillWidth: true }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                ActionButton {
                    objectName: "materialEditorButton"
                    text: "Material Editor"
                    onClicked: if (materialEditorAction) materialEditorAction.trigger()
                }
                ActionButton {
                    objectName: "materialAssetBrowserButton"
                    text: "Asset Browser"
                    onClicked: root.revealBottomTool("assetBrowser")
                }
                Item { Layout.fillWidth: true }
            }
        }
    }

    Component {
        id: validationSummary
        ColumnLayout {
            objectName: "validationSummaryRoot"
            anchors.fill: parent
            anchors.margins: 10
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                spacing: 18

                SummaryText { label: "Selection"; value: MeshValidator.hasSelection ? PropertiesPanelController.selectionName : "None" }
                SummaryText { label: "Findings"; value: root.issueSummary() }
                SummaryText { label: "Fixable"; value: MeshValidator.hasFixableIssues ? "Yes" : "No" }
                Item { Layout.fillWidth: true }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                ActionButton {
                    objectName: "validationRunButton"
                    text: MeshValidator.validating ? "Validating" : "Validate"
                    enabled: MeshValidator.hasSelection && !MeshValidator.validating
                    onClicked: MeshValidator.validate()
                }
                ActionButton {
                    objectName: "validationFixButton"
                    text: "Fix All"
                    enabled: MeshValidator.hasFixableIssues
                    onClicked: MeshValidator.fixAll()
                }
                ActionButton {
                    objectName: "validationContextButton"
                    text: "Context"
                    onClicked: root.revealBottomTool("context")
                }
                Item { Layout.fillWidth: true }
            }
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

    component ActionButton: Button {
        implicitHeight: 26
        implicitWidth: Math.max(88, contentItem.implicitWidth + 20)
    }
}
