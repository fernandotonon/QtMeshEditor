import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import PropertiesPanel 1.0
import AnimationControl 1.0
import EditorMode 1.0
import MaterialEditorQML 1.0

Rectangle {
    id: root
    color: PropertiesPanelController.panelColor

    readonly property int inspectorTab: EditorModeController.InspectorTab
    readonly property int sceneTab: EditorModeController.SceneTab
    readonly property int modeToolsTab: EditorModeController.ModeToolsTab
    readonly property int historyTab: EditorModeController.HistoryTab
    property int currentTab: 0
    property bool showAllModeTools: false
    property var bottomToolHost: null

    function revealBottomTool(toolId) {
        if (bottomToolHost && bottomToolHost.revealBottomTool)
            bottomToolHost.revealBottomTool(toolId)
    }

    function showModeToolsForMode(mode) {
        return EditorModeController.modeHasModeTools(mode)
    }

    function defaultTabForMode(mode) {
        return EditorModeController.defaultInspectorTabForMode(mode)
    }

    function shouldKeepExplicitTab(tab) {
        return EditorModeController.shouldKeepExplicitInspectorTab(tab)
    }

    function modeToolMatches(mode) {
        return EditorModeController.modeToolMatchesCurrentMode(
            mode, root.showAllModeTools, EditorModeController.currentMode)
    }

    function modeToolSectionVisible(mode, available) {
        return root.currentTab === root.modeToolsTab
            && available
            && root.modeToolMatches(mode)
    }

    function targetAccent(kind) {
        switch (kind) {
        case "node": return "#6ca0dc"
        case "editMesh": return "#55b65a"
        case "mesh": return "#55b65a"
        case "submesh": return "#c9b64f"
        case "mixed":
        case "mixedGeometry": return "#d18f3f"
        default: return PropertiesPanelController.borderColor
        }
    }

    Connections {
        target: EditorModeController
        function onModeChanged() {
            // Don't yank the user away from Scene (1) or History (3) when
            // they're explicitly browsing those tabs. Only retarget the
            // Inspector/Mode-Tools pair, which are the mode-aware ones.
            if (root.shouldKeepExplicitTab(root.currentTab))
                return
            root.currentTab = root.defaultTabForMode(EditorModeController.currentMode)
            // Switching modes resets the Mode-Tools filter back to "Current"
            // so each mode lands on its own tools by default. A sticky "All"
            // would silently survive a mode change and contradict the
            // "current-mode tools" default the filter advertises.
            root.showAllModeTools = false
        }
    }

    ScrollView {
        anchors.fill: parent
        clip: true

        Column {
            width: root.width
            spacing: 0

            // ---- Top-level Inspector Tabs ----
            Rectangle {
                width: parent.width
                height: 38
                color: PropertiesPanelController.headerColor

                Row {
                    anchors.fill: parent
                    anchors.margins: 5
                    spacing: 3

                    Repeater {
                        // Bind label order to the canonical InspectorTabId enum
                        // values exposed by EditorModeController. Don't rely on
                        // the Repeater's implicit `index` to map tabs — that
                        // would silently couple this array's order to the C++
                        // enum order and break every `sectionVisible` binding
                        // if anyone reordered either.
                        model: [
                            { label: "Inspector",  id: root.inspectorTab  },
                            { label: "Scene",      id: root.sceneTab      },
                            { label: "Mode Tools", id: root.modeToolsTab  },
                            { label: "History",    id: root.historyTab    }
                        ]

                        Rectangle {
                            width: Math.max(66, (parent.width - 9) / 4)
                            height: 28
                            radius: 4
                            color: root.currentTab === modelData.id
                                ? PropertiesPanelController.highlightColor
                                : tabMouse.containsMouse
                                  ? Qt.lighter(PropertiesPanelController.panelColor, 1.2)
                                  : PropertiesPanelController.panelColor
                            border.color: PropertiesPanelController.borderColor
                            border.width: 1

                            Text {
                                anchors.centerIn: parent
                                text: modelData.label
                                color: PropertiesPanelController.textColor
                                font.pixelSize: 10
                                font.bold: root.currentTab === modelData.id
                                elide: Text.ElideRight
                            }

                            MouseArea {
                                id: tabMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.currentTab = modelData.id
                            }
                        }
                    }
                }
            }

            Rectangle {
                width: parent.width
                height: visible ? 34 : 0
                visible: root.currentTab === root.modeToolsTab
                color: Qt.darker(PropertiesPanelController.headerColor, 1.08)

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    spacing: 6

                    Text {
                        Layout.fillWidth: true
                        text: EditorModeController.modeName + " Tools"
                        color: PropertiesPanelController.textColor
                        font.pixelSize: 11
                        font.bold: true
                        elide: Text.ElideRight
                    }

                    Repeater {
                        model: [
                            { label: "Current", all: false },
                            { label: "All", all: true }
                        ]

                        Rectangle {
                            width: Math.max(54, filterLabel.implicitWidth + 16)
                            height: 22
                            radius: 4
                            color: root.showAllModeTools === modelData.all
                                ? PropertiesPanelController.highlightColor
                                : filterMouse.containsMouse
                                  ? Qt.lighter(PropertiesPanelController.panelColor, 1.2)
                                  : PropertiesPanelController.panelColor
                            border.color: PropertiesPanelController.borderColor
                            border.width: 1

                            Text {
                                id: filterLabel
                                anchors.centerIn: parent
                                text: modelData.label
                                color: PropertiesPanelController.textColor
                                font.pixelSize: 10
                                font.bold: root.showAllModeTools === modelData.all
                            }

                            MouseArea {
                                id: filterMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.showAllModeTools = modelData.all
                            }
                        }
                    }
                }
            }

            // ---- Selection Target Indicator ----
            Rectangle {
                width: parent.width
                height: visible ? 58 : 0
                visible: PropertiesPanelController.hasSelection
                color: Qt.darker(PropertiesPanelController.panelColor, 1.08)
                border.color: root.targetAccent(PropertiesPanelController.transformTargetKind)
                border.width: 1

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 8

                    Rectangle {
                        width: 4
                        Layout.fillHeight: true
                        radius: 2
                        color: root.targetAccent(PropertiesPanelController.transformTargetKind)
                    }

                    ColumnLayout {
                        spacing: 2
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter

                        RowLayout {
                            spacing: 6
                            Layout.fillWidth: true

                            Text {
                                text: PropertiesPanelController.transformTargetLabel
                                color: PropertiesPanelController.textColor
                                font.pixelSize: 12
                                font.bold: true
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }

                            Rectangle {
                                radius: 3
                                color: root.targetAccent(PropertiesPanelController.transformTargetKind)
                                implicitWidth: impactLabel.implicitWidth + 10
                                implicitHeight: 18

                                Text {
                                    id: impactLabel
                                    anchors.centerIn: parent
                                    text: PropertiesPanelController.transformAffectsMesh ? "EXPORTS" : "PLACEMENT"
                                    color: "white"
                                    font.pixelSize: 9
                                    font.bold: true
                                }
                            }
                        }

                        Text {
                            text: PropertiesPanelController.transformTargetDetail
                            color: PropertiesPanelController.textColor
                            opacity: 0.78
                            font.pixelSize: 10
                            wrapMode: Text.WordWrap
                            maximumLineCount: 2
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                    }
                }
            }

            // ---- Edit Mode Tools ----
            CollapsibleSection {
                title: "Edit Mode Tools"
                sectionVisible: root.modeToolSectionVisible(
                    EditorModeController.EditMode,
                    EditModeController.editModeActive)
                expanded: true

                Component.onCompleted: content = editModeToolsComponent
            }

            // ---- Texture Paint (Material mode) ----
            CollapsibleSection {
                title: "Paint Brush"
                sectionVisible: root.modeToolSectionVisible(
                    EditorModeController.MaterialMode,
                    true)
                expanded: true

                Component.onCompleted: content = paintBrushComponent
            }

            CollapsibleSection {
                title: "Texture Paint"
                sectionVisible: root.modeToolSectionVisible(
                    EditorModeController.MaterialMode,
                    true)
                expanded: true

                Component.onCompleted: content = texturePaintComponent
            }

            CollapsibleSection {
                title: "Workspace Panels"
                sectionVisible: root.currentTab === root.modeToolsTab
                    && (root.showAllModeTools
                        || EditorModeController.currentMode === EditorModeController.AnimationMode
                        || EditorModeController.currentMode === EditorModeController.MaterialMode)
                expanded: false

                Component.onCompleted: content = workspacePanelsComponent
            }

            // ---- Scene Outliner ----
            CollapsibleSection {
                title: "Scene"
                sectionVisible: root.currentTab === root.sceneTab
                expanded: true

                Component.onCompleted: content = sceneOutlinerComponent
            }

            // ---- Transform ----
            CollapsibleSection {
                title: PropertiesPanelController.transformTargetLabel
                sectionVisible: root.currentTab === root.inspectorTab
                    && PropertiesPanelController.hasSelection

                Component.onCompleted: content = transformComponent
            }

            // ---- Snap Settings ----
            CollapsibleSection {
                title: "Snap Settings"
                sectionVisible: root.currentTab === root.inspectorTab
                expanded: false

                Component.onCompleted: content = snapSettingsComponent
            }

            // ---- Primitive Parameters ----
            CollapsibleSection {
                title: "Primitive: " + PropertiesPanelController.primitiveType
                sectionVisible: root.currentTab === root.inspectorTab
                    && PropertiesPanelController.hasPrimitive

                Component.onCompleted: content = primitiveComponent
            }

            // ---- Animations ----
            CollapsibleSection {
                title: "Animations"
                sectionVisible: root.modeToolSectionVisible(
                    EditorModeController.AnimationMode,
                    PropertiesPanelController.hasAnimations)

                Component.onCompleted: content = animationComponent
            }

            // ---- Animation Control (keyframe editor) ----
            CollapsibleSection {
                title: "Animation Control"
                sectionVisible: root.modeToolSectionVisible(
                    EditorModeController.AnimationMode,
                    AnimationControlController.hasAnimation)
                expanded: false

                Component.onCompleted: content = animControlComponent
            }

            // ---- LOD Generation ----
            CollapsibleSection {
                title: "LOD Generation"
                sectionVisible: root.modeToolSectionVisible(
                    EditorModeController.ObjectMode,
                    MeshLodController.hasSelection)
                expanded: true

                Component.onCompleted: content = lodComponent
            }

            // ---- Decimate (single-pass) ----
            CollapsibleSection {
                title: "Decimate (single-pass)"
                sectionVisible: root.modeToolSectionVisible(
                    EditorModeController.ObjectMode,
                    MeshDecimatorController.hasSelection)
                expanded: false

                Component.onCompleted: content = decimateComponent
            }

            // ---- Material Editor (Material mode) ----
            CollapsibleSection {
                title: "Material Editor"
                sectionVisible: root.modeToolSectionVisible(
                    EditorModeController.MaterialMode,
                    true)
                expanded: true

                Component.onCompleted: content = materialEditorToolComponent
            }

            // ---- Material Presets ----
            CollapsibleSection {
                title: "Material Presets"
                sectionVisible: root.modeToolSectionVisible(
                    EditorModeController.MaterialMode,
                    PropertiesPanelController.hasSelection)
                expanded: false

                Component.onCompleted: content = materialPresetsComponent
            }

            // ---- Mesh Validation ----
            CollapsibleSection {
                title: "Mesh Validation"
                sectionVisible: root.modeToolSectionVisible(
                    EditorModeController.ValidationMode,
                    MeshValidator.hasSelection)
                expanded: true

                Component.onCompleted: content = validationComponent
            }

            // ---- Undo History ----
            CollapsibleSection {
                title: "Undo History"
                sectionVisible: root.currentTab === root.historyTab
                expanded: true

                Component.onCompleted: content = undoHistoryComponent
            }
        }
    }

    // ---- Scene Outliner Content ----
    Component {
        id: sceneOutlinerComponent

        Column {
            id: outlinerColumn
            width: parent ? parent.width : 200

            property var treeModel: PropertiesPanelController.sceneTreeModel
            property int nodeCount: treeModel ? treeModel.rowCount() : 0
            property bool delegatesActive: true

            // Scene header — clear all, reparent to root
            RowLayout {
                width: outlinerColumn.width
                height: 24
                spacing: 6

                Text {
                    text: "\u25A1 Scene (Root)"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 11
                    font.bold: true
                    leftPadding: 4
                    Layout.alignment: Qt.AlignVCenter
                }

                Item { Layout.fillWidth: true; Layout.minimumHeight: 1 }

                Rectangle {
                    visible: outlinerColumn.nodeCount > 0
                    implicitWidth: 22
                    implicitHeight: 22
                    radius: 3
                    color: clearSceneMa.containsMouse ? PropertiesPanelController.highlightColor : PropertiesPanelController.headerColor
                    border.color: PropertiesPanelController.borderColor
                    border.width: 1
                    Layout.alignment: Qt.AlignVCenter

                    Text {
                        anchors.centerIn: parent
                        text: "\uD83D\uDDD1"
                        font.pixelSize: 13
                        color: PropertiesPanelController.textColor
                    }
                    MouseArea {
                        id: clearSceneMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: PropertiesPanelController.clearSceneTreeAllNodes()
                    }
                }

                Rectangle {
                    visible: PropertiesPanelController.selectionName !== "" &&
                             PropertiesPanelController.canReparentNode(PropertiesPanelController.selectionName, "root")
                    implicitWidth: toRootText.implicitWidth + 10
                    implicitHeight: 18
                    radius: 3
                    color: toRootMa.containsMouse ? PropertiesPanelController.highlightColor : PropertiesPanelController.headerColor
                    border.color: PropertiesPanelController.borderColor
                    border.width: 1
                    Layout.alignment: Qt.AlignVCenter
                    Text {
                        id: toRootText
                        anchors.centerIn: parent
                        text: "\u2191 to Root"
                        color: PropertiesPanelController.textColor
                        font.pixelSize: 9
                    }
                    MouseArea {
                        id: toRootMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: PropertiesPanelController.reparentNode(PropertiesPanelController.selectionName, "root")
                    }
                }
            }

            Row {
                width: outlinerColumn.width
                height: 26
                spacing: 6

                Rectangle {
                    width: Math.min(parent.width - 8, mergeAnimLabel.implicitWidth + 16)
                    height: 22
                    radius: 3
                    anchors.verticalCenter: parent.verticalCenter
                    opacity: PropertiesPanelController.mergeAnimationsEnabled ? 1.0 : 0.45
                    color: mergeAnimMa.containsMouse && PropertiesPanelController.mergeAnimationsEnabled
                        ? PropertiesPanelController.highlightColor
                        : PropertiesPanelController.headerColor
                    border.color: PropertiesPanelController.borderColor
                    border.width: 1

                    Text {
                        id: mergeAnimLabel
                        anchors.centerIn: parent
                        text: "Merge animations"
                        color: PropertiesPanelController.textColor
                        font.pixelSize: 10
                    }
                    MouseArea {
                        id: mergeAnimMa
                        anchors.fill: parent
                        hoverEnabled: true
                        enabled: PropertiesPanelController.mergeAnimationsEnabled
                        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ForbiddenCursor
                        onClicked: PropertiesPanelController.triggerMergeAnimations()
                    }
                }
            }

            Repeater {
                model: outlinerColumn.nodeCount

                Loader {
                    active: outlinerColumn.delegatesActive
                    width: outlinerColumn.width
                    source: "qrc:/PropertiesPanel/SceneTreeNode.qml"
                    onLoaded: {
                        item.nodeIndex = outlinerColumn.treeModel.index(index, 0)
                        item.treeModel = outlinerColumn.treeModel
                        item.indentLevel = 0
                        item.width = Qt.binding(function() { return outlinerColumn.width })
                        if (item.refreshSelected)
                            item.refreshSelected()
                    }
                }
            }

            Connections {
                target: outlinerColumn.treeModel
                function onModelReset() {
                    outlinerColumn.delegatesActive = false
                    outlinerColumn.nodeCount = outlinerColumn.treeModel
                        ? outlinerColumn.treeModel.rowCount() : 0
                    Qt.callLater(function() { outlinerColumn.delegatesActive = true })
                }
            }
        }
    }

    // ---- Edit Mode Tools Content ----
    Component {
        id: editModeToolsComponent

        Column {
            id: editToolsCol
            width: parent ? parent.width : 200
            padding: 8
            spacing: 6

            property int activeSelMode: EditModeController.selectionMode
            property int activeFalloff: EditModeController.softSelectionFalloff
            property int activeNormals: EditModeController.normalsMode
            property bool softSelOn: EditModeController.softSelectionEnabled
            property bool wireframeOn: EditModeController.wireframeEnabled
            property bool vertexPreviewOn: EditModeController.vertexColorPreviewEnabled

            Connections {
                target: EditModeController
                function onSelectionModeChanged() { editToolsCol.activeSelMode = EditModeController.selectionMode }
                function onSoftSelectionFalloffChanged() { editToolsCol.activeFalloff = EditModeController.softSelectionFalloff }
                function onNormalsModeChanged() { editToolsCol.activeNormals = EditModeController.normalsMode }
                function onSoftSelectionEnabledChanged() { editToolsCol.softSelOn = EditModeController.softSelectionEnabled }
                function onWireframeChanged() { editToolsCol.wireframeOn = EditModeController.wireframeEnabled }
                function onVertexColorPreviewChanged() { editToolsCol.vertexPreviewOn = EditModeController.vertexColorPreviewEnabled }
            }

            // Selection mode buttons
            Row {
                spacing: 4
                width: parent.width - 16

                Text {
                    text: "Select:"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 11
                    anchors.verticalCenter: parent.verticalCenter
                }

                Repeater {
                    model: [
                        { label: "Vertex", mode: 0 },
                        { label: "Edge", mode: 1 },
                        { label: "Face", mode: 2 }
                    ]

                    Rectangle {
                        width: 52; height: 22; radius: 3
                        color: editToolsCol.activeSelMode === modelData.mode
                            ? PropertiesPanelController.highlightColor
                            : selMa.containsMouse ? Qt.lighter(PropertiesPanelController.panelColor, 1.5)
                            : PropertiesPanelController.headerColor
                        border.color: PropertiesPanelController.borderColor; border.width: 1
                        Behavior on color { ColorAnimation { duration: 50 } }

                        Text {
                            anchors.centerIn: parent
                            text: modelData.label
                            color: PropertiesPanelController.textColor
                            font.pixelSize: 10
                        }
                        MouseArea {
                            id: selMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                            onClicked: { editToolsCol.activeSelMode = modelData.mode; EditModeController.selectionMode = modelData.mode }
                        }
                    }
                }
            }

            // Selected count display
            Text {
                text: "Selected: " + EditModeController.selectedVertexCount + " vertices, "
                    + EditModeController.selectedEdgeCount + " edges, "
                    + EditModeController.selectedFaceCount + " faces"
                color: PropertiesPanelController.textColor
                font.pixelSize: 10; opacity: 0.8
                width: parent.width - 16
                wrapMode: Text.Wrap
            }

            // Soft selection toggle (themed checkbox)
            Row {
                spacing: 6; width: parent.width - 16

                Rectangle {
                    width: 14; height: 14; anchors.verticalCenter: parent.verticalCenter
                    border.color: PropertiesPanelController.borderColor; border.width: 1; radius: 2
                    color: editToolsCol.softSelOn ? PropertiesPanelController.highlightColor : PropertiesPanelController.controlBgColor
                    Behavior on color { ColorAnimation { duration: 50 } }
                    Text { anchors.centerIn: parent; text: editToolsCol.softSelOn ? "\u2713" : ""; color: "white"; font.pixelSize: 10 }
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                        onClicked: { editToolsCol.softSelOn = !editToolsCol.softSelOn; EditModeController.softSelectionEnabled = editToolsCol.softSelOn }
                    }
                }
                Text { text: "Soft Selection"; font.pixelSize: 11; color: PropertiesPanelController.textColor; anchors.verticalCenter: parent.verticalCenter }
            }

            // Soft selection radius slider
            Column {
                width: parent.width - 16
                visible: EditModeController.softSelectionEnabled
                spacing: 2

                Text {
                    // Display the actual radius (log-mapped from slider)
                    property real logRadius: {
                        var minVal = 0.01; var maxVal = 20.0;
                        return minVal * Math.pow(maxVal / minVal, softRadiusSlider.value);
                    }
                    text: "Radius: " + logRadius.toFixed(logRadius < 1 ? 3 : 2)
                    color: PropertiesPanelController.textColor; font.pixelSize: 10
                }
                Slider {
                    id: softRadiusSlider
                    width: parent.width
                    from: 0.0; to: 1.0; stepSize: 0.005
                    // Convert current radius to normalized log position
                    property real minVal: 0.01
                    property real maxVal: 20.0
                    property bool updating: false
                    Component.onCompleted: {
                        updating = true;
                        value = Math.log(EditModeController.softSelectionRadius / minVal) / Math.log(maxVal / minVal);
                        updating = false;
                    }
                    onValueChanged: {
                        if (!updating) {
                            var radius = minVal * Math.pow(maxVal / minVal, value);
                            EditModeController.softSelectionRadius = radius;
                        }
                    }
                    Connections {
                        target: EditModeController
                        function onSoftSelectionChanged() {
                            softRadiusSlider.updating = true;
                            softRadiusSlider.value = Math.log(EditModeController.softSelectionRadius / softRadiusSlider.minVal) / Math.log(softRadiusSlider.maxVal / softRadiusSlider.minVal);
                            softRadiusSlider.updating = false;
                        }
                    }
                }

                // Falloff mode
                Row {
                    spacing: 4

                    Text {
                        text: "Falloff:"
                        color: PropertiesPanelController.textColor; font.pixelSize: 10
                        anchors.verticalCenter: parent.verticalCenter
                    }

                    Repeater {
                        model: [
                            { label: "Linear", mode: 0 },
                            { label: "Smooth", mode: 1 }
                        ]

                        Rectangle {
                            width: 50; height: 20; radius: 3
                            color: editToolsCol.activeFalloff === modelData.mode
                                ? PropertiesPanelController.highlightColor
                                : fallMa.containsMouse ? Qt.lighter(PropertiesPanelController.panelColor, 1.5)
                                : PropertiesPanelController.headerColor
                            border.color: PropertiesPanelController.borderColor; border.width: 1
                            Behavior on color { ColorAnimation { duration: 50 } }

                            Text {
                                anchors.centerIn: parent
                                text: modelData.label
                                color: PropertiesPanelController.textColor; font.pixelSize: 9
                            }
                            MouseArea {
                                id: fallMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                onClicked: { editToolsCol.activeFalloff = modelData.mode; EditModeController.softSelectionFalloff = modelData.mode }
                            }
                        }
                    }
                }
            }

            // Wireframe toggle (themed checkbox)
            Row {
                spacing: 6; width: parent.width - 16

                Rectangle {
                    width: 14; height: 14; anchors.verticalCenter: parent.verticalCenter
                    border.color: PropertiesPanelController.borderColor; border.width: 1; radius: 2
                    color: editToolsCol.wireframeOn ? PropertiesPanelController.highlightColor : PropertiesPanelController.controlBgColor
                    Behavior on color { ColorAnimation { duration: 50 } }
                    Text { anchors.centerIn: parent; text: editToolsCol.wireframeOn ? "\u2713" : ""; color: "white"; font.pixelSize: 10 }
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                        onClicked: { editToolsCol.wireframeOn = !editToolsCol.wireframeOn; EditModeController.wireframeEnabled = editToolsCol.wireframeOn }
                    }
                }
                Text { text: "Wireframe"; font.pixelSize: 11; color: PropertiesPanelController.textColor; anchors.verticalCenter: parent.verticalCenter }
            }

            // Vertex color preview toggle (keep in Inspector; brush settings live on the toolbar button).
            Row {
                spacing: 6
                width: parent.width - 16

                Rectangle {
                    width: 14; height: 14; anchors.verticalCenter: parent.verticalCenter
                    border.color: PropertiesPanelController.borderColor; border.width: 1; radius: 2
                    color: editToolsCol.vertexPreviewOn ? PropertiesPanelController.highlightColor : PropertiesPanelController.controlBgColor
                    Behavior on color { ColorAnimation { duration: 50 } }
                    Text { anchors.centerIn: parent; text: editToolsCol.vertexPreviewOn ? "\u2713" : ""; color: "white"; font.pixelSize: 10 }
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                        onClicked: { editToolsCol.vertexPreviewOn = !editToolsCol.vertexPreviewOn; EditModeController.vertexColorPreviewEnabled = editToolsCol.vertexPreviewOn }
                    }
                }
                Text { text: "Vertex Color Preview"; font.pixelSize: 11; color: PropertiesPanelController.textColor; anchors.verticalCenter: parent.verticalCenter }
            }

            // Separator
            Rectangle { width: parent.width - 16; height: 1; color: PropertiesPanelController.borderColor }

            // Normals recalculation
            Row {
                spacing: 4; width: parent.width - 16

                Text {
                    text: "Normals:"
                    color: PropertiesPanelController.textColor; font.pixelSize: 11
                    anchors.verticalCenter: parent.verticalCenter
                }

                Repeater {
                    model: [
                        { label: "Smooth", mode: 0 },
                        { label: "Flat", mode: 1 }
                    ]

                    Rectangle {
                        width: 52; height: 22; radius: 3
                        color: editToolsCol.activeNormals === modelData.mode
                            ? PropertiesPanelController.highlightColor
                            : normMa.containsMouse ? Qt.lighter(PropertiesPanelController.panelColor, 1.5)
                            : PropertiesPanelController.headerColor
                        border.color: PropertiesPanelController.borderColor; border.width: 1
                        Behavior on color { ColorAnimation { duration: 50 } }

                        Text {
                            anchors.centerIn: parent
                            text: modelData.label
                            color: PropertiesPanelController.textColor; font.pixelSize: 10
                        }
                        MouseArea {
                            id: normMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                            onClicked: { editToolsCol.activeNormals = modelData.mode; EditModeController.normalsMode = modelData.mode }
                        }
                    }
                }
            }

            // Recalc Normals button
            Rectangle {
                width: parent.width - 16; height: 26; radius: 3
                color: recalcMouse.pressed ? Qt.darker(PropertiesPanelController.highlightColor, 1.2)
                     : recalcMouse.containsMouse ? Qt.lighter(PropertiesPanelController.highlightColor, 1.1)
                     : PropertiesPanelController.highlightColor
                Text { anchors.centerIn: parent; text: "Recalculate Normals"; color: "white"; font.pixelSize: 11 }
                MouseArea {
                    id: recalcMouse; anchors.fill: parent; hoverEnabled: true
                    onClicked: EditModeController.recalculateNormals(EditModeController.normalsMode === 0)
                }
            }

            // Topology triggers (Extrude / Bevel) live on the main
            // objects toolbar now — see mainwindow.cpp. The Inspector
            // keeps only the operation parameters below (bevel session
            // segments + profile) which are only relevant mid-drag.

            // Bevel session controls (visible only while a bevel session is
            // active — i.e., between Cmd+B and the commit/cancel click).
            // Lets the user tweak segment count and profile shape while the
            // gizmo is up.
            Column {
                visible: EditModeController.bevelSessionActiveValue
                width: parent.width - 16
                spacing: 4

                // Segments — subdivides the chamfer/cap along a profile
                // curve. Works in both edge and vertex modes.
                Row {
                    width: parent.width
                    spacing: 6
                    Text {
                        text: "Segments"
                        color: PropertiesPanelController.textColor
                        font.pixelSize: 11
                        anchors.verticalCenter: parent.verticalCenter
                        width: 60
                    }
                    SpinBox {
                        id: bevelSegmentsSpin
                        from: 1
                        to: 16
                        value: EditModeController.bevelSegmentsValue
                        onValueModified: EditModeController.updateBevelSegments(value)
                        width: parent.width - 70
                    }
                }

                // Profile shape — only meaningful with segments > 1.
                Column {
                    visible: EditModeController.bevelSegmentsValue > 1
                    width: parent.width
                    spacing: 4

                    Text {
                        text: "Profile"
                        color: PropertiesPanelController.textColor
                        font.pixelSize: 11
                    }
                    ProfileGraph {
                        id: bevelProfileGraph
                        width: parent.width
                        height: 100
                        values: EditModeController.bevelProfilePointsList
                        onPointChanged: (idx, v) => EditModeController.updateBevelProfilePoint(idx, v)
                        onResetRequested: EditModeController.resetBevelProfile()
                    }
                    Text {
                        text: "Drag a dot · double-click to reset"
                        color: PropertiesPanelController.subtleTextColor !== undefined
                             ? PropertiesPanelController.subtleTextColor
                             : "#888"
                        font.pixelSize: 9
                        width: parent.width
                        wrapMode: Text.Wrap
                    }
                }
            }

            // Separator
            Rectangle { width: parent.width - 16; height: 1; color: PropertiesPanelController.borderColor }

            // Mesh validation warnings
            Text {
                width: parent.width - 16
                visible: EditModeController.hasValidationWarnings
                text: "\u26A0 " + EditModeController.degenerateTriangleCount + " degenerate triangle(s)"
                color: "#e0a030"; font.pixelSize: 11; font.bold: true
                wrapMode: Text.Wrap
            }

            // Remove degenerates button
            Rectangle {
                width: parent.width - 16; height: 26; radius: 3
                visible: EditModeController.hasValidationWarnings
                color: removeDegMouse.pressed ? Qt.darker("#c04040", 1.2)
                     : removeDegMouse.containsMouse ? Qt.lighter("#c04040", 1.2)
                     : "#c04040"
                Text { anchors.centerIn: parent; text: "Remove Degenerates"; color: "white"; font.pixelSize: 11 }
                MouseArea {
                    id: removeDegMouse; anchors.fill: parent; hoverEnabled: true
                    onClicked: EditModeController.removeDegenerateTriangles()
                }
            }

            // Validation OK
            Text {
                width: parent.width - 16
                visible: EditModeController.editModeActive && !EditModeController.hasValidationWarnings
                text: "\u2714 No degenerate triangles"
                color: "#60c060"; font.pixelSize: 10
            }
        }
    }

    // ---- Paint Brush Content (shared by vertex paint + texture paint) ----
    Component {
        id: paintBrushComponent

        Column {
            id: brushCol
            width: parent ? parent.width : 200
            padding: 8
            spacing: 6

            property color brushColor: TexturePaintController.texturePaintColor
            property real brushRadius: TexturePaintController.texturePaintRadius
            property real brushStrength: TexturePaintController.texturePaintStrength
            property real brushFalloff: TexturePaintController.texturePaintFalloff

            Connections {
                target: TexturePaintController
                function onTexturePaintChanged() {
                    brushCol.brushColor = TexturePaintController.texturePaintColor
                    brushCol.brushRadius = TexturePaintController.texturePaintRadius
                    brushCol.brushStrength = TexturePaintController.texturePaintStrength
                    brushCol.brushFalloff = TexturePaintController.texturePaintFalloff
                }
            }

            Text {
                width: parent.width - 16
                text: "Shared brush settings for Vertex Paint and Texture Paint. " +
                      "The toolbar Vertex Paint popup uses these same values."
                color: PropertiesPanelController.textColor
                font.pixelSize: 10; opacity: 0.7
                wrapMode: Text.Wrap
            }

            // Color picker (native dialog)
            Row {
                spacing: 6
                Text {
                    text: "Color"
                    color: PropertiesPanelController.textColor; font.pixelSize: 11
                    anchors.verticalCenter: parent.verticalCenter
                    width: 70
                }
                Rectangle {
                    width: 28; height: 22; radius: 3
                    color: brushCol.brushColor
                    border.color: PropertiesPanelController.borderColor; border.width: 1
                    MouseArea {
                        anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                        onClicked: TexturePaintController.pickBrushColorInteractive()
                    }
                }
            }

            // Radius slider
            Row {
                spacing: 6
                Text {
                    text: "Radius"; width: 70
                    color: PropertiesPanelController.textColor; font.pixelSize: 11
                    anchors.verticalCenter: parent.verticalCenter
                }
                Slider {
                    width: 140
                    from: 0.02; to: 2.0; stepSize: 0.01
                    value: brushCol.brushRadius
                    onMoved: TexturePaintController.setBrushRadius(value)
                }
                Text {
                    text: brushCol.brushRadius.toFixed(2)
                    color: PropertiesPanelController.textColor; font.pixelSize: 10
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            // Strength slider
            Row {
                spacing: 6
                Text {
                    text: "Strength"; width: 70
                    color: PropertiesPanelController.textColor; font.pixelSize: 11
                    anchors.verticalCenter: parent.verticalCenter
                }
                Slider {
                    width: 140
                    from: 0.0; to: 1.0; stepSize: 0.01
                    value: brushCol.brushStrength
                    onMoved: TexturePaintController.setBrushStrength(value)
                }
                Text {
                    text: brushCol.brushStrength.toFixed(2)
                    color: PropertiesPanelController.textColor; font.pixelSize: 10
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            // Falloff slider
            Row {
                spacing: 6
                Text {
                    text: "Falloff"; width: 70
                    color: PropertiesPanelController.textColor; font.pixelSize: 11
                    anchors.verticalCenter: parent.verticalCenter
                }
                Slider {
                    width: 140
                    from: 0.0; to: 1.0; stepSize: 0.01
                    value: brushCol.brushFalloff
                    onMoved: TexturePaintController.setBrushFalloff(value)
                }
                Text {
                    text: brushCol.brushFalloff.toFixed(2)
                    color: PropertiesPanelController.textColor; font.pixelSize: 10
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
        }
    }

    // ---- Texture Paint Content ----
    Component {
        id: texturePaintComponent

        Column {
            id: texPaintCol
            width: parent ? parent.width : 200
            padding: 8
            spacing: 8

            property bool paintOn: TexturePaintController.texturePaintEnabled
            property bool hasSession: TexturePaintController.hasActiveSession
            property int sessionRes: TexturePaintController.textureResolution
            property var slots: TexturePaintController.textureSlots
            property int activeSlot: TexturePaintController.activeSlotIndex
            property int brushTool: TexturePaintController.brushTool
            property string previewUri: TexturePaintController.previewDataUri
            // Live hover position in UV space, fed by hoveredUVChanged.
            property real hoverU: -1
            property real hoverV: -1

            Connections {
                target: TexturePaintController
                function onTexturePaintChanged() {
                    texPaintCol.paintOn = TexturePaintController.texturePaintEnabled
                }
                function onSessionChanged() {
                    texPaintCol.hasSession = TexturePaintController.hasActiveSession
                    texPaintCol.sessionRes = TexturePaintController.textureResolution
                }
                function onSlotsChanged() {
                    texPaintCol.slots = TexturePaintController.textureSlots
                    texPaintCol.activeSlot = TexturePaintController.activeSlotIndex
                }
                function onPreviewChanged() {
                    texPaintCol.previewUri = TexturePaintController.previewDataUri
                }
                function onBrushToolChanged() {
                    texPaintCol.brushTool = TexturePaintController.brushTool
                }
                function onHoveredUVChanged(u, v) {
                    texPaintCol.hoverU = u
                    texPaintCol.hoverV = v
                }
            }

            Text {
                width: parent.width - 16
                text: "Paint directly into a BaseColor texture. " +
                      "The brush color/radius/strength/falloff comes from the " +
                      "Paint Brush section above (same brush used by the toolbar " +
                      "Vertex Paint popup)."
                color: PropertiesPanelController.textColor
                font.pixelSize: 10; opacity: 0.7
                wrapMode: Text.Wrap
            }

            // Enable toggle
            Row {
                spacing: 6
                Rectangle {
                    width: 18; height: 18; radius: 3
                    color: texPaintCol.paintOn ? PropertiesPanelController.highlightColor : PropertiesPanelController.controlBgColor
                    border.color: PropertiesPanelController.borderColor; border.width: 1
                    Text { anchors.centerIn: parent; text: texPaintCol.paintOn ? "\u2713" : ""; color: "white"; font.pixelSize: 10 }
                    MouseArea {
                        anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                        onClicked: { TexturePaintController.texturePaintEnabled = !texPaintCol.paintOn }
                    }
                }
                Text {
                    text: "Enable paint mode"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 11
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            // Paint target selector \u2014 Texture vs Vertex
            Row {
                spacing: 4
                Text {
                    text: "Target:"
                    color: PropertiesPanelController.textColor; font.pixelSize: 11
                    anchors.verticalCenter: parent.verticalCenter
                    width: 50
                }
                Repeater {
                    model: [
                        { target: 0, label: "Texture" },
                        { target: 1, label: "Vertex" }
                    ]
                    Rectangle {
                        width: 70; height: 24; radius: 3
                        color: TexturePaintController.paintTarget === modelData.target
                            ? PropertiesPanelController.highlightColor
                            : (tgtMa.containsMouse ? Qt.lighter(PropertiesPanelController.panelColor, 1.5)
                                                   : PropertiesPanelController.headerColor)
                        border.color: PropertiesPanelController.borderColor; border.width: 1
                        Text { anchors.centerIn: parent; text: modelData.label
                            color: PropertiesPanelController.textColor; font.pixelSize: 10 }
                        MouseArea {
                            id: tgtMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                            onClicked: TexturePaintController.paintTarget = modelData.target
                        }
                    }
                }
            }

            // Tool selector \u2014 Paint, Erase, Fill, Picker, Smudge
            Row {
                spacing: 4
                Repeater {
                    model: [
                        { tool: 0, label: "Paint", glyph: "\u270f" },
                        { tool: 1, label: "Erase", glyph: "\u232b" },
                        { tool: 2, label: "Fill",  glyph: "\u29c9" },
                        { tool: 3, label: "Pick",  glyph: "\u22b0" },
                        { tool: 4, label: "Smudge", glyph: "\u223f" }
                    ]
                    Rectangle {
                        width: 52; height: 26; radius: 3
                        color: texPaintCol.brushTool === modelData.tool
                            ? PropertiesPanelController.highlightColor
                            : (toolMa.containsMouse ? Qt.lighter(PropertiesPanelController.panelColor, 1.5)
                                                    : PropertiesPanelController.headerColor)
                        border.color: PropertiesPanelController.borderColor; border.width: 1
                        Text {
                            anchors.centerIn: parent
                            text: modelData.glyph + " " + modelData.label
                            color: PropertiesPanelController.textColor
                            font.pixelSize: 10
                        }
                        MouseArea {
                            id: toolMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                            onClicked: TexturePaintController.brushTool = modelData.tool
                        }
                    }
                }
            }

            // Texture slot picker \u2014 populated by selection
            Row {
                spacing: 6
                width: parent.width - 16
                Text {
                    text: "Slot:"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 11
                    anchors.verticalCenter: parent.verticalCenter
                    width: 36
                }
                ThemedComboBox {
                    id: slotCombo
                    width: 180
                    enabled: texPaintCol.slots.length > 0
                    model: {
                        const labels = []
                        for (let i = 0; i < texPaintCol.slots.length; ++i)
                            labels.push(texPaintCol.slots[i].label || ("slot " + i))
                        return labels.length === 0 ? ["(no texture slots \u2014 select a mesh)"] : labels
                    }
                    currentIndex: Math.max(0, texPaintCol.activeSlot)
                    onActivated: function(index) {
                        if (texPaintCol.slots.length > 0)
                            TexturePaintController.activeSlotIndex = index
                    }
                }
                // UV island overlay toggle
                Rectangle {
                    width: 18; height: 18; radius: 3
                    color: TexturePaintController.uvOverlayVisible
                        ? PropertiesPanelController.highlightColor
                        : PropertiesPanelController.controlBgColor
                    border.color: PropertiesPanelController.borderColor; border.width: 1
                    anchors.verticalCenter: parent.verticalCenter
                    Text {
                        anchors.centerIn: parent
                        text: TexturePaintController.uvOverlayVisible ? "\u2713" : ""
                        color: "white"; font.pixelSize: 10
                    }
                    MouseArea {
                        anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                        onClicked: TexturePaintController.uvOverlayVisible = !TexturePaintController.uvOverlayVisible
                    }
                }
                Text {
                    text: "UV"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 11
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            // Session info
            Text {
                width: parent.width - 16
                text: texPaintCol.hasSession
                    ? ("Active texture: " + texPaintCol.sessionRes + "\u00d7" + texPaintCol.sessionRes)
                    : "No texture session \u2014 enable paint or click \"Create / Attach Texture\"."
                color: texPaintCol.hasSession ? "#60c060" : PropertiesPanelController.textColor
                font.pixelSize: 10
                opacity: texPaintCol.hasSession ? 1.0 : 0.7
                wrapMode: Text.Wrap
            }

            // ---- 2D preview / paint surface ----
            // Live image of the paint buffer; clicking and dragging
            // paints into the texture in UV space. Crosshair indicator
            // mirrors the 3D-mesh hover position.
            Rectangle {
                width: 256; height: 256
                color: "#222"
                border.color: PropertiesPanelController.borderColor
                border.width: 1
                visible: texPaintCol.hasSession

                Image {
                    id: previewImg
                    anchors.fill: parent
                    anchors.margins: 1
                    source: texPaintCol.previewUri
                    fillMode: Image.PreserveAspectFit
                    smooth: false
                    cache: false
                    // Bust the cache when source string changes
                    onSourceChanged: previewImg.update()
                }
                // UV-island wireframe overlay (toggleable).
                Image {
                    id: uvOverlayImg
                    anchors.fill: parent
                    anchors.margins: 1
                    visible: TexturePaintController.uvOverlayVisible
                    opacity: 0.7
                    source: TexturePaintController.uvOverlayDataUri
                    fillMode: Image.PreserveAspectFit
                    smooth: false
                    cache: false
                }

                // Crosshair indicator at hover UV
                Rectangle {
                    visible: texPaintCol.hoverU >= 0 && texPaintCol.hoverV >= 0
                    width: 1; height: parent.height - 2
                    color: "#ff3030"
                    x: 1 + Math.round(texPaintCol.hoverU * (parent.width - 2))
                    y: 1
                }
                Rectangle {
                    visible: texPaintCol.hoverU >= 0 && texPaintCol.hoverV >= 0
                    width: parent.width - 2; height: 1
                    color: "#ff3030"
                    x: 1
                    y: 1 + Math.round(texPaintCol.hoverV * (parent.height - 2))
                }

                MouseArea {
                    id: paintArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.CrossCursor
                    property bool dragging: false

                    function toUV(mx, my) {
                        const W = paintArea.width
                        const H = paintArea.height
                        return Qt.point(Math.max(0, Math.min(1, mx / W)),
                                        Math.max(0, Math.min(1, my / H)))
                    }
                    onPositionChanged: function(m) {
                        const uv = toUV(m.x, m.y)
                        if (dragging) {
                            TexturePaintController.updateStrokeUV(uv.x, uv.y)
                        } else {
                            TexturePaintController.setHoveredUV(uv.x, uv.y)
                        }
                    }
                    onExited: TexturePaintController.clearHoveredUV()
                    onPressed: function(m) {
                        if (m.button !== Qt.LeftButton) return
                        const uv = toUV(m.x, m.y)
                        if (TexturePaintController.beginStrokeUV(uv.x, uv.y))
                            dragging = true
                    }
                    onReleased: function(m) {
                        if (dragging) {
                            TexturePaintController.endStrokeUV()
                            dragging = false
                        }
                    }
                }
            }

            // Action row 1: create, save, load
            Flow {
                width: parent.width - 16
                spacing: 4

                // Resolution picker for fresh textures.
                ThemedComboBox {
                    id: resCombo
                    width: 80
                    model: ["256", "512", "1024", "2048", "4096"]
                    currentIndex: 2  // 1024
                }

                Rectangle {
                    width: 130; height: 24; radius: 3
                    color: createMa.containsMouse ? Qt.lighter(PropertiesPanelController.panelColor, 1.5) : PropertiesPanelController.headerColor
                    border.color: PropertiesPanelController.borderColor; border.width: 1
                    Text { anchors.centerIn: parent; text: "Create / Attach Texture"; color: PropertiesPanelController.textColor; font.pixelSize: 10 }
                    MouseArea {
                        id: createMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            const res = parseInt(resCombo.model[resCombo.currentIndex])
                            TexturePaintController.ensurePaintableTexture(res)
                        }
                    }
                }

                Rectangle {
                    width: 70; height: 24; radius: 3
                    color: saveMa.containsMouse ? Qt.lighter(PropertiesPanelController.panelColor, 1.5) : PropertiesPanelController.headerColor
                    border.color: PropertiesPanelController.borderColor; border.width: 1
                    opacity: texPaintCol.hasSession ? 1.0 : 0.4
                    Text { anchors.centerIn: parent; text: "Save\u2026"; color: PropertiesPanelController.textColor; font.pixelSize: 10 }
                    MouseArea {
                        id: saveMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                        enabled: texPaintCol.hasSession
                        onClicked: TexturePaintController.savePaintBufferInteractive()
                    }
                }

                Rectangle {
                    width: 70; height: 24; radius: 3
                    color: loadMa.containsMouse ? Qt.lighter(PropertiesPanelController.panelColor, 1.5) : PropertiesPanelController.headerColor
                    border.color: PropertiesPanelController.borderColor; border.width: 1
                    Text { anchors.centerIn: parent; text: "Load\u2026"; color: PropertiesPanelController.textColor; font.pixelSize: 10 }
                    MouseArea {
                        id: loadMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                        onClicked: TexturePaintController.loadPaintBufferInteractive()
                    }
                }
            }

            // Action row 2: bake vertex colors
            Flow {
                width: parent.width - 16
                spacing: 4

                Rectangle {
                    width: 200; height: 24; radius: 3
                    color: bakeMa.containsMouse ? Qt.lighter(PropertiesPanelController.panelColor, 1.5) : PropertiesPanelController.headerColor
                    border.color: PropertiesPanelController.borderColor; border.width: 1
                    Text { anchors.centerIn: parent; text: "Bake Vertex Colors \u2192 Texture"; color: PropertiesPanelController.textColor; font.pixelSize: 10 }
                    MouseArea {
                        id: bakeMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            const res = parseInt(resCombo.model[resCombo.currentIndex])
                            TexturePaintController.bakeVertexColorsToTexture(res, 4, "")
                        }
                    }
                }
            }
        }
    }

    Component {
        id: workspacePanelsComponent

        Column {
            width: parent ? parent.width : 200
            padding: 8
            spacing: 6

            Flow {
                width: parent.width - 16
                spacing: 6

                ModeToolShortcutButton {
                    objectName: "workspaceAssetBrowserButton"
                    visible: root.showAllModeTools
                        || EditorModeController.currentMode === EditorModeController.MaterialMode
                    text: "Asset Browser"
                    onClicked: root.revealBottomTool("assetBrowser")
                }

                ModeToolShortcutButton {
                    objectName: "workspaceDopeSheetButton"
                    visible: root.showAllModeTools
                        || EditorModeController.currentMode === EditorModeController.AnimationMode
                    text: "Dope Sheet"
                    onClicked: root.revealBottomTool("dopeSheet")
                }

                ModeToolShortcutButton {
                    objectName: "workspaceCurveEditorButton"
                    visible: root.showAllModeTools
                        || EditorModeController.currentMode === EditorModeController.AnimationMode
                    text: "Curve Editor"
                    onClicked: root.revealBottomTool("curveEditor")
                }
            }
        }
    }

    // ---- Transform Content ----
    Component {
        id: transformComponent

        Column {
            width: parent ? parent.width : 200
            padding: 8
            spacing: 6

            // Position
            Text {
                text: "Position"
                color: PropertiesPanelController.textColor
                font.pixelSize: 11
                font.bold: true
            }
            Row {
                spacing: 4
                width: parent.width - 16

                TransformField { label: "X"; value: PropertiesPanelController.posX; color: "#c04040"
                    onNewValue: function(val) { PropertiesPanelController.posX = val } }
                TransformField { label: "Y"; value: PropertiesPanelController.posY; color: "#40c040"
                    onNewValue: function(val) { PropertiesPanelController.posY = val } }
                TransformField { label: "Z"; value: PropertiesPanelController.posZ; color: "#4040c0"
                    onNewValue: function(val) { PropertiesPanelController.posZ = val } }
            }

            // Rotation
            Text {
                text: "Rotation"
                color: PropertiesPanelController.textColor
                font.pixelSize: 11
                font.bold: true
            }
            Row {
                spacing: 4
                width: parent.width - 16

                TransformField { label: "X"; value: PropertiesPanelController.rotX; color: "#c04040"
                    onNewValue: function(val) { PropertiesPanelController.rotX = val } }
                TransformField { label: "Y"; value: PropertiesPanelController.rotY; color: "#40c040"
                    onNewValue: function(val) { PropertiesPanelController.rotY = val } }
                TransformField { label: "Z"; value: PropertiesPanelController.rotZ; color: "#4040c0"
                    onNewValue: function(val) { PropertiesPanelController.rotZ = val } }
            }

            // Scale
            Text {
                text: "Scale"
                color: PropertiesPanelController.textColor
                font.pixelSize: 11
                font.bold: true
            }
            Row {
                spacing: 4
                width: parent.width - 16

                TransformField { label: "X"; value: PropertiesPanelController.scaleX; color: "#c04040"
                    onNewValue: function(val) { PropertiesPanelController.scaleX = val } }
                TransformField { label: "Y"; value: PropertiesPanelController.scaleY; color: "#40c040"
                    onNewValue: function(val) { PropertiesPanelController.scaleY = val } }
                TransformField { label: "Z"; value: PropertiesPanelController.scaleZ; color: "#4040c0"
                    onNewValue: function(val) { PropertiesPanelController.scaleZ = val } }
            }

            // Pivot Point
            Text {
                text: "Pivot Point (P)"
                color: PropertiesPanelController.textColor
                font.pixelSize: 11
                font.bold: true
                topPadding: 4
            }
            Row {
                spacing: 4
                width: parent.width - 16

                property int activePivot: PropertiesPanelController.pivotMode

                Repeater {
                    model: [
                        { label: "Center", mode: 0 },
                        { label: "Bottom", mode: 1 },
                        { label: "Origin", mode: 2 }
                    ]
                    delegate: Rectangle {
                        required property var modelData
                        required property int index
                        width: (parent.width - 8) / 3
                        height: 24
                        radius: 3
                        color: PropertiesPanelController.pivotMode === modelData.mode
                            ? PropertiesPanelController.highlightColor
                            : PropertiesPanelController.inputColor
                        border.width: 1
                        border.color: PropertiesPanelController.borderColor

                        Text {
                            anchors.centerIn: parent
                            text: modelData.label
                            color: PropertiesPanelController.textColor
                            font.pixelSize: 10
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: PropertiesPanelController.pivotMode = modelData.mode
                        }
                    }
                }
            }
        }
    }

    component ModeToolShortcutButton: Button {
        id: shortcutButton
        implicitHeight: 24
        implicitWidth: Math.max(92, contentItem.implicitWidth + 18)
        font.pixelSize: 10

        background: Rectangle {
            radius: 3
            color: shortcutButton.down
                ? Qt.darker(PropertiesPanelController.highlightColor, 1.1)
                : shortcutButton.hovered
                    ? Qt.lighter(PropertiesPanelController.inputColor, 1.08)
                    : PropertiesPanelController.inputColor
            border.width: 1
            border.color: PropertiesPanelController.borderColor
        }

        contentItem: Text {
            text: shortcutButton.text
            color: PropertiesPanelController.textColor
            font: shortcutButton.font
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
    }

    // ---- Snap Settings Content ----
    Component {
        id: snapSettingsComponent

        Column {
            id: snapCol
            width: parent ? parent.width : 200
            padding: 8
            spacing: 6

            property real btnAreaWidth: snapCol.width - 16

            // Active indices — pure QML properties, guaranteed reactive in delegates
            property int activeGridIdx: -1
            property int activeAngleIdx: -1
            property int activeScaleIdx: -1
            property bool snapOn: false

            property var gridPresets: [0.1, 0.25, 0.5, 1.0, 2.0, 5.0]
            property var anglePresets: [5, 15, 45, 90]
            property var scalePresets: [0.1, 0.25, 0.5]

            function findIdx(arr, val) {
                for (var i = 0; i < arr.length; ++i)
                    if (Math.abs(arr[i] - val) < 0.001) return i
                return -1
            }

            // Timer to force QQuickWidget repaint — toggling opacity marks
            // the entire subtree dirty in the scene graph, forcing a redraw.
            Timer {
                id: snapRepaintTimer
                interval: 1; repeat: false
                onTriggered: snapCol.opacity = 1.0
            }
            function forceRepaint() {
                // Nuclear option: reset Repeater models to force full delegate recreation.
                // QQuickWidget inside QDockWidget doesn't repaint sibling delegates on
                // property changes — only mouse events trigger repaints.
                var g = gridRepeater.model; gridRepeater.model = null; gridRepeater.model = g
                var a = angleRepeater.model; angleRepeater.model = null; angleRepeater.model = a
                var s = scaleRepeater.model; scaleRepeater.model = null; scaleRepeater.model = s
            }

            Component.onCompleted: {
                snapOn = PropertiesPanelController.snapEnabled
                activeGridIdx = findIdx(gridPresets, PropertiesPanelController.snapGridSize)
                activeAngleIdx = findIdx(anglePresets, PropertiesPanelController.snapAngleStep)
                activeScaleIdx = findIdx(scalePresets, PropertiesPanelController.snapScaleStep)
            }

            // Sync from external changes (MCP tools, other UI paths)
            Connections {
                target: PropertiesPanelController
                function onSnapEnabledChanged() { snapCol.snapOn = PropertiesPanelController.snapEnabled; snapCol.forceRepaint() }
                function onSnapGridSizeChanged() { snapCol.activeGridIdx = snapCol.findIdx(snapCol.gridPresets, PropertiesPanelController.snapGridSize); snapCol.forceRepaint() }
                function onSnapAngleStepChanged() { snapCol.activeAngleIdx = snapCol.findIdx(snapCol.anglePresets, PropertiesPanelController.snapAngleStep); snapCol.forceRepaint() }
                function onSnapScaleStepChanged() { snapCol.activeScaleIdx = snapCol.findIdx(snapCol.scalePresets, PropertiesPanelController.snapScaleStep); snapCol.forceRepaint() }
            }

            // Enable toggle
            Row {
                spacing: 6
                width: snapCol.btnAreaWidth

                Rectangle {
                    width: 14; height: 14; anchors.verticalCenter: parent.verticalCenter
                    border.color: PropertiesPanelController.borderColor; border.width: 1; radius: 2
                    color: snapCol.snapOn ? PropertiesPanelController.highlightColor : PropertiesPanelController.controlBgColor
                    Behavior on color { ColorAnimation { duration: 50 } }
                    Text { anchors.centerIn: parent; text: snapCol.snapOn ? "\u2713" : ""; color: "white"; font.pixelSize: 10 }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: { snapCol.snapOn = !snapCol.snapOn; PropertiesPanelController.snapEnabled = snapCol.snapOn; snapCol.forceRepaint() }
                    }
                }
                Text {
                    text: "Enable Snap (or hold Ctrl)"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 11
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            // Grid Size
            Text { text: "Grid Size (Translation)"; color: PropertiesPanelController.textColor; font.pixelSize: 11; font.bold: true }
            Flow {
                spacing: 3; width: snapCol.btnAreaWidth
                Repeater {
                    id: gridRepeater
                    model: snapCol.gridPresets
                    delegate: Rectangle {
                        width: Math.max(30, (snapCol.btnAreaWidth - 3 * (snapCol.gridPresets.length - 1)) / snapCol.gridPresets.length)
                        height: 22; radius: 3
                        color: index === snapCol.activeGridIdx ? PropertiesPanelController.highlightColor
                             : ma1.containsMouse ? Qt.lighter(PropertiesPanelController.panelColor, 1.5)
                             : PropertiesPanelController.headerColor
                        Behavior on color { ColorAnimation { duration: 50 } }
                        border.color: PropertiesPanelController.borderColor; border.width: 1
                        Text { anchors.centerIn: parent; text: modelData.toString(); color: PropertiesPanelController.textColor; font.pixelSize: 10 }
                        MouseArea {
                            id: ma1; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                            onClicked: { snapCol.activeGridIdx = index; PropertiesPanelController.snapGridSize = modelData; snapCol.forceRepaint() }
                        }
                    }
                }
            }

            // Angle Step
            Text { text: "Angle Step (Rotation)"; color: PropertiesPanelController.textColor; font.pixelSize: 11; font.bold: true }
            Flow {
                spacing: 3; width: snapCol.btnAreaWidth
                Repeater {
                    id: angleRepeater
                    model: snapCol.anglePresets
                    delegate: Rectangle {
                        width: Math.max(30, (snapCol.btnAreaWidth - 3 * (snapCol.anglePresets.length - 1)) / snapCol.anglePresets.length)
                        height: 22; radius: 3
                        color: index === snapCol.activeAngleIdx ? PropertiesPanelController.highlightColor
                             : ma2.containsMouse ? Qt.lighter(PropertiesPanelController.panelColor, 1.5)
                             : PropertiesPanelController.headerColor
                        Behavior on color { ColorAnimation { duration: 50 } }
                        border.color: PropertiesPanelController.borderColor; border.width: 1
                        Text { anchors.centerIn: parent; text: modelData + "\u00B0"; color: PropertiesPanelController.textColor; font.pixelSize: 10 }
                        MouseArea {
                            id: ma2; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                            onClicked: { snapCol.activeAngleIdx = index; PropertiesPanelController.snapAngleStep = modelData; snapCol.forceRepaint() }
                        }
                    }
                }
            }

            // Scale Step
            Text { text: "Scale Step"; color: PropertiesPanelController.textColor; font.pixelSize: 11; font.bold: true }
            Flow {
                spacing: 3; width: snapCol.btnAreaWidth
                Repeater {
                    id: scaleRepeater
                    model: snapCol.scalePresets
                    delegate: Rectangle {
                        width: Math.max(30, (snapCol.btnAreaWidth - 3 * (snapCol.scalePresets.length - 1)) / snapCol.scalePresets.length)
                        height: 22; radius: 3
                        color: index === snapCol.activeScaleIdx ? PropertiesPanelController.highlightColor
                             : ma3.containsMouse ? Qt.lighter(PropertiesPanelController.panelColor, 1.5)
                             : PropertiesPanelController.headerColor
                        Behavior on color { ColorAnimation { duration: 50 } }
                        border.color: PropertiesPanelController.borderColor; border.width: 1
                        Text { anchors.centerIn: parent; text: modelData.toString(); color: PropertiesPanelController.textColor; font.pixelSize: 10 }
                        MouseArea {
                            id: ma3; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                            onClicked: { snapCol.activeScaleIdx = index; PropertiesPanelController.snapScaleStep = modelData; snapCol.forceRepaint() }
                        }
                    }
                }
            }
        }
    }

    // ---- Primitive Content ----
    Component {
        id: primitiveComponent

        Column {
            width: parent ? parent.width : 200
            padding: 8
            spacing: 6

            property var rawCfg: PropertiesPanelController.primFieldConfig || {}
            property bool cfgShowSizeX: rawCfg.showSizeX || false
            property bool cfgShowSizeY: rawCfg.showSizeY || false
            property bool cfgShowSizeZ: rawCfg.showSizeZ || false
            property bool cfgShowRadius: rawCfg.showRadius || false
            property bool cfgShowRadius2: rawCfg.showRadius2 || false
            property bool cfgShowHeight: rawCfg.showHeight || false
            property bool cfgShowSegX: rawCfg.showSegX || false
            property bool cfgShowSegY: rawCfg.showSegY || false
            property bool cfgShowSegZ: rawCfg.showSegZ || false
            property bool cfgShowUV: rawCfg.showUV || false
            property var cfg: rawCfg

            // Size
            Text {
                visible: cfgShowSizeX || cfgShowSizeY || cfgShowSizeZ
                text: "Size"
                color: PropertiesPanelController.textColor
                font.pixelSize: 11; font.bold: true
            }
            Row {
                visible: cfgShowSizeX || cfgShowSizeY || cfgShowSizeZ
                spacing: 4; width: parent.width - 16

                TransformField { visible: cfgShowSizeX; label: "X"; value: PropertiesPanelController.primSizeX; color: "#c04040"; step: 0.1
                    onNewValue: function(val) { PropertiesPanelController.primSizeX = val } }
                TransformField { visible: cfgShowSizeY; label: "Y"; value: PropertiesPanelController.primSizeY; color: "#40c040"; step: 0.1
                    onNewValue: function(val) { PropertiesPanelController.primSizeY = val } }
                TransformField { visible: cfgShowSizeZ; label: "Z"; value: PropertiesPanelController.primSizeZ; color: "#4040c0"; step: 0.1
                    onNewValue: function(val) { PropertiesPanelController.primSizeZ = val } }
            }

            // Radius / Radius2 / Height
            Text {
                visible: cfgShowRadius || cfgShowRadius2 || cfgShowHeight
                text: {
                    var parts = []
                    if (cfg.showRadius) parts.push(cfg.radiusLabel || "Radius")
                    if (cfg.showRadius2) parts.push(cfg.radius2Label || "Radius2")
                    if (cfg.showHeight) parts.push("Height")
                    return parts.join(" / ")
                }
                color: PropertiesPanelController.textColor
                font.pixelSize: 11; font.bold: true
            }
            Row {
                visible: cfgShowRadius || cfgShowRadius2 || cfgShowHeight
                spacing: 4; width: parent.width - 16

                TransformField { visible: cfgShowRadius; label: (cfg.radiusLabel || "R").charAt(0); value: PropertiesPanelController.primRadius; color: "#c08040"; step: 0.1
                    onNewValue: function(val) { PropertiesPanelController.primRadius = val } }
                TransformField { visible: cfgShowRadius2; label: (cfg.radius2Label || "R2").charAt(0); value: PropertiesPanelController.primRadius2; color: "#c0a040"; step: 0.1
                    onNewValue: function(val) { PropertiesPanelController.primRadius2 = val } }
                TransformField { visible: cfgShowHeight; label: "H"; value: PropertiesPanelController.primHeight; color: "#40c0c0"; step: 0.1
                    onNewValue: function(val) { PropertiesPanelController.primHeight = val } }
            }

            // Segments
            Text {
                visible: cfgShowSegX || cfgShowSegY || cfgShowSegZ
                text: "Segments"
                color: PropertiesPanelController.textColor
                font.pixelSize: 11; font.bold: true
            }
            Row {
                visible: cfgShowSegX || cfgShowSegY || cfgShowSegZ
                spacing: 4; width: parent.width - 16

                TransformField { visible: cfgShowSegX; label: (cfg.segXLabel || "X").charAt(0); value: PropertiesPanelController.primSegX; color: "#808080"; step: 1
                    onNewValue: function(val) { PropertiesPanelController.primSegX = Math.max(1, Math.round(val)) } }
                TransformField { visible: cfgShowSegY; label: (cfg.segYLabel || "Y").charAt(0); value: PropertiesPanelController.primSegY; color: "#808080"; step: 1
                    onNewValue: function(val) { PropertiesPanelController.primSegY = Math.max(1, Math.round(val)) } }
                TransformField { visible: cfgShowSegZ; label: (cfg.segZLabel || "Z").charAt(0); value: PropertiesPanelController.primSegZ; color: "#808080"; step: 1
                    onNewValue: function(val) { PropertiesPanelController.primSegZ = Math.max(1, Math.round(val)) } }
            }

            // UV Tiling
            Text {
                visible: cfgShowUV
                text: "UV Tiling"
                color: PropertiesPanelController.textColor
                font.pixelSize: 11; font.bold: true
            }
            Row {
                visible: cfgShowUV
                spacing: 4; width: parent.width - 16

                TransformField { label: "U"; value: PropertiesPanelController.primUTile; color: "#c040c0"; step: 0.1
                    onNewValue: function(val) { PropertiesPanelController.primUTile = val } }
                TransformField { label: "V"; value: PropertiesPanelController.primVTile; color: "#c040c0"; step: 0.1
                    onNewValue: function(val) { PropertiesPanelController.primVTile = val } }
            }
        }
    }

    // ---- LOD Generation Content ----
    Component {
        id: lodComponent

        Column {
            id: lodContent
            width: parent ? parent.width : 200
            padding: 8
            spacing: 6

            property int lodCount: 2
            // Tracks the live reduction values from the sliders so Generate can read them
            property var reductionValues: [0.25, 0.5, 0.75, 1.0]

            // LOD level summary table
            Column {
                id: lodInfoColumn
                width: parent.width - 16
                spacing: 2
                visible: MeshLodController.currentLodLevels > 0
                property var lodInfoModel: MeshLodController.lodLevelInfo()
                Connections {
                    target: MeshLodController
                    function onLodChanged() {
                        lodInfoColumn.lodInfoModel = MeshLodController.lodLevelInfo()
                        lodPreviewSlider.value = 0
                    }
                    function onGenerationSucceeded() { lodInfoColumn.lodInfoModel = MeshLodController.lodLevelInfo() }
                }

                Text {
                    text: MeshLodController.currentLodLevels + " LOD level(s) generated:"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 11; font.bold: true
                }

                Repeater {
                    model: lodInfoColumn.lodInfoModel
                    Row {
                        spacing: 8
                        Text {
                            text: modelData.label + ":"
                            color: PropertiesPanelController.textColor; font.pixelSize: 10; width: 46
                        }
                        Text {
                            text: modelData.triangles.toLocaleString() + " triangles"
                            color: Qt.lighter(PropertiesPanelController.textColor, 0.8); font.pixelSize: 10
                        }
                    }
                }

                // Preview LOD slider
                Row {
                    spacing: 6; width: parent.width
                    Text {
                        text: "Preview:"
                        color: PropertiesPanelController.textColor; font.pixelSize: 10
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Slider {
                        id: lodPreviewSlider
                        from: 0; to: Math.max(0, MeshLodController.currentLodLevels)
                        value: 0; stepSize: 1
                        width: parent.width - 90
                        anchors.verticalCenter: parent.verticalCenter
                        onValueChanged: MeshLodController.previewLod(value)
                    }
                    Text {
                        property var info: (lodPreviewSlider.value < lodInfoColumn.lodInfoModel.length)
                            ? lodInfoColumn.lodInfoModel[Math.round(lodPreviewSlider.value)] : null
                        text: info ? (info.label + "\n" + info.triangles + " tri") : ""
                        color: PropertiesPanelController.textColor; font.pixelSize: 9; width: 52
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
            }
            Text {
                text: "No LOD levels — click Generate or Auto below."
                visible: MeshLodController.currentLodLevels === 0
                color: Qt.lighter(PropertiesPanelController.textColor, 0.6)
                font.pixelSize: 10; font.italic: true
            }

            // LOD count row
            Row {
                spacing: 6
                Text {
                    text: "Levels:"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 11
                    anchors.verticalCenter: parent.verticalCenter
                }
                Row {
                    spacing: 2
                    Repeater {
                        model: 4
                        Rectangle {
                            width: 22; height: 22; radius: 3
                            color: (index + 1) === lodCountSelector.value
                                   ? PropertiesPanelController.highlightColor
                                   : PropertiesPanelController.headerColor
                            border.color: PropertiesPanelController.borderColor; border.width: 1
                            Text {
                                anchors.centerIn: parent
                                text: index + 1
                                color: PropertiesPanelController.textColor; font.pixelSize: 11
                            }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: lodCountSelector.value = index + 1
                            }
                        }
                    }
                    // hidden SpinBox just to track value
                    SpinBox { id: lodCountSelector; visible: false; value: 2; from: 1; to: 4 }
                }
            }

            // Reduction sliders per level
            Column {
                width: parent.width - 16
                spacing: 4

                Repeater {
                    model: lodCountSelector.value

                    Row {
                        spacing: 6
                        width: parent.width

                        Text {
                            text: "LOD " + (index + 1) + ":"
                            color: PropertiesPanelController.textColor; font.pixelSize: 11
                            width: 42; anchors.verticalCenter: parent.verticalCenter
                        }
                        Slider {
                            id: reductionSlider
                            from: 0.1; to: 0.95
                            value: 0.25 * (index + 1)
                            stepSize: 0.05
                            width: parent.width - 90
                            anchors.verticalCenter: parent.verticalCenter
                            onValueChanged: {
                                var arr = lodContent.reductionValues.slice()
                                arr[index] = value
                                lodContent.reductionValues = arr
                            }
                        }
                        Text {
                            text: Math.round(reductionSlider.value * 100) + "%"
                            color: PropertiesPanelController.textColor; font.pixelSize: 10; width: 36
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }
                }
            }

            // Action buttons
            Row {
                spacing: 6
                width: parent.width - 16

                Rectangle {
                    id: generateBtn
                    height: 26; width: (parent.width - 6) * 0.5; radius: 3
                    color: genMouse.pressed ? Qt.darker(PropertiesPanelController.highlightColor, 1.2)
                         : genMouse.containsMouse ? Qt.lighter(PropertiesPanelController.highlightColor, 1.1)
                         : PropertiesPanelController.highlightColor
                    Text { anchors.centerIn: parent; text: "Generate"; color: "white"; font.pixelSize: 11 }
                    MouseArea {
                        id: genMouse; anchors.fill: parent; hoverEnabled: true
                        onClicked: {
                            var reductions = lodContent.reductionValues.slice(0, lodCountSelector.value)
                            MeshLodController.generateLods(lodCountSelector.value, reductions)
                        }
                    }
                }

                Rectangle {
                    id: autoBtn
                    height: 26; width: (parent.width - 6) * 0.25; radius: 3
                    color: autoMouse.pressed ? Qt.darker(PropertiesPanelController.headerColor, 1.3)
                         : autoMouse.containsMouse ? Qt.lighter(PropertiesPanelController.headerColor, 1.2)
                         : PropertiesPanelController.headerColor
                    border.color: PropertiesPanelController.borderColor; border.width: 1
                    Text { anchors.centerIn: parent; text: "Auto"; color: PropertiesPanelController.textColor; font.pixelSize: 11 }
                    MouseArea {
                        id: autoMouse; anchors.fill: parent; hoverEnabled: true
                        onClicked: MeshLodController.generateAutoLods()
                    }
                }

                Rectangle {
                    id: removeBtn
                    height: 26; width: (parent.width - 6) * 0.25; radius: 3
                    color: removeMouse.pressed ? Qt.darker(PropertiesPanelController.headerColor, 1.3)
                         : removeMouse.containsMouse ? Qt.lighter(PropertiesPanelController.headerColor, 1.2)
                         : PropertiesPanelController.headerColor
                    border.color: PropertiesPanelController.borderColor; border.width: 1
                    Text { anchors.centerIn: parent; text: "Remove"; color: PropertiesPanelController.textColor; font.pixelSize: 11 }
                    MouseArea {
                        id: removeMouse; anchors.fill: parent; hoverEnabled: true
                        onClicked: MeshLodController.removeLods()
                    }
                }
            }

            // Persistence note
            Text {
                width: parent.width - 16
                wrapMode: Text.Wrap
                font.pixelSize: 10
                font.italic: true
                color: Qt.lighter(PropertiesPanelController.textColor, 0.7)
                text: "LOD levels persist only when exporting as Ogre .mesh.\nFor other engines, use Export LODs below."
            }

            // Export LODs row
            Row {
                spacing: 6
                width: parent.width - 16

                ThemedComboBox {
                    id: exportFormatCombo
                    width: 90; height: 26
                    model: ["gltf", "glb", "fbx", "obj", "mesh"]
                    font.pixelSize: 11
                }

                Rectangle {
                    height: 26; width: parent.width - exportFormatCombo.width - 6; radius: 3
                    color: exportMouse.pressed ? Qt.darker(PropertiesPanelController.headerColor, 1.3)
                         : exportMouse.containsMouse ? Qt.lighter(PropertiesPanelController.headerColor, 1.2)
                         : PropertiesPanelController.headerColor
                    border.color: PropertiesPanelController.borderColor; border.width: 1
                    Text { anchors.centerIn: parent; text: "Export LODs…"; color: PropertiesPanelController.textColor; font.pixelSize: 11 }
                    MouseArea {
                        id: exportMouse; anchors.fill: parent; hoverEnabled: true
                        onClicked: MeshLodController.exportLods(exportFormatCombo.currentText)
                    }
                }
            }

            // Feedback
            Text {
                id: lodFeedback
                width: parent.width - 16
                wrapMode: Text.Wrap
                font.pixelSize: 10
                color: "#60c060"
                text: ""

                Connections {
                    target: MeshLodController
                    function onGenerationSucceeded(levels) {
                        lodFeedback.color = "#60c060"
                        lodFeedback.text = levels < 0
                            ? "Auto LOD applied."
                            : levels + " LOD level(s) generated."
                    }
                    function onExportSucceeded(count, directory) {
                        lodFeedback.color = "#60c060"
                        lodFeedback.text = count + " LOD file(s) saved to " + directory
                    }
                    function onError(msg) {
                        lodFeedback.color = "#c06060"
                        lodFeedback.text = msg
                    }
                    function onLodChanged() {
                        lodFeedback.text = ""
                    }
                }
            }
        }
    }

    // ---- Decimate (single-pass) — Phase 6 slice D ----
    // Live slider + preview that swaps a temporary LOD into the viewport,
    // mirroring the LOD section's previewLod pattern but for one-shot
    // base-mesh reduction. Apply commits the swap permanently.
    Component {
        id: decimateComponent

        Column {
            id: decimateContent
            width: parent ? parent.width : 200
            padding: 8
            spacing: 6

            // Slider state — 0..0.95. Default 0 = no change, so opening the
            // section doesn't already preview a reduction.
            property real reduction: 0.0

            // Ensure baseTriangleCount is populated when the section is opened
            // for the first time — the controller's selectionChanged hook
            // doesn't fire if the user had a selection before the singleton
            // was instantiated.
            Component.onCompleted: MeshDecimatorController.primeBaseline()

            // Debounce the preview regenerator so the slider doesn't melt
            // Ogre with a per-pixel LOD rebuild.
            Timer {
                id: previewDebounce
                interval: 150
                repeat: false
                onTriggered: MeshDecimatorController.previewReduction(decimateContent.reduction)
            }

            // Reset reduction back to the default when selection changes
            // so the slider doesn't carry a stale value across meshes.
            Connections {
                target: MeshDecimatorController
                function onSelectionChanged() {
                    decimateContent.reduction = 0.0
                    decimateFeedback.text = ""
                }
                function onApplied(before, after) {
                    decimateFeedback.color = "#60c060"
                    decimateFeedback.text = "Decimated: " + before.toLocaleString() +
                                            " → " + after.toLocaleString() + " triangles"
                }
                function onError(msg) {
                    decimateFeedback.color = "#c06060"
                    decimateFeedback.text = msg
                }
            }

            Text {
                width: parent.width - 16
                text: "Single-pass reduction of the base mesh. Drag to preview, click Apply to commit."
                wrapMode: Text.Wrap
                color: PropertiesPanelController.textColor
                font.pixelSize: 10
            }

            // Reduction percent slider
            Row {
                spacing: 6; width: parent.width - 16
                Text {
                    text: "Reduce:"
                    width: 50
                    color: PropertiesPanelController.textColor; font.pixelSize: 11
                    anchors.verticalCenter: parent.verticalCenter
                }
                Slider {
                    id: decimateSlider
                    from: 0.0; to: 0.95; stepSize: 0.05
                    value: decimateContent.reduction
                    width: parent.width - 130
                    anchors.verticalCenter: parent.verticalCenter
                    onValueChanged: {
                        decimateContent.reduction = value
                        previewDebounce.restart()
                    }
                }
                Text {
                    text: Math.round(decimateContent.reduction * 100) + "%"
                    width: 40
                    color: PropertiesPanelController.textColor; font.pixelSize: 11
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            // Live tri-count display: base → preview.
            Row {
                spacing: 8; width: parent.width - 16
                Text {
                    text: "Tris:"
                    color: PropertiesPanelController.textColor; font.pixelSize: 10
                    width: 50
                }
                Text {
                    text: MeshDecimatorController.baseTriangleCount.toLocaleString()
                    color: Qt.lighter(PropertiesPanelController.textColor, 0.8)
                    font.pixelSize: 10
                }
                Text {
                    visible: MeshDecimatorController.hasActivePreview
                    text: "→ " + MeshDecimatorController.previewTriangleCount.toLocaleString()
                    color: "#5090d0"; font.pixelSize: 10
                }
            }

            // Apply / Reset row
            Row {
                spacing: 6; width: parent.width - 16

                Rectangle {
                    width: 110; height: 28; radius: 3
                    color: applyMouse.pressed ? Qt.darker(PropertiesPanelController.highlightColor, 1.2)
                         : applyMouse.containsMouse ? Qt.lighter(PropertiesPanelController.highlightColor, 1.1)
                         : PropertiesPanelController.highlightColor
                    Text { anchors.centerIn: parent; text: "Apply"; color: "white"; font.pixelSize: 12 }
                    MouseArea {
                        id: applyMouse; anchors.fill: parent; hoverEnabled: true
                        onClicked: MeshDecimatorController.applyReduction(decimateContent.reduction)
                    }
                }

                Rectangle {
                    width: 110; height: 28; radius: 3
                    visible: MeshDecimatorController.hasActivePreview
                    color: resetMouse.pressed ? "#705050"
                         : resetMouse.containsMouse ? "#806060"
                         : "#604040"
                    Text { anchors.centerIn: parent; text: "Reset Preview"; color: "white"; font.pixelSize: 11 }
                    MouseArea {
                        id: resetMouse; anchors.fill: parent; hoverEnabled: true
                        onClicked: MeshDecimatorController.clearPreview()
                    }
                }
            }

            Text {
                id: decimateFeedback
                width: parent.width - 16
                wrapMode: Text.Wrap
                font.pixelSize: 10
                color: "#60c060"
                text: ""
            }
        }
    }

    // ---- Material Library + Mode-Tools tools (Material mode) ----
    //
    // Slice I: this column replaces the old "Open Material Editor"
    // button + the modal Material List window. The library now lives
    // inline in the Inspector: New/Import at the top, a scrollable
    // list of materials, then the live preview + per-material actions.
    Component {
        id: materialEditorToolComponent

        Column {
            id: materialToolCol
            width: parent ? parent.width : 200
            padding: 8
            spacing: 8

            // The currently-selected material in the library list.
            // Drives the preview thumbnail and the action buttons.
            property string selectedMaterialName: ""
            property var materialNames: []

            // Refresh the list when materials are imported/created.
            function refreshMaterialList() {
                materialNames = MaterialEditorQML.getMaterialList()
            }

            // Defer the model load by one event-loop tick so the
            // GridView's delegate instantiation (each of which triggers
            // a synchronous Ogre RTT preview render) doesn't fire while
            // the host QQuickWidget is still completing its own first
            // frame. Hitting the Ogre GL context from inside Qt's first
            // paint pass crashes on macOS.
            property bool gridReady: false
            Component.onCompleted: deferTimer.start()
            Timer {
                id: deferTimer
                interval: 100
                repeat: false
                onTriggered: {
                    materialToolCol.refreshMaterialList()
                    materialToolCol.gridReady = true
                }
            }

            // Re-pull the list when the Material Editor's current
            // material name changes (New / Import / Apply paths).
            Connections {
                target: MaterialEditorQML
                function onMaterialNameChanged() { materialToolCol.refreshMaterialList() }
            }

            // ── Material Library header: New / Import / Refresh ──

            Text {
                width: parent.width - 16
                text: "Material Library"
                color: PropertiesPanelController.textColor
                font.pixelSize: 11
                font.bold: true
            }

            Row {
                spacing: 4
                width: parent.width - 16

                Rectangle {
                    width: (parent.width - 8) / 3
                    height: 24
                    radius: 3
                    color: newMa.containsMouse
                        ? PropertiesPanelController.highlightColor
                        : PropertiesPanelController.headerColor
                    border.color: PropertiesPanelController.borderColor
                    border.width: 1
                    Text {
                        anchors.centerIn: parent
                        text: "+ New"
                        color: PropertiesPanelController.textColor
                        font.pixelSize: 11
                    }
                    MouseArea {
                        id: newMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            // Create a uniquely-named material and open the
                            // editor window with it. The new name is generated
                            // by MaterialEditorQML to avoid collisions.
                            MaterialEditorQML.createNewMaterial("")
                            materialToolCol.refreshMaterialList()
                            MaterialEditorQML.openMaterialEditorWindow(
                                MaterialEditorQML.materialName)
                            materialToolCol.selectedMaterialName =
                                MaterialEditorQML.materialName
                        }
                        ToolTip.visible: containsMouse
                        ToolTip.delay: 500
                        ToolTip.text: "Create a new empty material and open it in the editor."
                    }
                }

                Rectangle {
                    width: (parent.width - 8) / 3
                    height: 24
                    radius: 3
                    color: importMa.containsMouse
                        ? PropertiesPanelController.highlightColor
                        : PropertiesPanelController.headerColor
                    border.color: PropertiesPanelController.borderColor
                    border.width: 1
                    Text {
                        anchors.centerIn: parent
                        text: "↓ Import"
                        color: PropertiesPanelController.textColor
                        font.pixelSize: 11
                    }
                    MouseArea {
                        id: importMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            const picked = MaterialEditorQML.openMaterialImportDialog()
                            if (picked && picked.length > 0) {
                                MaterialEditorQML.importMaterialFile(picked)
                                materialToolCol.refreshMaterialList()
                            }
                        }
                        ToolTip.visible: containsMouse
                        ToolTip.delay: 500
                        ToolTip.text: "Import a .material script from disk."
                    }
                }

                Rectangle {
                    width: (parent.width - 8) / 3
                    height: 24
                    radius: 3
                    color: refreshMa.containsMouse
                        ? PropertiesPanelController.highlightColor
                        : PropertiesPanelController.headerColor
                    border.color: PropertiesPanelController.borderColor
                    border.width: 1
                    Text {
                        anchors.centerIn: parent
                        text: "↻ Refresh"
                        color: PropertiesPanelController.textColor
                        font.pixelSize: 11
                    }
                    MouseArea {
                        id: refreshMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: materialToolCol.refreshMaterialList()
                    }
                }
            }

            // ── Material card grid ──
            //
            // Cards mirror the old MaterialListModal layout: 90x100 cell,
            // 52x52 RTT preview thumbnail (Sphere, 64x64), name below.
            // GridView packs as many columns as the panel width allows.
            Rectangle {
                width: parent.width - 16
                // ~2 rows visible by default; ScrollView handles overflow.
                height: 220
                color: PropertiesPanelController.inputColor
                border.color: PropertiesPanelController.borderColor
                border.width: 1
                radius: 3

                Text {
                    anchors.centerIn: parent
                    visible: !materialToolCol.gridReady
                    text: "Loading materials…"
                    color: PropertiesPanelController.textColor
                    opacity: 0.5
                    font.pixelSize: 11
                }

                ScrollView {
                    anchors.fill: parent
                    anchors.margins: 1
                    clip: true
                    visible: materialToolCol.gridReady

                    GridView {
                        id: matGrid
                        cellWidth: Math.max(90,
                            (width - 4) / Math.max(1, Math.floor((width - 4) / 90)))
                        cellHeight: 100
                        model: materialToolCol.materialNames
                        delegate: Item {
                            width: matGrid.cellWidth
                            height: matGrid.cellHeight

                            Rectangle {
                                id: matCard
                                anchors.centerIn: parent
                                width: parent.width - 6
                                height: parent.height - 6
                                radius: 5
                                property bool isSelected:
                                    materialToolCol.selectedMaterialName === modelData
                                color: isSelected
                                    ? Qt.lighter(PropertiesPanelController.highlightColor, 1.5)
                                    : (cardMa.containsMouse
                                        ? Qt.lighter(PropertiesPanelController.panelColor, 1.3)
                                        : PropertiesPanelController.panelColor)
                                border.color: isSelected
                                    ? PropertiesPanelController.highlightColor
                                    : PropertiesPanelController.borderColor
                                border.width: isSelected ? 2 : 1

                                Column {
                                    anchors.centerIn: parent
                                    spacing: 4

                                    Image {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        width: 52; height: 52
                                        source: (modelData && modelData.length > 0)
                                            ? MaterialEditorQML.materialPreview(modelData)
                                            : ""
                                        fillMode: Image.PreserveAspectFit
                                        asynchronous: true
                                        // The data: URI is deterministic per
                                        // material name — QML would otherwise
                                        // hand back the stale bitmap after a
                                        // material edit. Mirror previewImage.
                                        cache: false
                                        sourceSize.width: 52
                                        sourceSize.height: 52
                                        smooth: true

                                        // Fallback dot if the RTT preview
                                        // isn't ready yet (no GL context,
                                        // first-frame load, etc.).
                                        Rectangle {
                                            anchors.centerIn: parent
                                            width: 40; height: 40; radius: 20
                                            visible: parent.status !== Image.Ready
                                            color: Qt.darker(
                                                PropertiesPanelController.highlightColor, 1.5)
                                            Text {
                                                anchors.centerIn: parent
                                                text: "🔵"
                                                font.pixelSize: 20
                                            }
                                        }
                                    }

                                    Text {
                                        width: matCard.width - 8
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        text: modelData
                                        color: PropertiesPanelController.textColor
                                        font.pixelSize: 9
                                        elide: Text.ElideMiddle
                                        horizontalAlignment: Text.AlignHCenter
                                    }
                                }

                                MouseArea {
                                    id: cardMa
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: materialToolCol.selectedMaterialName = modelData
                                    onDoubleClicked: {
                                        materialToolCol.selectedMaterialName = modelData
                                        MaterialEditorQML.openMaterialEditorWindow(modelData)
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // ── Per-material action buttons (Apply / Edit / Export) ──

            Row {
                spacing: 4
                width: parent.width - 16
                visible: materialToolCol.selectedMaterialName.length > 0

                Rectangle {
                    width: (parent.width - 8) / 3
                    height: 24
                    radius: 3
                    color: applyMa.containsMouse
                        ? PropertiesPanelController.highlightColor
                        : PropertiesPanelController.headerColor
                    border.color: PropertiesPanelController.borderColor
                    border.width: 1
                    Text {
                        anchors.centerIn: parent
                        text: "Apply to selection"
                        color: PropertiesPanelController.textColor
                        font.pixelSize: 10
                    }
                    MouseArea {
                        id: applyMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            PropertiesPanelController.applyMaterialToSelection(
                                materialToolCol.selectedMaterialName)
                        }
                        ToolTip.visible: containsMouse
                        ToolTip.delay: 500
                        ToolTip.text: "Assign this material to the selected entity/submesh(es)."
                    }
                }

                Rectangle {
                    width: (parent.width - 8) / 3
                    height: 24
                    radius: 3
                    color: editMa.containsMouse
                        ? PropertiesPanelController.highlightColor
                        : PropertiesPanelController.headerColor
                    border.color: PropertiesPanelController.borderColor
                    border.width: 1
                    Text {
                        anchors.centerIn: parent
                        text: "Edit"
                        color: PropertiesPanelController.textColor
                        font.pixelSize: 11
                    }
                    MouseArea {
                        id: editMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: MaterialEditorQML.openMaterialEditorWindow(
                            materialToolCol.selectedMaterialName)
                    }
                }

                Rectangle {
                    width: (parent.width - 8) / 3
                    height: 24
                    radius: 3
                    color: exportMa.containsMouse
                        ? PropertiesPanelController.highlightColor
                        : PropertiesPanelController.headerColor
                    border.color: PropertiesPanelController.borderColor
                    border.width: 1
                    Text {
                        anchors.centerIn: parent
                        text: "Export"
                        color: PropertiesPanelController.textColor
                        font.pixelSize: 11
                    }
                    MouseArea {
                        id: exportMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            const out = MaterialEditorQML.openMaterialExportDialog(
                                materialToolCol.selectedMaterialName)
                            if (out && out.length > 0) {
                                MaterialEditorQML.exportMaterial(
                                    out, materialToolCol.selectedMaterialName)
                            }
                        }
                    }
                }
            }

            // Slice G: Texture Channel Packer — utility for combining
            // grayscale source images into a single packed RGBA texture
            // (e.g. ORM = AO+Roughness+Metallic). Lives in Mode Tools
            // because it operates on PNG/TGA files on disk, not on the
            // currently-selected submesh's TUS.
            Rectangle {
                width: Math.min(parent.width - 16, packLabel.implicitWidth + 16)
                height: 26
                radius: 3
                color: packMa.containsMouse
                    ? PropertiesPanelController.highlightColor
                    : PropertiesPanelController.headerColor
                border.color: PropertiesPanelController.borderColor
                border.width: 1

                Text {
                    id: packLabel
                    anchors.centerIn: parent
                    text: "Pack Texture Channels…"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 11
                }
                MouseArea {
                    id: packMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.openTextureChannelPackerDialog()
                    ToolTip.visible: containsMouse
                    ToolTip.delay: 500
                    ToolTip.text: "Pack 1–4 grayscale source images into a single RGBA texture (e.g. Unity ORM = AO+Roughness+Metallic, Unreal MR)."
                }
            }

            // Slice H: Normal Map Generator — Sobel-filter a height/bump
            // source into a tangent-space normal map. Same Mode-Tools
            // placement as Pack Texture Channels (operates on disk
            // files, not the selection's TUS).
            Rectangle {
                width: Math.min(parent.width - 16, normalLabel.implicitWidth + 16)
                height: 26
                radius: 3
                color: normalMa.containsMouse
                    ? PropertiesPanelController.highlightColor
                    : PropertiesPanelController.headerColor
                border.color: PropertiesPanelController.borderColor
                border.width: 1

                Text {
                    id: normalLabel
                    anchors.centerIn: parent
                    text: "Generate Normal Map…"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 11
                }
                MouseArea {
                    id: normalMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.openNormalMapGeneratorDialog()
                    ToolTip.visible: containsMouse
                    ToolTip.delay: 500
                    ToolTip.text: "Generate a tangent-space normal map from a height/bump source via Sobel filter."
                }
            }

            // Phase 6 slice E: Texture Atlas Packer — pack N textures into
            // a single atlas + JSON UV manifest. Same Mode-Tools placement
            // as Pack Texture Channels and Generate Normal Map (operates
            // on disk files, not the selection's TUS).
            Rectangle {
                width: Math.min(parent.width - 16, atlasLabel.implicitWidth + 16)
                height: 26
                radius: 3
                color: atlasMa.containsMouse
                    ? PropertiesPanelController.highlightColor
                    : PropertiesPanelController.headerColor
                border.color: PropertiesPanelController.borderColor
                border.width: 1

                Text {
                    id: atlasLabel
                    anchors.centerIn: parent
                    text: "Pack Atlas…"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 11
                }
                MouseArea {
                    id: atlasMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.openTextureAtlasDialog()
                    ToolTip.visible: containsMouse
                    ToolTip.delay: 500
                    ToolTip.text: "Pack N textures into a single atlas image + JSON UV manifest. Reduces GPU draw-call count by consolidating bindings."
                }
            }

            // Slice I: Material Preview Environment — interactive
            // preview of the currently-selected material on Sphere/Cube
            // shapes. Drag horizontally on the thumbnail to rotate the
            // environment (yaw the directional light). Bound to the
            // library list selection.
            Item { height: 8; width: 1 }   // spacer
            Text {
                width: parent.width - 16
                text: "Material Preview"
                color: PropertiesPanelController.textColor
                font.pixelSize: 11
                font.bold: true
            }

            // The preview thumbnail itself. Square, scales with the
            // panel width up to 256 px.
            Item {
                id: previewHost
                width: parent.width - 16
                height: Math.min(width, 256)

                property int previewShape: 0   // 0=Sphere, 1=Cube, 2=Plane
                property real previewYaw: 0.0
                property real dragStartX: 0
                property real dragStartYaw: 0

                Rectangle {
                    id: previewBg
                    anchors.fill: parent
                    color: PropertiesPanelController.inputColor
                    border.color: PropertiesPanelController.borderColor
                    border.width: 1
                    radius: 3

                    Image {
                        id: previewImage
                        anchors.fill: parent
                        anchors.margins: 1
                        fillMode: Image.PreserveAspectFit
                        smooth: true
                        cache: false
                        asynchronous: true
                        source: {
                            const mat = materialToolCol.selectedMaterialName
                            if (!mat || mat.length === 0) return ""
                            return MaterialEditorQML.interactiveMaterialPreview(
                                mat,
                                Math.min(Math.floor(previewHost.width), 256),
                                previewHost.previewShape,
                                previewHost.previewYaw)
                        }
                    }
                    Text {
                        visible: previewImage.source.toString().length === 0
                        anchors.centerIn: parent
                        text: "(select a material from the list)"
                        color: PropertiesPanelController.textColor
                        opacity: 0.5
                        font.pixelSize: 10
                    }

                    // Drag horizontally to yaw the environment light.
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.OpenHandCursor
                        onPressed: mouse => {
                            previewHost.dragStartX = mouse.x
                            previewHost.dragStartYaw = previewHost.previewYaw
                            cursorShape = Qt.ClosedHandCursor
                        }
                        onReleased: cursorShape = Qt.OpenHandCursor
                        onPositionChanged: mouse => {
                            if (pressed) {
                                // 360° drag spans the panel width.
                                const dx = mouse.x - previewHost.dragStartX
                                previewHost.previewYaw =
                                    previewHost.dragStartYaw + dx * (360.0 / previewHost.width)
                            }
                        }
                        ToolTip.visible: containsMouse
                        ToolTip.delay: 500
                        ToolTip.text: "Drag horizontally to rotate the environment around the model."
                    }
                }
            }

            // Shape switcher row — Sphere / Cube / Plane.
            Row {
                spacing: 4
                width: parent.width - 16

                Repeater {
                    // Slice I: Plane was removed — its lit shading had no
                    // visible specular response and added little to a
                    // material reading.
                    model: [
                        { name: "Sphere", id: 0 },
                        { name: "Cube",   id: 1 }
                    ]
                    delegate: Rectangle {
                        property bool isSelected: previewHost.previewShape === modelData.id
                        width: (parent.width - 4) / 2
                        height: 22
                        radius: 3
                        color: isSelected
                            ? PropertiesPanelController.highlightColor
                            : (shapeMa.containsMouse
                                ? Qt.lighter(PropertiesPanelController.headerColor, 1.1)
                                : PropertiesPanelController.headerColor)
                        border.color: PropertiesPanelController.borderColor
                        border.width: 1
                        Text {
                            anchors.centerIn: parent
                            text: modelData.name
                            color: PropertiesPanelController.textColor
                            font.pixelSize: 10
                            font.bold: parent.isSelected
                        }
                        MouseArea {
                            id: shapeMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: previewHost.previewShape = modelData.id
                        }
                    }
                }
            }

            // Reset-yaw button — useful when the user has dragged far
            // around and wants the default lighting back.
            Rectangle {
                width: 90
                height: 22
                radius: 3
                color: resetMa.containsMouse
                    ? PropertiesPanelController.highlightColor
                    : PropertiesPanelController.headerColor
                border.color: PropertiesPanelController.borderColor
                border.width: 1
                Text {
                    anchors.centerIn: parent
                    text: "Reset yaw"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 10
                }
                MouseArea {
                    id: resetMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: previewHost.previewYaw = 0
                }
            }
        }
    }

    // The dialog is loaded by URL (rather than as a typed component)
    // because the Properties Panel's QML engine doesn't have the
    // MaterialEditorQML module's import path set up — only its singleton
    // C++ type. Using a Loader with a qrc:// source bypasses module
    // resolution while keeping the dialog as a top-level child so it
    // overlays the viewport correctly.
    Loader {
        id: textureChannelPackerLoader
        active: false
        anchors.centerIn: parent
        source: "qrc:/MaterialEditorQML/TextureChannelPackerDialog.qml"
        onLoaded: if (item && item.open) item.open()
    }

    function openTextureChannelPackerDialog() {
        if (!textureChannelPackerLoader.active) {
            textureChannelPackerLoader.active = true
        } else if (textureChannelPackerLoader.item) {
            textureChannelPackerLoader.item.open()
        }
    }

    // Slice H: Normal Map Generator dialog — same Loader pattern.
    Loader {
        id: normalMapGeneratorLoader
        active: false
        anchors.centerIn: parent
        source: "qrc:/MaterialEditorQML/NormalMapGeneratorDialog.qml"
        onLoaded: if (item && item.open) item.open()
    }

    function openNormalMapGeneratorDialog() {
        if (!normalMapGeneratorLoader.active) {
            normalMapGeneratorLoader.active = true
        } else if (normalMapGeneratorLoader.item) {
            normalMapGeneratorLoader.item.open()
        }
    }

    // Phase 6 slice E: Texture Atlas dialog — same Loader pattern.
    Loader {
        id: textureAtlasLoader
        active: false
        anchors.centerIn: parent
        source: "qrc:/MaterialEditorQML/TextureAtlasDialog.qml"
        onLoaded: if (item && item.open) item.open()
    }

    function openTextureAtlasDialog() {
        if (!textureAtlasLoader.active) {
            textureAtlasLoader.active = true
        } else if (textureAtlasLoader.item) {
            textureAtlasLoader.item.open()
        }
    }

    // Phase 6 slice E2: Apply Atlas dialog is launched from inside the
    // Pack Atlas dialog (Atlas → "Apply to Mesh…") to avoid taking up
    // toolbar space for a niche follow-up tool.

    // ---- Material Presets Content ----
    Component {
        id: materialPresetsComponent

        Column {
            id: presetsRoot
            width: parent ? parent.width : 200
            padding: 8
            spacing: 8

            // Flat list of presets with visual properties for the sphere preview
            property var presets: [
                { name: "Plastic (Red)",   label: "Red",      cat: "Plastic", diff: "#cc3333", spec: "#ffbbbb", shin: 35,  alpha: 1.0,  wire: false, unlit: false },
                { name: "Plastic (Blue)",  label: "Blue",     cat: "Plastic", diff: "#3355cc", spec: "#aabbff", shin: 35,  alpha: 1.0,  wire: false, unlit: false },
                { name: "Plastic (White)", label: "White",    cat: "Plastic", diff: "#dddddd", spec: "#ffffff", shin: 35,  alpha: 1.0,  wire: false, unlit: false },
                { name: "Metal (Silver)",  label: "Silver",   cat: "Metal",   diff: "#aaaaaa", spec: "#ffffff", shin: 80,  alpha: 1.0,  wire: false, unlit: false },
                { name: "Metal (Gold)",    label: "Gold",     cat: "Metal",   diff: "#cc9922", spec: "#ffffaa", shin: 80,  alpha: 1.0,  wire: false, unlit: false },
                { name: "Metal (Copper)",  label: "Copper",   cat: "Metal",   diff: "#c06030", spec: "#ffccaa", shin: 80,  alpha: 1.0,  wire: false, unlit: false },
                { name: "Wood (Oak)",      label: "Oak",      cat: "Wood",    diff: "#8b5e3c", spec: "#aa7755", shin: 5,   alpha: 1.0,  wire: false, unlit: false },
                { name: "Wood (Birch)",    label: "Birch",    cat: "Wood",    diff: "#c8a878", spec: "#e8d8b8", shin: 5,   alpha: 1.0,  wire: false, unlit: false },
                { name: "Glass (Clear)",   label: "Clear",    cat: "Glass",   diff: "#aaccee", spec: "#ffffff", shin: 100, alpha: 0.42, wire: false, unlit: false },
                { name: "Glass (Tinted)",  label: "Tinted",   cat: "Glass",   diff: "#446688", spec: "#aaccee", shin: 100, alpha: 0.55, wire: false, unlit: false },
                { name: "Unlit (White)",   label: "Unlit",    cat: "Other",   diff: "#eeeeee", spec: "#eeeeee", shin: 0,   alpha: 1.0,  wire: false, unlit: true  },
                { name: "Wireframe",       label: "Wireframe",cat: "Other",   diff: "#223322", spec: "#44dd44", shin: 0,   alpha: 1.0,  wire: true,  unlit: false },
                // PBR templates — slice E. Material structure with the
                // canonical 6 texture-unit slots (albedo / normal_map /
                // metallic / roughness / ao / emissive). Slice E renders
                // these via Phong approximation; slice F will swap in
                // real PBR shading via the pbr_workflow Pass user-binding.
                { name: "Metallic-Roughness",  label: "M-R",   cat: "PBR",     diff: "#bbbbbb", spec: "#888888", shin: 40,  alpha: 1.0,  wire: false, unlit: false },
                { name: "Specular-Glossiness", label: "S-G",   cat: "PBR",     diff: "#bbbbbb", spec: "#dddddd", shin: 60,  alpha: 1.0,  wire: false, unlit: false },
                { name: "Unlit PBR",           label: "Unlit",  cat: "PBR",    diff: "#dddddd", spec: "#dddddd", shin: 0,   alpha: 1.0,  wire: false, unlit: true  }
            ]
            property var categories: ["Plastic", "Metal", "Wood", "Glass", "Other", "PBR"]
            property string lastApplied: ""

            // Draw one category group
            Repeater {
                model: presetsRoot.categories

                Column {
                    id: catGroup
                    width: presetsRoot.width - 16
                    spacing: 4
                    property string catName: modelData
                    property var catPresets: {
                        var result = []
                        for (var i = 0; i < presetsRoot.presets.length; ++i)
                            if (presetsRoot.presets[i].cat === catName) result.push(presetsRoot.presets[i])
                        return result
                    }

                    Text {
                        text: catGroup.catName
                        color: PropertiesPanelController.textColor
                        font.pixelSize: 10; font.bold: true
                        leftPadding: 1
                    }

                    Flow {
                        width: parent.width
                        spacing: 6

                        Repeater {
                            model: catGroup.catPresets

                            Column {
                                id: sphereItem
                                spacing: 2
                                property var pdata: modelData

                                // Sphere canvas
                                Rectangle {
                                    width: 52; height: 52; radius: 4
                                    color: sphereArea.containsMouse
                                         ? Qt.lighter(PropertiesPanelController.panelColor, 1.4)
                                         : PropertiesPanelController.panelColor
                                    border.color: presetsRoot.lastApplied === sphereItem.pdata.name
                                               ? PropertiesPanelController.highlightColor
                                               : PropertiesPanelController.borderColor
                                    border.width: presetsRoot.lastApplied === sphereItem.pdata.name ? 2 : 1

                                    Canvas {
                                        anchors.centerIn: parent
                                        width: 44; height: 44

                                        property var pd: sphereItem.pdata

                                        onPaint: {
                                            var ctx = getContext("2d")
                                            ctx.clearRect(0, 0, width, height)
                                            var cx = width / 2, cy = height / 2
                                            var r = Math.min(cx, cy) - 1

                                            if (pd.wire) {
                                                // Wireframe sphere
                                                ctx.save()
                                                ctx.beginPath()
                                                ctx.arc(cx, cy, r, 0, Math.PI * 2)
                                                ctx.fillStyle = "#1a2a1a"
                                                ctx.fill()
                                                ctx.clip()
                                                ctx.strokeStyle = pd.spec
                                                ctx.lineWidth = 0.9
                                                // Latitude lines
                                                for (var lat = -0.65; lat <= 0.7; lat += 0.32) {
                                                    var latR = r * Math.sqrt(Math.max(0, 1 - lat * lat))
                                                    var latY = cy + lat * r
                                                    ctx.beginPath()
                                                    ctx.ellipse(cx, latY, latR, latR * 0.28, 0, 0, Math.PI * 2)
                                                    ctx.stroke()
                                                }
                                                // Longitude lines
                                                for (var lon = 0; lon < Math.PI; lon += Math.PI / 3) {
                                                    ctx.save()
                                                    ctx.translate(cx, cy)
                                                    ctx.rotate(lon)
                                                    ctx.beginPath()
                                                    ctx.ellipse(0, 0, r * 0.32, r, 0, 0, Math.PI * 2)
                                                    ctx.stroke()
                                                    ctx.restore()
                                                }
                                                ctx.restore()
                                            } else {
                                                // Lit sphere with radial gradient
                                                var specSize = pd.shin > 60 ? r * 0.08
                                                             : pd.shin > 20 ? r * 0.18 : r * 0.30
                                                var hx = cx - r * 0.30, hy = cy - r * 0.32

                                                // Dark edge shadow
                                                ctx.beginPath()
                                                ctx.arc(cx, cy, r, 0, Math.PI * 2)
                                                ctx.fillStyle = Qt.darker(pd.diff, 3.0)
                                                ctx.fill()

                                                // Main lit gradient
                                                var grad = ctx.createRadialGradient(hx, hy, specSize * 0.3, cx + r * 0.05, cy + r * 0.05, r * 1.05)
                                                grad.addColorStop(0.00, pd.unlit ? pd.diff : pd.spec)
                                                grad.addColorStop(0.22, pd.diff)
                                                grad.addColorStop(0.75, Qt.darker(pd.diff, 1.7))
                                                grad.addColorStop(1.00, Qt.darker(pd.diff, 3.0))

                                                ctx.beginPath()
                                                ctx.arc(cx, cy, r, 0, Math.PI * 2)
                                                ctx.globalAlpha = pd.alpha
                                                ctx.fillStyle = grad
                                                ctx.fill()
                                                ctx.globalAlpha = 1.0

                                                // Glass refraction rim
                                                if (pd.alpha < 0.9) {
                                                    var rimGrad = ctx.createRadialGradient(cx, cy, r * 0.6, cx, cy, r)
                                                    rimGrad.addColorStop(0, "transparent")
                                                    rimGrad.addColorStop(1, Qt.lighter(pd.diff, 1.6))
                                                    ctx.beginPath()
                                                    ctx.arc(cx, cy, r, 0, Math.PI * 2)
                                                    ctx.globalAlpha = 0.55
                                                    ctx.fillStyle = rimGrad
                                                    ctx.fill()
                                                    ctx.globalAlpha = 1.0
                                                }
                                            }
                                        }
                                    }

                                    MouseArea {
                                        id: sphereArea
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            MaterialPresetLibrary.applyPreset(sphereItem.pdata.name)
                                            presetsRoot.lastApplied = sphereItem.pdata.name
                                        }
                                    }
                                }

                                // Label
                                Text {
                                    text: sphereItem.pdata.label
                                    color: PropertiesPanelController.textColor
                                    font.pixelSize: 9
                                    width: 52
                                    horizontalAlignment: Text.AlignHCenter
                                    elide: Text.ElideRight
                                }
                            }
                        }
                    }
                }
            }

            // Feedback
            Text {
                id: presetFeedback
                width: presetsRoot.width - 16
                wrapMode: Text.Wrap; font.pixelSize: 10
                color: "#60c060"; text: ""

                Connections {
                    target: MaterialPresetLibrary
                    function onPresetApplied(name) {
                        presetFeedback.text = "Applied: " + name
                    }
                }
            }
        }
    }

    // ---- Mesh Validation Content ----
    Component {
        id: validationComponent

        Column {
            width: parent ? parent.width : 200
            padding: 8
            spacing: 6

            // Validate button
            Rectangle {
                width: parent.width - 16; height: 28; radius: 3
                color: validateMouse.pressed ? Qt.darker(PropertiesPanelController.highlightColor, 1.2)
                     : validateMouse.containsMouse ? Qt.lighter(PropertiesPanelController.highlightColor, 1.1)
                     : PropertiesPanelController.highlightColor
                Text { anchors.centerIn: parent; text: "Run Validation"; color: "white"; font.pixelSize: 12 }
                MouseArea {
                    id: validateMouse; anchors.fill: parent; hoverEnabled: true
                    onClicked: MeshValidator.validate()
                }
            }

            // Validating indicator
            Text {
                width: parent.width - 16
                visible: MeshValidator.validating
                text: "Validating\u2026"
                color: PropertiesPanelController.textColor; font.pixelSize: 11
                font.italic: true
            }

            // Issues list
            Column {
                width: parent.width - 16
                spacing: 3
                visible: MeshValidator.validated

                Repeater {
                    model: MeshValidator.issues

                    Row {
                        spacing: 6; width: parent.width

                        Text {
                            text: modelData.type === "error" ? "\u2718"
                                : modelData.type === "warning" ? "\u26A0"
                                : modelData.type === "info" ? "\u2139"
                                : "\u2714"
                            color: modelData.type === "error" ? "#e05050"
                                 : modelData.type === "warning" ? "#e0a030"
                                 : modelData.type === "info" ? "#5090d0"
                                 : "#60c060"
                            font.pixelSize: 13
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Text {
                            text: modelData.description
                            color: PropertiesPanelController.textColor; font.pixelSize: 10
                            wrapMode: Text.Wrap; width: parent.width - 24
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }
                }
            }

            // Fix All button
            Rectangle {
                width: parent.width - 16; height: 28; radius: 3
                visible: MeshValidator.hasFixableIssues
                color: fixMouse.pressed ? Qt.darker("#c04040", 1.2)
                     : fixMouse.containsMouse ? Qt.lighter("#c04040", 1.2)
                     : "#c04040"
                Text { anchors.centerIn: parent; text: "Fix All (re-import with cleanup)"; color: "white"; font.pixelSize: 11 }
                MouseArea {
                    id: fixMouse; anchors.fill: parent; hoverEnabled: true
                    onClicked: MeshValidator.fixAll()
                }
            }

            // Optimize Vertex Cache button (Phase 6 slice C). Distinct from
            // "Fix All" — this only mutates index ordering, never geometry.
            Rectangle {
                width: parent.width - 16; height: 28; radius: 3
                visible: MeshValidator.hasCacheOptimization
                color: cacheMouse.pressed ? Qt.darker("#5090d0", 1.2)
                     : cacheMouse.containsMouse ? Qt.lighter("#5090d0", 1.2)
                     : "#5090d0"
                Text { anchors.centerIn: parent; text: "Optimize Vertex Cache"; color: "white"; font.pixelSize: 11 }
                MouseArea {
                    id: cacheMouse; anchors.fill: parent; hoverEnabled: true
                    onClicked: MeshValidator.optimizeVertexCache()
                }
            }

            // Fix feedback
            Text {
                id: fixFeedback
                width: parent.width - 16; wrapMode: Text.Wrap
                font.pixelSize: 10; color: "#60c060"; text: ""

                Connections {
                    target: MeshValidator
                    function onFixApplied(msg) {
                        fixFeedback.color = "#60c060"
                        fixFeedback.text = msg
                    }
                    function onError(msg) {
                        fixFeedback.color = "#c06060"
                        fixFeedback.text = msg
                    }
                    // Clear the fix feedback whenever the validation result is
                    // invalidated — selection-change, or any other reason the
                    // validator resets `validated` to false.
                    function onIssuesChanged() {
                        if (!MeshValidator.validated)
                            fixFeedback.text = ""
                    }
                }
            }
        }
    }

    // ---- Animation Control Content (keyframe editor) ----
    Component {
        id: animControlComponent

        Loader {
            width: parent ? parent.width : 300
            source: "qrc:/AnimationControl/AnimationControlPanel.qml"
        }
    }

    // ---- Animation Content ----
    Component {
        id: animationComponent

        Column {
            width: parent ? parent.width : 200
            padding: 8
            spacing: 4

            property var entityGroups: PropertiesPanelController.animationData()

            function refreshAnimData() {
                entityGroups = PropertiesPanelController.animationData()
            }

            Connections {
                target: PropertiesPanelController
                function onSelectionChanged() { refreshAnimData() }
                function onAnimationStateChanged() { refreshAnimData() }
            }

            // Play/Pause button + playback speed (applies to selected entity only)
            Row {
                spacing: 8
                width: parent.width - 16

                Rectangle {
                    width: 28; height: 28; radius: 3
                    color: playMouse.pressed ? Qt.darker(PropertiesPanelController.headerColor, 1.2)
                         : playMouse.containsMouse ? Qt.lighter(PropertiesPanelController.headerColor, 1.2)
                         : PropertiesPanelController.headerColor
                    border.color: PropertiesPanelController.borderColor

                    Text {
                        anchors.centerIn: parent
                        text: PropertiesPanelController.playing ? "\u275A\u275A" : "\u25B6"
                        color: PropertiesPanelController.textColor; font.pixelSize: 14
                    }
                    MouseArea {
                        id: playMouse; anchors.fill: parent; hoverEnabled: true
                        onClicked: PropertiesPanelController.playing = !PropertiesPanelController.playing
                    }
                }

                Text {
                    text: "Speed:"
                    color: PropertiesPanelController.textColor; font.pixelSize: 11
                    anchors.verticalCenter: parent.verticalCenter
                }

                ComboBox {
                    id: speedCombo
                    width: 72; height: 26
                    anchors.verticalCenter: parent.verticalCenter
                    model: ["0.25x", "0.5x", "1x", "2x", "4x"]
                    property var values: [0.25, 0.5, 1.0, 2.0, 4.0]
                    // Pick the nearest preset rather than silently falling back
                    // to 1× when the controller's value isn't an exact match.
                    currentIndex: {
                        var s = AnimationControlController.playbackSpeed
                        var bestIndex = 0
                        var bestDiff = Math.abs(values[0] - s)
                        for (var i = 1; i < values.length; ++i) {
                            var diff = Math.abs(values[i] - s)
                            if (diff < bestDiff) { bestDiff = diff; bestIndex = i }
                        }
                        return bestIndex
                    }
                    onActivated: AnimationControlController.playbackSpeed = values[currentIndex]
                    font.pixelSize: 11
                }
            }

            // ── Blend (two-way live blend + bake) — collapsible subgroup ─────
            Column {
                id: blendGroup
                width: parent.width - 16
                spacing: 4
                visible: AnimationBlender.animations.length >= 2

                property bool blendExpanded: false

                Rectangle {
                    width: parent.width
                    height: 22
                    color: blendHeaderMouse.containsMouse
                           ? Qt.lighter(PropertiesPanelController.headerColor, 1.1)
                           : PropertiesPanelController.headerColor
                    border.color: PropertiesPanelController.borderColor
                    border.width: 1

                    Row {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left; anchors.leftMargin: 6
                        spacing: 4

                        Text {
                            text: blendGroup.blendExpanded ? "▼" : "▶"
                            color: PropertiesPanelController.textColor
                            font.pixelSize: 9
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Text {
                            text: "Blend"
                            color: PropertiesPanelController.textColor
                            font.pixelSize: 11; font.bold: true
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Text {
                            visible: AnimationBlender.active
                            text: "(active)"
                            color: PropertiesPanelController.textColor
                            font.pixelSize: 10; font.italic: true
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }

                    MouseArea {
                        id: blendHeaderMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: blendGroup.blendExpanded = !blendGroup.blendExpanded
                    }
                }

                Column {
                    width: parent.width
                    spacing: 4
                    visible: blendGroup.blendExpanded

                    Row {
                        spacing: 6
                        // "Active" toggle styled like the per-anim Enable/Loop boxes
                        Rectangle {
                            width: 14; height: 14
                            anchors.verticalCenter: parent.verticalCenter
                            border.color: PropertiesPanelController.borderColor; border.width: 1; radius: 2
                            color: AnimationBlender.active
                                   ? PropertiesPanelController.highlightColor
                                   : PropertiesPanelController.controlBgColor
                            Text {
                                anchors.centerIn: parent
                                text: AnimationBlender.active ? "✓" : ""
                                color: "white"; font.pixelSize: 10
                            }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: AnimationBlender.active = !AnimationBlender.active
                            }
                        }
                        Text {
                            text: "Active"
                            color: PropertiesPanelController.textColor
                            font.pixelSize: 11
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }

                    Row {
                        spacing: 6
                        width: parent.width

                        ComboBox {
                            id: animAPicker
                            width: (parent.width - 12) / 2
                            height: 24
                            model: AnimationBlender.animations
                            property string current: AnimationBlender.animA
                            currentIndex: model.indexOf(current)
                            onActivated: AnimationBlender.animA = model[currentIndex]
                            Connections {
                                target: AnimationBlender
                                function onSelectionChanged() {
                                    animAPicker.currentIndex = animAPicker.model.indexOf(AnimationBlender.animA)
                                }
                                function onAnimationsChanged() {
                                    animAPicker.currentIndex = animAPicker.model.indexOf(AnimationBlender.animA)
                                }
                            }
                            font.pixelSize: 11
                        }

                        ComboBox {
                            id: animBPicker
                            width: (parent.width - 12) / 2
                            height: 24
                            model: AnimationBlender.animations
                            property string current: AnimationBlender.animB
                            currentIndex: model.indexOf(current)
                            onActivated: AnimationBlender.animB = model[currentIndex]
                            Connections {
                                target: AnimationBlender
                                function onSelectionChanged() {
                                    animBPicker.currentIndex = animBPicker.model.indexOf(AnimationBlender.animB)
                                }
                                function onAnimationsChanged() {
                                    animBPicker.currentIndex = animBPicker.model.indexOf(AnimationBlender.animB)
                                }
                            }
                            font.pixelSize: 11
                        }
                    }

                    Row {
                        spacing: 6
                        width: parent.width
                        Text {
                            text: "Weight:"
                            color: PropertiesPanelController.textColor; font.pixelSize: 11
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Slider {
                            id: weightSlider
                            from: 0; to: 1; stepSize: 0.01
                            width: parent.width - 90
                            value: AnimationBlender.weight
                            onMoved: AnimationBlender.weight = value
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Text {
                            text: AnimationBlender.weight.toFixed(2)
                            color: PropertiesPanelController.textColor; font.pixelSize: 11
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }

                    Row {
                        spacing: 6
                        width: parent.width

                        ComboBox {
                            id: modeCombo
                            width: 100; height: 24
                            model: ["Mix", "Additive", "Override"]
                            currentIndex: AnimationBlender.mode
                            onActivated: AnimationBlender.mode = currentIndex
                            font.pixelSize: 11
                        }

                        TextField {
                            id: bakeNameInput
                            width: parent.width - 100 - 6 - 70 - 12
                            height: 24
                            placeholderText: "BlendedClip"
                            font.pixelSize: 11
                        }

                        Button {
                            text: "Bake"
                            width: 70; height: 24
                            font.pixelSize: 11
                            onClicked: {
                                var name = bakeNameInput.text.length > 0
                                           ? bakeNameInput.text
                                           : "Blended_" + AnimationBlender.animA + "_" + AnimationBlender.animB
                                var result = AnimationBlender.bake(name, 30)
                                if (result.length > 0) bakeNameInput.text = ""
                            }
                        }
                    }
                }
            }

            // Per-entity groups
            Repeater {
                model: entityGroups

                Column {
                    width: parent.width - 16
                    spacing: 2

                    property var grp: modelData
                    property bool groupExpanded: true

                    // Entity header with chevron
                    Rectangle {
                        width: parent.width; height: 22
                        color: entHeaderMouse.containsMouse
                               ? Qt.lighter(PropertiesPanelController.headerColor, 1.1)
                               : PropertiesPanelController.headerColor
                        border.color: PropertiesPanelController.borderColor; border.width: 1

                        Row {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left; anchors.leftMargin: 6; spacing: 4

                            Text {
                                text: groupExpanded ? "\u25BC" : "\u25B6"
                                color: PropertiesPanelController.textColor; font.pixelSize: 8
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Text {
                                text: grp.entity + " (" + grp.animations.length + " anim)"
                                color: PropertiesPanelController.textColor; font.pixelSize: 11; font.bold: true
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }

                        MouseArea {
                            id: entHeaderMouse; anchors.fill: parent; hoverEnabled: true
                            onClicked: groupExpanded = !groupExpanded
                        }
                    }

                    // Expanded content
                    Column {
                        id: entityGroupColumn
                        visible: groupExpanded
                        width: parent.width
                        spacing: 2
                        leftPadding: 8

                        // Per-entity tolerance preset for the Simplify button.
                        // Stored on the column so every animation row in this group reads
                        // the same value.
                        property string simplifyPreset: "conservative"

                        // Tolerance preset row
                        Row {
                            spacing: 6; topPadding: 4; bottomPadding: 2
                            Text {
                                text: "Simplify tolerance:"
                                color: PropertiesPanelController.textColor; font.pixelSize: 10
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            ThemedComboBox {
                                id: simplifyPresetCombo
                                width: 130; height: 22
                                model: ["Conservative", "Balanced", "Aggressive"]
                                // Default is Conservative since simplify is
                                // destructive — Balanced/Aggressive trade
                                // fidelity for compression and should be an
                                // opt-in choice.
                                currentIndex: 0
                                font.pixelSize: 10
                                onActivated: {
                                    var v = ["conservative", "balanced", "aggressive"][currentIndex]
                                    entityGroupColumn.simplifyPreset = v
                                }
                            }
                        }

                        // Animation rows
                        Repeater {
                            model: grp.animations

                            Rectangle {
                                width: parent.width - 8; height: 22; color: "transparent"

                                // True when this entity's per-anim toggles are owned by the blender.
                                // While the blender is active for this entity, Enable/Loop are
                                // disabled to avoid fighting blender::apply() each frame.
                                property bool blenderOwns: AnimationBlender.active
                                    && AnimationBlender.activeEntityName === grp.entity

                                Row {
                                    anchors.fill: parent; spacing: 6

                                    // Enable checkbox
                                    Rectangle {
                                        width: 14; height: 14; anchors.verticalCenter: parent.verticalCenter
                                        border.color: PropertiesPanelController.borderColor; border.width: 1; radius: 2
                                        color: modelData.enabled ? PropertiesPanelController.highlightColor : PropertiesPanelController.controlBgColor
                                        opacity: blenderOwns ? 0.4 : 1.0
                                        Text { anchors.centerIn: parent; text: modelData.enabled ? "\u2713" : ""; color: "white"; font.pixelSize: 10 }
                                        MouseArea {
                                            anchors.fill: parent
                                            enabled: !blenderOwns
                                            onClicked: PropertiesPanelController.toggleAnimationEnabled(grp.entity, modelData.name, !modelData.enabled)
                                        }
                                    }

                                    // Loop checkbox
                                    Rectangle {
                                        width: 14; height: 14; anchors.verticalCenter: parent.verticalCenter
                                        border.color: PropertiesPanelController.borderColor; border.width: 1; radius: 7
                                        color: modelData.loop ? PropertiesPanelController.highlightColor : PropertiesPanelController.controlBgColor
                                        opacity: blenderOwns ? 0.4 : 1.0
                                        Text { anchors.centerIn: parent; text: modelData.loop ? "\u21BB" : ""; color: "white"; font.pixelSize: 8 }
                                        MouseArea {
                                            anchors.fill: parent
                                            enabled: !blenderOwns
                                            onClicked: PropertiesPanelController.toggleAnimationLoop(grp.entity, modelData.name, !modelData.loop)
                                        }
                                    }

                                    // Name (double-click to rename)
                                    Text {
                                        id: animText
                                        visible: !animEdit.visible
                                        text: modelData.name + " (" + modelData.length.toFixed(2) + "s)"
                                        color: PropertiesPanelController.textColor; font.pixelSize: 11
                                        elide: Text.ElideRight; anchors.verticalCenter: parent.verticalCenter
                                        MouseArea {
                                            anchors.fill: parent
                                            onDoubleClicked: { animEdit.text = modelData.name; animEdit.visible = true; animEdit.forceActiveFocus(); animEdit.selectAll() }
                                        }
                                    }
                                    TextInput {
                                        id: animEdit; visible: false; width: 100
                                        color: PropertiesPanelController.textColor; font.pixelSize: 11
                                        anchors.verticalCenter: parent.verticalCenter; selectByMouse: true
                                        Rectangle { anchors.fill: parent; anchors.margins: -2; z: -1; color: PropertiesPanelController.inputColor; border.color: PropertiesPanelController.highlightColor; border.width: 1; radius: 2 }
                                        onEditingFinished: { if (text.length > 0 && text !== modelData.name) PropertiesPanelController.renameAnimation(grp.entity, modelData.name, text); visible = false }
                                        Keys.onEscapePressed: visible = false
                                    }

                                    // Simplify button — removes redundant keyframes from this animation.
                                    // Hidden for non-skeletal entities since simplifyAnimation
                                    // requires skeletal node tracks.
                                    Rectangle {
                                        id: simplifyBtn
                                        visible: grp.hasSkeleton
                                        width: 22; height: 18; radius: 3
                                        anchors.verticalCenter: parent.verticalCenter
                                        color: simplifyMouse.pressed ? Qt.darker(PropertiesPanelController.headerColor, 1.2)
                                             : simplifyMouse.containsMouse ? Qt.lighter(PropertiesPanelController.headerColor, 1.2)
                                             : PropertiesPanelController.headerColor
                                        border.color: PropertiesPanelController.borderColor; border.width: 1

                                        // Lazy-cached redundancy analysis. The walk is O(N keyframes),
                                        // ~25ms on a Mixamo clip — running it from a binding fires on
                                        // every delegate refresh, which dominates the inspector frame
                                        // budget. We compute on first hover, then re-run whenever the
                                        // user changes the preset.
                                        property var cachedAnalysis: null
                                        function refreshAnalysis() {
                                            cachedAnalysis = PropertiesPanelController.analyzeAnimationKeyframes(
                                                grp.entity, modelData.name, entityGroupColumn.simplifyPreset)
                                        }
                                        Connections {
                                            target: entityGroupColumn
                                            function onSimplifyPresetChanged() { simplifyBtn.cachedAnalysis = null }
                                        }

                                        Text {
                                            anchors.centerIn: parent
                                            text: "\u2702"  // scissors — keyframe trimming
                                            color: PropertiesPanelController.textColor; font.pixelSize: 11
                                        }
                                        ToolTip.visible: simplifyMouse.containsMouse
                                        ToolTip.delay: 600
                                        ToolTip.text: {
                                            var a = simplifyBtn.cachedAnalysis
                                            if (!a) return "Simplify (hover to analyze…)"
                                            if (a.total === 0) return "Simplify (analyze unavailable)"
                                            return "Simplify (" + entityGroupColumn.simplifyPreset + "): "
                                                   + a.redundant + " of " + a.total
                                                   + " keyframes redundant (" + a.percent.toFixed(1) + "%)"
                                        }
                                        MouseArea {
                                            id: simplifyMouse; anchors.fill: parent; hoverEnabled: true
                                            onEntered: {
                                                if (!simplifyBtn.cachedAnalysis)
                                                    simplifyBtn.refreshAnalysis()
                                            }
                                            onClicked: {
                                                var preset = entityGroupColumn.simplifyPreset
                                                var removed = PropertiesPanelController.simplifyAnimation(grp.entity, modelData.name, preset)
                                                simplifyResultPopup.removed = removed
                                                simplifyResultPopup.animName = modelData.name
                                                simplifyResultPopup.open()
                                                simplifyBtn.cachedAnalysis = null  // invalidate after mutation
                                            }
                                        }
                                    }

                                    // Bake — resample / reduce every bone track in this
                                    // animation. Mirrors the curve editor's per-bone Bake
                                    // dropdown but applies to the whole animation in one
                                    // undo macro. Hidden for non-skeletal entities.
                                    ThemedComboBox {
                                        id: bakeAnimCombo
                                        visible: grp.hasSkeleton
                                        width: 90; height: 18
                                        anchors.verticalCenter: parent.verticalCenter
                                        font.pixelSize: 10
                                        model: [
                                            "Bake…",
                                            "Sparse",
                                            "Medium",
                                            "Dense",
                                            "Set to 10 FPS",
                                            "Set to 15 FPS",
                                            "Set to 30 FPS",
                                            "Set to 60 FPS"
                                        ]
                                        ToolTip.visible: hovered
                                        ToolTip.text: "Bake every bone track in this animation"
                                        onActivated: function(index) {
                                            // Density int passes through:
                                            // 0 Sparse / 1 Medium / 2 Dense / 3-6 = 10/15/30/60 FPS exact.
                                            if (index >= 1 && index < model.length) {
                                                PropertiesPanelController.bakeAnimation(
                                                    grp.entity, modelData.name, index - 1)
                                            }
                                            currentIndex = 0
                                            simplifyBtn.cachedAnalysis = null
                                        }
                                    }
                                }
                            }
                        }

                        // Skeleton/Weights row (if has skeleton)
                        Row {
                            visible: grp.hasSkeleton
                            spacing: 8; topPadding: 4

                            Rectangle {
                                width: 14; height: 14; anchors.verticalCenter: parent.verticalCenter
                                border.color: PropertiesPanelController.borderColor; border.width: 1; radius: 2
                                color: grp.showSkeleton ? PropertiesPanelController.highlightColor : PropertiesPanelController.controlBgColor
                                Text { anchors.centerIn: parent; text: grp.showSkeleton ? "\u2713" : ""; color: "white"; font.pixelSize: 10 }
                                MouseArea { anchors.fill: parent; onClicked: PropertiesPanelController.toggleSkeletonDebug(grp.entity, !grp.showSkeleton) }
                            }
                            Text { text: "Skeleton"; color: PropertiesPanelController.textColor; font.pixelSize: 10; anchors.verticalCenter: parent.verticalCenter }

                            Rectangle {
                                width: 14; height: 14; anchors.verticalCenter: parent.verticalCenter
                                border.color: PropertiesPanelController.borderColor; border.width: 1; radius: 2
                                color: grp.showWeights ? PropertiesPanelController.highlightColor : PropertiesPanelController.controlBgColor
                                Text { anchors.centerIn: parent; text: grp.showWeights ? "\u2713" : ""; color: "white"; font.pixelSize: 10 }
                                MouseArea { anchors.fill: parent; onClicked: PropertiesPanelController.toggleBoneWeights(grp.entity, !grp.showWeights) }
                            }
                            Text { text: "Weights"; color: PropertiesPanelController.textColor; font.pixelSize: 10; anchors.verticalCenter: parent.verticalCenter }
                        }

                        // Export Pose button (if has skeleton)
                        Rectangle {
                            visible: grp.hasSkeleton
                            width: parent.width - 8; height: 24; radius: 3
                            color: exportPoseMouse.pressed ? Qt.darker(PropertiesPanelController.headerColor, 1.2)
                                 : exportPoseMouse.containsMouse ? Qt.lighter(PropertiesPanelController.headerColor, 1.2)
                                 : PropertiesPanelController.headerColor
                            border.color: PropertiesPanelController.borderColor; border.width: 1

                            Text {
                                anchors.centerIn: parent
                                text: "Export Pose"
                                color: PropertiesPanelController.textColor; font.pixelSize: 11
                            }
                            MouseArea {
                                id: exportPoseMouse; anchors.fill: parent; hoverEnabled: true
                                onClicked: PropertiesPanelController.exportCurrentPose()
                            }
                        }
                    }
                }
            }
        }
    }

    // ---- Undo History Content ----
    Component {
        id: undoHistoryComponent

        Column {
            width: parent ? parent.width : 200
            spacing: 0

            property var historyEntries: PropertiesPanelController.undoHistory
            property int currentIndex: PropertiesPanelController.undoIndex

            // Empty state
            Text {
                visible: historyEntries.length === 0
                text: "No undo history"
                color: Qt.darker(PropertiesPanelController.textColor, 1.4)
                font.pixelSize: 11
                font.italic: true
                padding: 8
            }

            // "Clean State" entry (index 0 — before any command)
            Rectangle {
                visible: historyEntries.length > 0
                width: parent.width
                height: 26
                color: currentIndex === 0 ? PropertiesPanelController.highlightColor
                                          : historyCleanMouse.containsMouse ? Qt.lighter(PropertiesPanelController.panelColor, 1.15)
                                          : "transparent"

                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    spacing: 6

                    Text {
                        text: currentIndex === 0 ? "\u25B6" : ""
                        color: currentIndex === 0 ? "white" : PropertiesPanelController.textColor
                        font.pixelSize: 9
                        anchors.verticalCenter: parent.verticalCenter
                    }

                    Text {
                        text: "Initial State"
                        color: currentIndex === 0 ? "white" : Qt.darker(PropertiesPanelController.textColor, 1.2)
                        font.pixelSize: 11
                        font.italic: true
                    }
                }

                MouseArea {
                    id: historyCleanMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: PropertiesPanelController.undoToIndex(0)
                }
            }

            // Command entries
            Repeater {
                model: historyEntries

                Rectangle {
                    required property var modelData
                    required property int index

                    width: parent ? parent.width : 200
                    height: 26
                    color: {
                        var isActive = (index < currentIndex)
                        var isCurrent = (index === currentIndex - 1)
                        if (isCurrent) return PropertiesPanelController.highlightColor
                        if (historyEntryMouse.containsMouse) return Qt.lighter(PropertiesPanelController.panelColor, 1.15)
                        if (!isActive) return Qt.darker(PropertiesPanelController.panelColor, 1.05)
                        return "transparent"
                    }

                    Row {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 8
                        spacing: 6

                        Text {
                            text: (index === currentIndex - 1) ? "\u25B6" : ""
                            color: (index === currentIndex - 1) ? "white" : PropertiesPanelController.textColor
                            font.pixelSize: 9
                            anchors.verticalCenter: parent.verticalCenter
                        }

                        Text {
                            text: modelData.text || ("Command " + (index + 1))
                            color: {
                                var isActive = (index < currentIndex)
                                var isCurrent = (index === currentIndex - 1)
                                if (isCurrent) return "white"
                                if (!isActive) return Qt.darker(PropertiesPanelController.textColor, 1.4)
                                return PropertiesPanelController.textColor
                            }
                            font.pixelSize: 11
                        }
                    }

                    MouseArea {
                        id: historyEntryMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: PropertiesPanelController.undoToIndex(index + 1)
                    }
                }
            }

            // Separator
            Rectangle {
                visible: historyEntries.length > 0
                width: parent.width
                height: 1
                color: PropertiesPanelController.borderColor
            }

            // Clear History button
            Rectangle {
                visible: historyEntries.length > 0
                width: parent.width - 16
                height: 26
                anchors.horizontalCenter: parent.horizontalCenter
                radius: 3
                color: clearHistoryMouse.pressed ? Qt.darker(PropertiesPanelController.headerColor, 1.2)
                     : clearHistoryMouse.containsMouse ? Qt.lighter(PropertiesPanelController.headerColor, 1.2)
                     : PropertiesPanelController.headerColor
                border.color: PropertiesPanelController.borderColor
                border.width: 1

                Text {
                    anchors.centerIn: parent
                    text: "Clear History"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 11
                }

                MouseArea {
                    id: clearHistoryMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: PropertiesPanelController.clearUndoHistory()
                }
            }

            // Bottom padding
            Item { width: 1; height: 8 }
        }
    }

    // Result popup for the per-animation Simplify button.
    Popup {
        id: simplifyResultPopup
        modal: true; focus: true
        anchors.centerIn: Overlay.overlay
        padding: 16
        property int removed: 0
        property string animName: ""
        background: Rectangle {
            color: PropertiesPanelController.panelColor
            border.color: PropertiesPanelController.borderColor; border.width: 1
            radius: 4
        }
        contentItem: Column {
            spacing: 10
            Text {
                text: simplifyResultPopup.removed > 0
                      ? "Removed " + simplifyResultPopup.removed + " redundant keyframe(s) from '" + simplifyResultPopup.animName + "'."
                      : "No redundant keyframes found in '" + simplifyResultPopup.animName + "'."
                color: PropertiesPanelController.textColor
                font.pixelSize: 12
                wrapMode: Text.WordWrap; width: 320
            }
            Rectangle {
                width: 80; height: 24; radius: 3
                anchors.right: parent.right
                color: simplifyOkMouse.containsMouse
                    ? Qt.lighter(PropertiesPanelController.headerColor, 1.2)
                    : PropertiesPanelController.headerColor
                border.color: PropertiesPanelController.borderColor
                Text { anchors.centerIn: parent; text: "OK"; color: PropertiesPanelController.textColor; font.pixelSize: 11 }
                MouseArea {
                    id: simplifyOkMouse; anchors.fill: parent; hoverEnabled: true
                    onClicked: simplifyResultPopup.close()
                }
            }
        }
    }
}
