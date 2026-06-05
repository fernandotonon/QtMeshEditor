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
    readonly property string currentSummaryObjectName: summaryLoader.item ? summaryLoader.item.objectName : ""

    // Count error+warning rows (the "must fix" tier). Info rows are
    // perf observations and counted separately via suggestionSummary().
    function issueSummary() {
        if (!MeshValidator.validated)
            return "Not run"
        var n = 0
        for (var i = 0; i < MeshValidator.issues.length; ++i) {
            var item = MeshValidator.issues[i]
            var typ = item.type !== undefined ? item.type : item["type"]
            if (typ === "error" || typ === "warning")
                ++n
        }
        return String(n)
    }

    // Phase 6: count info rows that carry a one-click fix (e.g. vertex
    // cache reorder). These aren't errors but the user has an in-UI
    // action available, so surface them as "Suggestions" in the panel.
    function suggestionSummary() {
        if (!MeshValidator.validated)
            return "Not run"
        var n = 0
        for (var i = 0; i < MeshValidator.issues.length; ++i) {
            var item = MeshValidator.issues[i]
            var typ = item.type !== undefined ? item.type : item["type"]
            var fixable = item.fixable === true
            if (typ === "info" && fixable)
                ++n
        }
        return String(n)
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
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                spacing: 18

                SummaryText { label: "Selection"; value: PropertiesPanelController.hasSelection ? PropertiesPanelController.selectionName : "None" }
                SummaryText { label: "Transform Target"; value: PropertiesPanelController.transformTargetLabel }
                SummaryText { label: "Scene Nodes"; value: PropertiesPanelController.sceneTreeModel ? PropertiesPanelController.sceneTreeModel.rowCount() : 0 }
                SummaryText { label: "Primitive"; value: PropertiesPanelController.hasPrimitive ? PropertiesPanelController.primitiveType : "No" }
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
            spacing: 0

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
        }
    }

    Component {
        id: animationSummary
        ColumnLayout {
            objectName: "animationSummaryRoot"
            anchors.fill: parent
            anchors.margins: 10
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                spacing: 18

                SummaryText { label: "Animated"; value: PropertiesPanelController.hasAnimations ? "Available" : "None" }
                SummaryText { label: "Clip"; value: AnimationControlController.hasAnimation ? AnimationControlController.selectedAnimation : "No clip" }
                SummaryText { label: "Bone"; value: AnimationControlController.selectedBone.length > 0 ? AnimationControlController.selectedBone : "None" }
                SummaryText { label: "Timeline"; value: AnimationControlController.sliderValue / 1000.0 }
                SummaryText { label: "Playback"; value: PropertiesPanelController.playing ? "Playing" : "Paused" }
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
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                spacing: 18

                SummaryText { label: "Selection"; value: PropertiesPanelController.hasSelection ? PropertiesPanelController.selectionName : "None" }
                SummaryText { label: "Asset Root"; value: root.rootFolderName() }
                SummaryText { label: "Files"; value: root.fileSummary() }
                SummaryText { label: "Editor"; value: PropertiesPanelController.hasSelection ? "Available" : "Select an object" }
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
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                spacing: 18

                SummaryText { label: "Profile"; value: AssetScanController.selectedProfileId.length > 0 ? AssetScanController.selectedProfileId : "None" }
                SummaryText { label: "Asset Root"; value: root.rootFolderName() }
                SummaryText { label: "Folder Scan"; value: AssetScanController.scanning ? "Running" : (AssetScanController.hasResults ? (AssetScanController.summaryErrors + " err / " + AssetScanController.summaryWarnings + " warn") : "Not run") }
                SummaryText { label: "Selection"; value: MeshValidator.hasSelection ? PropertiesPanelController.selectionName : "None" }
                SummaryText { label: "Mesh Findings"; value: root.issueSummary() }
                SummaryText { label: "Mesh Status"; value: MeshValidator.validating ? "Running" : (MeshValidator.validated ? "Ready" : "Idle") }
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

}
