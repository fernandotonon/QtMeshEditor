import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import PropertiesPanel 1.0
import AnimationControl 1.0
import EditorMode 1.0
import MaterialEditorQML 1.0
import ThemeManager 1.0
import AssetBrowser 1.0

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

    // ---- Auto-rig (#407) inline state, lives in the Inspector Rigging section
    // (replaces the old modal AutoRigDialog) ----
    property var    rigTemplates: ["humanoid", "biped", "quadruped", "generic"]
    property int    rigTemplateIndex: 0
    property var    rigAlgos: ["pinocchio", "unirig"]
    property int    rigAlgoIndex: 0             // pinocchio (offline) default
    property var    rigUpAxes: ["x", "y", "z"]
    property int    rigUpAxisIndex: 1           // +Y default
    property bool   rigAlsoSkin: true
    property bool   rigShowAdvanced: false      // up-axis picker
    property string rigStatus: ""
    property bool   rigStatusError: false

    function runAutoRig() {
        if (AutoRigController.busy || !AutoRigController.hasRiggableSelection) return
        const r = AutoRigController.autoRigSelected(
            root.rigTemplates[root.rigTemplateIndex],
            root.rigUpAxes[root.rigUpAxisIndex],
            root.rigAlsoSkin,
            root.rigAlgos[root.rigAlgoIndex])
        // UniRig runs on a worker thread: it returns {pending:true} immediately
        // and resolves later via the onRigged / onError signals. Don't touch the
        // status line yet — the progress bar takes over while busy.
        if (r && r.pending) {
            root.rigStatus = ""
            root.rigStatusError = false
            return
        }
        if (r && r.applied) {
            root.rigStatus = "Rigged (" + (r.algorithm ? r.algorithm : "pinocchio")
                + "): " + r.boneCount + " bones, "
                + r.verticesSampled + " verts, "
                + r.jointsRecentered + " recentered"
                + (root.rigAlsoSkin ? (r.skinned ? " (+ skinned)" : " (skin failed)") : "")
                + (r.fallbackReason ? "\n" + r.fallbackReason : "")
            root.rigStatusError = false
        } else {
            root.rigStatus = "Failed: " + (r && r.error ? r.error : "unknown error")
            root.rigStatusError = true
        }
    }

    function runMarkerRig() {
        if (AutoRigController.busy) return
        const r = AutoRigController.commitMarkerRig(root.rigAlsoSkin)
        if (r && r.applied) {
            root.rigStatus = "Rigged from markers: " + r.boneCount + " bones, "
                + r.markersApplied + " markers"
                + (root.rigAlsoSkin ? (r.skinned ? " (+ skinned)" : " (skin failed)") : "")
            root.rigStatusError = false
        } else {
            root.rigStatus = "Failed: " + (r && r.error ? r.error : "unknown error")
            root.rigStatusError = true
        }
    }

    Connections {
        target: AutoRigController
        // Worker-thread (UniRig) completion. The synchronous Pinocchio path
        // already set the status line in runAutoRig(); this only fires for the
        // async path (and harmlessly overwrites with the same info if both ran).
        function onRigged(r) {
            if (!r) return
            root.rigStatus = "Rigged (" + (r.algorithm ? r.algorithm : "pinocchio")
                + "): " + r.boneCount + " bones, "
                + r.verticesSampled + " verts"
                + (root.rigAlsoSkin ? (r.skinned ? " (+ skinned)" : " (skin failed)") : "")
                + (r.fallbackReason ? "\n" + r.fallbackReason : "")
            root.rigStatusError = false
        }
        function onError(msg) {
            root.rigStatus = "Failed: " + msg
            root.rigStatusError = true
        }
    }

    // ---- Small inline Inspector primitives reused by the Rigging section ----
    component RigButton: Rectangle {
        id: rb
        property string label: ""
        property bool buttonEnabled: true
        signal clicked()
        implicitWidth: rbText.implicitWidth + 18
        height: 24
        radius: 3
        opacity: rb.buttonEnabled ? 1.0 : 0.45
        color: rbMa.containsMouse && rb.buttonEnabled
            ? PropertiesPanelController.highlightColor
            : PropertiesPanelController.headerColor
        border.color: PropertiesPanelController.borderColor
        border.width: 1
        Text {
            id: rbText
            anchors.centerIn: parent
            text: rb.label
            color: PropertiesPanelController.textColor
            font.pixelSize: 11
        }
        MouseArea {
            id: rbMa
            anchors.fill: parent
            hoverEnabled: true
            enabled: rb.buttonEnabled
            cursorShape: rb.buttonEnabled ? Qt.PointingHandCursor : Qt.ForbiddenCursor
            onClicked: rb.clicked()
        }
    }

    component RigCheckbox: Row {
        id: rcb
        property string label: ""
        property bool checked: false
        signal toggled()
        spacing: 6
        Rectangle {
            width: 14; height: 14; radius: 2
            anchors.verticalCenter: parent.verticalCenter
            color: PropertiesPanelController.inputColor
            border.color: PropertiesPanelController.borderColor
            border.width: 1
            Text {
                anchors.centerIn: parent
                text: rcb.checked ? "✓" : ""
                color: PropertiesPanelController.textColor
                font.pixelSize: 11
            }
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: rcb.label
            color: PropertiesPanelController.textColor
            font.pixelSize: 11
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: rcb.toggled()
        }
    }

    component RigSegments: Row {
        id: rseg
        property var options: []
        property int index: 0
        signal picked(int i)
        spacing: 4
        Repeater {
            model: rseg.options
            Rectangle {
                id: segRect
                width: Math.max(56, rsegText.implicitWidth + 16)
                height: 22
                radius: 3
                color: index === rseg.index
                    ? PropertiesPanelController.highlightColor
                    : PropertiesPanelController.headerColor
                // Keyboard accessibility: each segment is tab-focusable, with a
                // focus ring; Space/Enter selects it. (Mouse still works too.)
                activeFocusOnTab: true
                border.color: segRect.activeFocus
                    ? PropertiesPanelController.highlightColor
                    : PropertiesPanelController.borderColor
                border.width: segRect.activeFocus ? 2 : 1
                Accessible.role: Accessible.RadioButton
                Accessible.name: modelData
                Accessible.checked: index === rseg.index
                Keys.onSpacePressed: rseg.picked(index)
                Keys.onReturnPressed: rseg.picked(index)
                Keys.onEnterPressed: rseg.picked(index)
                Text {
                    id: rsegText
                    anchors.centerIn: parent
                    text: modelData
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 10
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: { segRect.forceActiveFocus(); rseg.picked(index) }
                }
            }
        }
    }

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

    // On load, honor the current mode's default tab (e.g. Object mode → Mode
    // Tools now that it has tools). Without this the panel always started on the
    // Inspector tab because onModeChanged only fires on a subsequent mode switch.
    Component.onCompleted: {
        root.currentTab = root.defaultTabForMode(EditorModeController.currentMode)
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

            // ---- AI: Image → 3D (epic #764, Object mode) ----
            CollapsibleSection {
                title: "AI: Image → 3D"
                sectionVisible: root.currentTab === root.modeToolsTab
                    && MeshGenController.available
                    && root.modeToolMatches(EditorModeController.ObjectMode)
                expanded: false

                Component.onCompleted: content = meshGenToolsComponent
            }

            // ---- VAT ----
            // Bake Vertex Animation Texture (OpenVAT format). Lives in
            // the Animation Mode tools so it only surfaces when the
            // user is editing animated selections.
            CollapsibleSection {
                title: "VAT"
                sectionVisible: root.modeToolSectionVisible(
                    EditorModeController.AnimationMode,
                    PropertiesPanelController.hasAnimations)
                // Collapsed by default — VAT is a power-user export
                // step, not part of the routine animation review flow,
                // so the panel doesn't auto-occupy real estate. The
                // user expands when they want to bake.
                expanded: false

                Component.onCompleted: content = animationModeToolsComponent
            }

            // ---- Isometric sprites (#724) ----
            CollapsibleSection {
                title: "Isometric Sprites"
                sectionVisible: root.modeToolSectionVisible(
                    EditorModeController.AnimationMode,
                    IsometricSpritesController.hasExportableSelection)
                expanded: false

                Component.onCompleted: content = isometricSpritesToolsComponent
            }

            // ---- Skinning (Animation mode) ----
            // Issue #402: auto skin weights. Surfaced in Animation
            // Mode because skinning governs how the mesh deforms
            // under animation — it's a rigging step, not a mesh-topology
            // edit. Gated on having a *skinned* selection (a skeleton),
            // NOT on hasAnimations — a skeleton-bearing mesh with no
            // clips yet still needs skinning, and that's exactly the
            // case where you'd want to author weights before animating.
            CollapsibleSection {
                title: "Skinning"
                sectionVisible: root.currentTab === root.modeToolsTab
                    && root.modeToolMatches(EditorModeController.AnimationMode)
                    && SkinWeightsController.hasSkinnedSelection
                expanded: false

                Component.onCompleted: content = skinningToolsComponent
            }

            // ---- Rigging (Animation mode) ----
            // Issue #407: native auto-rig. Shown in Animation Mode for a
            // STATIC (skeleton-less) selection — embedding a skeleton is the
            // step that turns a static mesh into an animatable one, so it
            // belongs next to Skinning. Gated on hasRiggableSelection (a
            // static mesh); already-rigged meshes show the Skinning section
            // instead.
            CollapsibleSection {
                id: riggingSection
                title: "Rigging"
                sectionVisible: root.currentTab === root.modeToolsTab
                    && root.modeToolMatches(EditorModeController.AnimationMode)
                    && AutoRigController.hasRiggableSelection
                expanded: false

                Component.onCompleted: content = riggingToolsComponent

                // Don't strand the viewport in marker-capture mode if the
                // section disappears (mode change, deselect, re-rig) — the
                // inline UI replaced the dialog's onClosing cancel.
                onSectionVisibleChanged: if (!sectionVisible
                        && AutoRigController.markerMode)
                    AutoRigController.cancelMarkerPlacement()
            }

            // ---- Skeleton (Animation mode) ----
            // Bone/skeleton visualization toggles (skeleton overlay + bone-weight
            // heat-map). Lives in its OWN section, independent of animation clips,
            // so it surfaces for ANY skinned mesh — including a skeleton-bearing
            // mesh with no animations yet (e.g. a freshly auto-rigged static
            // mesh). Previously these toggles were buried per-animation-group
            // inside the Animations section and never appeared without clips.
            // This is the home for future bone-level features (bone select,
            // per-bone transforms, etc.).
            CollapsibleSection {
                title: "Skeleton"
                sectionVisible: root.currentTab === root.modeToolsTab
                    && root.modeToolMatches(EditorModeController.AnimationMode)
                    && PropertiesPanelController.hasSkeletonSelection
                expanded: false

                Component.onCompleted: content = skeletonToolsComponent
            }

            // ---- Texture Paint (Material mode) ----
            // (Brush color/radius/strength/falloff live on the toolbar
            //  paint-brush popup. The Inspector panel keeps only the
            //  paint-target switch, slot picker, and texture preview.)

            CollapsibleSection {
                title: "Texture Paint"
                sectionVisible: root.modeToolSectionVisible(
                    EditorModeController.MaterialMode,
                    true)
                expanded: true

                Component.onCompleted: content = texturePaintComponent
            }

            CollapsibleSection {
                id: uvEditSection
                title: "UV Edit"
                sectionVisible: root.modeToolSectionVisible(
                    EditorModeController.MaterialMode,
                    PropertiesPanelController.hasEntitySelection)
                expanded: false

                function updateUvEditEmbedded() {
                    if (sectionVisible && expanded)
                        UVEditorController.setInspectorEmbedded(true)
                    else
                        UVEditorController.setInspectorEmbedded(false)
                }
                onSectionVisibleChanged: updateUvEditEmbedded()
                onExpandedChanged: updateUvEditEmbedded()

                Component.onCompleted: content = uvEditComponent
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
            // Shown for any skeleton-bearing selection (not just clips) so the
            // "Generate from text" control is available on a freshly-rigged mesh
            // (e.g. a UniRig auto-rig with no animations yet).
            CollapsibleSection {
                title: "Animations"
                sectionVisible: root.modeToolSectionVisible(
                    EditorModeController.AnimationMode,
                    PropertiesPanelController.hasSkeletonSelection)

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
                expanded: true

                Component.onCompleted: content = decimateComponent
            }

            // ---- Material Editor (Material mode) ----
            CollapsibleSection {
                title: "Material Editor"
                sectionVisible: root.modeToolSectionVisible(
                    EditorModeController.MaterialMode,
                    true)
                expanded: false

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

            // ---- Asset Folder Scan (platform profile) ----
            CollapsibleSection {
                title: "Asset Folder Scan"
                sectionVisible: root.modeToolSectionVisible(
                    EditorModeController.ValidationMode,
                    true)
                expanded: false

                Component.onCompleted: content = assetScanComponent
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

            Column {
                width: outlinerColumn.width
                spacing: 6

                Row {
                    width: parent.width
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
                }  // end of inner Row holding the Merge button

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

    // ---- Animation Mode Tools Content ----
    Component {
        id: animationModeToolsComponent

        Column {
            id: animToolsCol
            width: parent ? parent.width : 200
            padding: 8
            spacing: 6

            property var animList: VATBakerController.availableAnimations
            property string animName: animList.length > 0 ? animList[0] : ""
            property int fps: 30
            property string outputDir: ""
            property string lastResultText: ""
            property bool lastOk: false
            // "Include shader" toggle + per-engine checkboxes. Carry
            // the QML state directly so the row is collapsed by default
            // and the user opts in.
            property bool includeShaders: false
            property bool shaderGodot: true
            property bool shaderUnity: false
            property bool shaderUnreal: false

            // Same label-column width Edit Mode Tools uses, so the
            // Anim / FPS / Out rows align with each other.
            readonly property int labelWidth: 44

            Connections {
                target: VATBakerController
                function onAvailableAnimationsChanged() {
                    animToolsCol.animList = VATBakerController.availableAnimations
                    if (animToolsCol.animList.indexOf(animToolsCol.animName) < 0) {
                        animToolsCol.animName = animToolsCol.animList.length > 0
                            ? animToolsCol.animList[0]
                            : ""
                    }
                }
                function onBakeFinished(ok, posTex, err) {
                    animToolsCol.lastOk = ok
                    animToolsCol.lastResultText = ok ? "✓ " + posTex : "✗ " + err
                }
            }

            // Animation picker — use ThemedComboBox so the dropdown
            // matches the Animations panel's per-clip simplify-preset
            // picker rather than Qt's default-styled ComboBox.
            Row {
                spacing: 6; width: parent.width - 16

                Text {
                    text: "Anim:"
                    color: PropertiesPanelController.textColor; font.pixelSize: 11
                    width: animToolsCol.labelWidth
                    anchors.verticalCenter: parent.verticalCenter
                }
                ThemedComboBox {
                    width: parent.width - animToolsCol.labelWidth - 6
                    height: 22
                    font.pixelSize: 11
                    model: animToolsCol.animList
                    currentIndex: model.indexOf(animToolsCol.animName)
                    onCurrentTextChanged: animToolsCol.animName = currentText
                    enabled: !VATBakerController.isBaking && model.length > 0
                }
            }

            // FPS — themed input with up/down step arrows, but plain
            // "FPS:" label on the left (no coloured box) so it matches
            // the Anim / Out row labels.
            Row {
                spacing: 6; width: parent.width - 16

                Text {
                    text: "FPS:"
                    color: PropertiesPanelController.textColor; font.pixelSize: 11
                    width: animToolsCol.labelWidth
                    anchors.verticalCenter: parent.verticalCenter
                }
                Rectangle {
                    id: animFpsBg
                    width: parent.width - animToolsCol.labelWidth - 6
                    height: 22
                    anchors.verticalCenter: parent.verticalCenter
                    color: PropertiesPanelController.inputColor
                    border.color: animFpsInput.activeFocus
                        ? PropertiesPanelController.highlightColor
                        : PropertiesPanelController.borderColor
                    border.width: 1
                    radius: 2
                    opacity: VATBakerController.isBaking ? 0.5 : 1.0

                    // Step from the LIVE textbox value, not the
                    // committed `animToolsCol.fps`. If the user typed
                    // "60" and then hit ↑ before pressing Enter, the
                    // arrow used to read the stale model value and
                    // could jump back to "41". Now we parse the
                    // current text first; if it doesn't parse, fall
                    // back to the committed value.
                    function stepFps(delta) {
                        var typed = parseInt(animFpsInput.text)
                        var base = isNaN(typed) ? animToolsCol.fps : typed
                        var next = Math.max(1, Math.min(120, base + delta))
                        animToolsCol.fps = next
                        animFpsInput.text = String(next)
                    }

                    TextInput {
                        id: animFpsInput
                        anchors.left: parent.left
                        anchors.right: animFpsArrows.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        anchors.margins: 4
                        text: String(animToolsCol.fps)
                        color: PropertiesPanelController.textColor
                        font.pixelSize: 11
                        verticalAlignment: TextInput.AlignVCenter
                        selectByMouse: true
                        clip: true
                        enabled: !VATBakerController.isBaking
                        validator: IntValidator { bottom: 1; top: 120 }
                        onEditingFinished: {
                            var v = parseInt(text)
                            if (!isNaN(v)) {
                                v = Math.max(1, Math.min(120, v))
                                animToolsCol.fps = v
                                text = String(v)
                            } else {
                                text = String(animToolsCol.fps)
                            }
                        }
                        Keys.onUpPressed:   animFpsBg.stepFps(1)
                        Keys.onDownPressed: animFpsBg.stepFps(-1)
                    }
                    Column {
                        id: animFpsArrows
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        width: 14
                        Rectangle {
                            width: parent.width; height: parent.height / 2
                            color: fpsUpMa.pressed ? Qt.darker(PropertiesPanelController.panelColor, 1.2)
                                 : fpsUpMa.containsMouse ? Qt.lighter(PropertiesPanelController.panelColor, 1.2)
                                 : PropertiesPanelController.panelColor
                            border.color: PropertiesPanelController.borderColor; border.width: 1
                            Text {
                                anchors.centerIn: parent; text: "▲"
                                font.pixelSize: 6; color: PropertiesPanelController.textColor
                            }
                            MouseArea {
                                id: fpsUpMa
                                anchors.fill: parent; hoverEnabled: true
                                enabled: !VATBakerController.isBaking
                                onClicked: animFpsBg.stepFps(1)
                            }
                        }
                        Rectangle {
                            width: parent.width; height: parent.height / 2
                            color: fpsDownMa.pressed ? Qt.darker(PropertiesPanelController.panelColor, 1.2)
                                 : fpsDownMa.containsMouse ? Qt.lighter(PropertiesPanelController.panelColor, 1.2)
                                 : PropertiesPanelController.panelColor
                            border.color: PropertiesPanelController.borderColor; border.width: 1
                            Text {
                                anchors.centerIn: parent; text: "▼"
                                font.pixelSize: 6; color: PropertiesPanelController.textColor
                            }
                            MouseArea {
                                id: fpsDownMa
                                anchors.fill: parent; hoverEnabled: true
                                enabled: !VATBakerController.isBaking
                                onClicked: animFpsBg.stepFps(-1)
                            }
                        }
                    }
                }
            }

            // Output dir + Browse — themed input panel (matches the
            // FPS input above) with the plain "Out:" label.
            Row {
                spacing: 6; width: parent.width - 16

                Text {
                    text: "Out:"
                    color: PropertiesPanelController.textColor; font.pixelSize: 11
                    width: animToolsCol.labelWidth
                    anchors.verticalCenter: parent.verticalCenter
                }
                Rectangle {
                    id: animOutBg
                    width: parent.width - animToolsCol.labelWidth - 6 - 60 - 6
                    height: 22
                    anchors.verticalCenter: parent.verticalCenter
                    color: PropertiesPanelController.inputColor
                    border.color: animOutField.activeFocus
                        ? PropertiesPanelController.highlightColor
                        : PropertiesPanelController.borderColor
                    border.width: 1
                    radius: 2
                    opacity: VATBakerController.isBaking ? 0.5 : 1.0
                    TextInput {
                        id: animOutField
                        anchors.fill: parent
                        anchors.margins: 4
                        text: animToolsCol.outputDir
                        onTextChanged: animToolsCol.outputDir = text
                        color: PropertiesPanelController.textColor
                        font.pixelSize: 11
                        verticalAlignment: TextInput.AlignVCenter
                        selectByMouse: true
                        clip: true
                        enabled: !VATBakerController.isBaking
                        // Placeholder via a sibling Text that hides
                        // when the input has content — Qt's TextInput
                        // doesn't carry placeholderText natively.
                        Text {
                            anchors.fill: parent
                            verticalAlignment: Text.AlignVCenter
                            text: "/path/to/output"
                            color: PropertiesPanelController.borderColor
                            font.pixelSize: 11
                            visible: animOutField.text.length === 0 && !animOutField.activeFocus
                        }
                    }
                }
                Rectangle {
                    id: animBrowseBtn
                    width: 60; height: 22; radius: 2
                    anchors.verticalCenter: parent.verticalCenter
                    color: animBrowseMa.pressed
                            ? Qt.darker(PropertiesPanelController.headerColor, 1.2)
                         : (animBrowseMa.containsMouse || animBrowseBtn.activeFocus)
                            ? Qt.lighter(PropertiesPanelController.headerColor, 1.2)
                         : PropertiesPanelController.headerColor
                    border.color: animBrowseBtn.activeFocus
                            ? PropertiesPanelController.highlightColor
                            : PropertiesPanelController.borderColor
                    border.width: animBrowseBtn.activeFocus ? 2 : 1
                    activeFocusOnTab: !VATBakerController.isBaking
                    Accessible.role: Accessible.Button
                    Accessible.name: "Browse for VAT output folder"
                    Keys.onSpacePressed: animBrowseMa.clicked(null)
                    Keys.onReturnPressed: animBrowseMa.clicked(null)
                    Keys.onEnterPressed: animBrowseMa.clicked(null)
                    Text {
                        anchors.centerIn: parent
                        text: "Browse"
                        color: PropertiesPanelController.textColor; font.pixelSize: 10
                    }
                    MouseArea {
                        id: animBrowseMa
                        anchors.fill: parent
                        hoverEnabled: true
                        enabled: !VATBakerController.isBaking
                        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ForbiddenCursor
                        onClicked: {
                            if (!animBrowseMa.enabled) return
                            const picked = VATBakerController.chooseOutputDir(animToolsCol.outputDir)
                            if (picked && picked.length > 0)
                                animToolsCol.outputDir = picked
                        }
                    }
                }
            }

            // Include-shader master toggle. Off by default — the bake
            // produces all of the data files unconditionally, and the
            // shader templates are an opt-in convenience: copy
            // openvat.gdshader / openvat.shader / openvat.usf next to
            // the bake so the user can drop the whole folder into a
            // project without chasing tools/vat-shaders/ in the repo.
            // Master "Include shader" checkbox — same 14×14 themed
            // square-with-checkmark style the Animations panel uses
            // for its per-clip Enable / Loop toggles. Stock
            // `Controls.CheckBox` carries Qt's full theming and is
            // ~20-22 px tall — overlarge inside the tight inspector
            // rows. The themed rectangle keeps the row at row-height
            // and the visual language consistent with the rest of
            // the panel.
            Row {
                spacing: 6
                width: parent.width - 16
                Rectangle {
                    id: includeShaderChk
                    width: 14; height: 14
                    anchors.verticalCenter: parent.verticalCenter
                    // Show a thicker / accent border on keyboard focus
                    // so it's obvious the toggle is reachable via Tab.
                    border.color: includeShaderChk.activeFocus
                        ? PropertiesPanelController.highlightColor
                        : PropertiesPanelController.borderColor
                    border.width: includeShaderChk.activeFocus ? 2 : 1
                    radius: 2
                    color: animToolsCol.includeShaders
                        ? PropertiesPanelController.highlightColor
                        : PropertiesPanelController.controlBgColor
                    opacity: VATBakerController.isBaking ? 0.4 : 1.0
                    // Keyboard-reachable + assistive-tech-readable: stock
                    // Controls.CheckBox handles all of this for free, but
                    // we replaced it with a themed Rectangle for size +
                    // visual consistency with the Animations panel. Add
                    // back the affordances by hand.
                    activeFocusOnTab: !VATBakerController.isBaking
                    Accessible.role: Accessible.CheckBox
                    Accessible.name: "Include shader"
                    Accessible.checkable: true
                    Accessible.checked: animToolsCol.includeShaders
                    Keys.onSpacePressed:  if (!VATBakerController.isBaking) animToolsCol.includeShaders = !animToolsCol.includeShaders
                    Keys.onReturnPressed: if (!VATBakerController.isBaking) animToolsCol.includeShaders = !animToolsCol.includeShaders
                    Keys.onEnterPressed:  if (!VATBakerController.isBaking) animToolsCol.includeShaders = !animToolsCol.includeShaders
                    Text {
                        anchors.centerIn: parent
                        text: animToolsCol.includeShaders ? "✓" : ""
                        color: "white"; font.pixelSize: 10
                    }
                    MouseArea {
                        anchors.fill: parent
                        enabled: !VATBakerController.isBaking
                        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ForbiddenCursor
                        onClicked: {
                            animToolsCol.includeShaders = !animToolsCol.includeShaders
                            includeShaderChk.forceActiveFocus()
                        }
                    }
                }
                Text {
                    text: "Include shader"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 11
                    anchors.verticalCenter: parent.verticalCenter
                    opacity: VATBakerController.isBaking ? 0.45 : 1.0
                    MouseArea {
                        anchors.fill: parent
                        enabled: !VATBakerController.isBaking
                        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ForbiddenCursor
                        // Clicking the label toggles AND focuses the
                        // checkbox, matching the behaviour of a native
                        // labelled CheckBox.
                        onClicked: {
                            animToolsCol.includeShaders = !animToolsCol.includeShaders
                            includeShaderChk.forceActiveFocus()
                        }
                    }
                }
            }

            // Per-engine checkboxes — only shown when the master
            // toggle is on. Defaults to Godot only since that's the
            // engine the website demo + most early users target.
            // Same 14×14 themed-rectangle style as the master.
            Row {
                visible: animToolsCol.includeShaders
                spacing: 10
                width: parent.width - 16
                leftPadding: 18
                Repeater {
                    model: [
                        { label: "Godot",  prop: "shaderGodot"  },
                        { label: "Unity",  prop: "shaderUnity"  },
                        { label: "Unreal", prop: "shaderUnreal" },
                    ]
                    delegate: Row {
                        spacing: 4
                        Rectangle {
                            id: engineChk
                            width: 14; height: 14
                            anchors.verticalCenter: parent.verticalCenter
                            border.color: engineChk.activeFocus
                                ? PropertiesPanelController.highlightColor
                                : PropertiesPanelController.borderColor
                            border.width: engineChk.activeFocus ? 2 : 1
                            radius: 2
                            color: animToolsCol[modelData.prop]
                                ? PropertiesPanelController.highlightColor
                                : PropertiesPanelController.controlBgColor
                            opacity: VATBakerController.isBaking ? 0.4 : 1.0
                            activeFocusOnTab: !VATBakerController.isBaking
                            Accessible.role: Accessible.CheckBox
                            Accessible.name: modelData.label + " shader"
                            Accessible.checkable: true
                            Accessible.checked: animToolsCol[modelData.prop]
                            Keys.onSpacePressed:  if (!VATBakerController.isBaking) animToolsCol[modelData.prop] = !animToolsCol[modelData.prop]
                            Keys.onReturnPressed: if (!VATBakerController.isBaking) animToolsCol[modelData.prop] = !animToolsCol[modelData.prop]
                            Keys.onEnterPressed:  if (!VATBakerController.isBaking) animToolsCol[modelData.prop] = !animToolsCol[modelData.prop]
                            Text {
                                anchors.centerIn: parent
                                text: animToolsCol[modelData.prop] ? "✓" : ""
                                color: "white"; font.pixelSize: 10
                            }
                            MouseArea {
                                anchors.fill: parent
                                enabled: !VATBakerController.isBaking
                                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ForbiddenCursor
                                onClicked: {
                                    animToolsCol[modelData.prop] = !animToolsCol[modelData.prop]
                                    engineChk.forceActiveFocus()
                                }
                            }
                        }
                        Text {
                            text: modelData.label
                            color: PropertiesPanelController.textColor
                            font.pixelSize: 11
                            anchors.verticalCenter: parent.verticalCenter
                            opacity: VATBakerController.isBaking ? 0.45 : 1.0
                            MouseArea {
                                anchors.fill: parent
                                enabled: !VATBakerController.isBaking
                                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ForbiddenCursor
                                onClicked: {
                                    animToolsCol[modelData.prop] = !animToolsCol[modelData.prop]
                                    engineChk.forceActiveFocus()
                                }
                            }
                        }
                    }
                }
            }

            // Bake button — same look as Animation Control's play button
            // (lighten/darken on the header color; no highlight ramp).
            Rectangle {
                id: animBakeBtn
                width: parent.width - 16; height: 26; radius: 3
                color: animBakeMa.pressed
                        ? Qt.darker(PropertiesPanelController.headerColor, 1.2)
                     : (animBakeMa.containsMouse || animBakeBtn.activeFocus) && animBakeMa.enabled
                        ? Qt.lighter(PropertiesPanelController.headerColor, 1.2)
                     : PropertiesPanelController.headerColor
                opacity: animBakeMa.enabled ? 1.0 : 0.45
                border.color: animBakeBtn.activeFocus
                        ? PropertiesPanelController.highlightColor
                        : PropertiesPanelController.borderColor
                border.width: animBakeBtn.activeFocus ? 2 : 1
                activeFocusOnTab: animBakeMa.enabled
                Accessible.role: Accessible.Button
                Accessible.name: VATBakerController.isBaking ? "Baking VAT" : "Bake VAT"
                Keys.onSpacePressed: animBakeMa.clicked(null)
                Keys.onReturnPressed: animBakeMa.clicked(null)
                Keys.onEnterPressed: animBakeMa.clicked(null)

                Text {
                    anchors.centerIn: parent
                    text: VATBakerController.isBaking ? "Baking…" : "Bake VAT"
                    color: PropertiesPanelController.textColor; font.pixelSize: 11
                }
                MouseArea {
                    id: animBakeMa
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: !VATBakerController.isBaking
                              && animToolsCol.animName !== ""
                              && animToolsCol.outputDir !== ""
                    cursorShape: enabled ? Qt.PointingHandCursor : Qt.ForbiddenCursor
                    onClicked: {
                        if (!animBakeMa.enabled) return
                        // Collect the per-engine selection only when
                        // the master "Include shader" toggle is on,
                        // otherwise pass an empty list (the controller
                        // then skips VATShaderEmitter entirely).
                        let engines = []
                        if (animToolsCol.includeShaders) {
                            if (animToolsCol.shaderGodot)  engines.push("godot")
                            if (animToolsCol.shaderUnity)  engines.push("unity")
                            if (animToolsCol.shaderUnreal) engines.push("unreal")
                        }
                        VATBakerController.bake(
                            animToolsCol.animName,
                            animToolsCol.fps,
                            animToolsCol.outputDir,
                            "",
                            engines)
                    }
                }
            }

            // Last-result one-liner (green on success, red on error).
            Text {
                visible: animToolsCol.lastResultText !== ""
                text: animToolsCol.lastResultText
                color: animToolsCol.lastOk ? "#60c060" : "#e06060"
                font.pixelSize: 10
                wrapMode: Text.Wrap
                width: parent.width - 16
            }
        }
    }

    // ---- Isometric Sprites Tools (Animation mode) ----
    Component {
        id: isometricSpritesToolsComponent

        Column {
            width: parent ? parent.width : 200
            padding: 8
            spacing: 6

            Text {
                width: parent.width - 16
                wrapMode: Text.Wrap
                opacity: 0.8
                color: PropertiesPanelController.textColor
                font.pixelSize: 10
                text: "Render the selected mesh to an 8-direction isometric sprite "
                    + "atlas (rows = directions, columns = animation frames)."
            }

            Rectangle {
                id: isoBtn
                width: Math.min(parent.width - 16, isoLabel.implicitWidth + 16)
                height: 26
                radius: 3
                opacity: IsometricSpritesController.hasExportableSelection ? 1.0 : 0.45
                color: isoMa.containsMouse && IsometricSpritesController.hasExportableSelection
                    ? PropertiesPanelController.highlightColor
                    : PropertiesPanelController.headerColor
                activeFocusOnTab: IsometricSpritesController.hasExportableSelection
                Accessible.role: Accessible.Button
                Accessible.name: "Export Isometric Sprites"
                Keys.onSpacePressed: if (IsometricSpritesController.hasExportableSelection) root.openIsometricSpritesDialog()
                Keys.onReturnPressed: if (IsometricSpritesController.hasExportableSelection) root.openIsometricSpritesDialog()
                Keys.onEnterPressed: if (IsometricSpritesController.hasExportableSelection) root.openIsometricSpritesDialog()
                border.color: isoBtn.activeFocus
                    ? PropertiesPanelController.highlightColor
                    : PropertiesPanelController.borderColor
                border.width: isoBtn.activeFocus ? 2 : 1

                Text {
                    id: isoLabel
                    anchors.centerIn: parent
                    text: "Export Isometric Sprites…"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 11
                }
                MouseArea {
                    id: isoMa
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: IsometricSpritesController.hasExportableSelection
                    cursorShape: IsometricSpritesController.hasExportableSelection
                        ? Qt.PointingHandCursor : Qt.ForbiddenCursor
                    onClicked: root.openIsometricSpritesDialog()
                    ToolTip.visible: containsMouse
                    ToolTip.delay: 500
                    ToolTip.text: IsometricSpritesController.hasExportableSelection
                        ? "Render an isometric directions×frames PNG atlas from the live scene."
                        : "Select a mesh first."
                }
            }
        }
    }

    // ---- AI: Image → 3D Content (Object mode, epic #764) ----
    // Runs TripoSR on a worker thread (MeshGenController) so the app stays
    // responsive; shows a staged progress bar + Cancel.
    Component {
        id: meshGenToolsComponent

        Column {
            width: parent ? parent.width : 200
            padding: 8
            spacing: 6

            Text {
                width: parent.width - 16
                wrapMode: Text.Wrap
                opacity: 0.8
                color: PropertiesPanelController.textColor
                font.pixelSize: 10
                text: "Reconstruct a 3D mesh from a single image (TripoSR). "
                    + "Background removal (U²-Net) runs first. Pick an image to start."
            }

            // Resolution picker
            Row {
                spacing: 6
                Text {
                    text: "Resolution"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 11
                    anchors.verticalCenter: parent.verticalCenter
                }
                ComboBox {
                    id: mgResCombo
                    width: 90
                    model: ["128 (fast)", "256", "320"]
                    currentIndex: 1
                    enabled: !MeshGenController.busy
                    property int resValue: [128, 256, 320][currentIndex]
                }
            }

            // Remove-background toggle
            CheckBox {
                id: mgRemoveBg
                text: "Remove background"
                checked: true
                enabled: !MeshGenController.busy
                contentItem: Text {
                    text: mgRemoveBg.text
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 11
                    leftPadding: mgRemoveBg.indicator.width + 4
                    verticalAlignment: Text.AlignVCenter
                }
            }

            // Generate button
            Rectangle {
                id: mgBtn
                width: Math.min(parent.width - 16, mgLabel.implicitWidth + 16)
                height: 26
                radius: 3
                opacity: MeshGenController.busy ? 0.45 : 1.0
                color: mgMa.containsMouse && !MeshGenController.busy
                    ? PropertiesPanelController.highlightColor
                    : PropertiesPanelController.headerColor
                border.color: PropertiesPanelController.borderColor
                border.width: 1
                Text {
                    id: mgLabel
                    anchors.centerIn: parent
                    text: "Generate from Image…"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 11
                }
                MouseArea {
                    id: mgMa
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: !MeshGenController.busy
                    cursorShape: MeshGenController.busy ? Qt.BusyCursor : Qt.PointingHandCursor
                    onClicked: MeshGenController.pickImageAndGenerate(
                        mgResCombo.resValue, mgRemoveBg.checked)
                }
            }

            // Progress bar + status (only while busy)
            ProgressBar {
                id: mgProgress
                width: parent.width - 16
                visible: MeshGenController.busy
                from: 0; to: 1
                indeterminate: value <= 0
                value: 0
            }

            Text {
                id: mgStatus
                width: parent.width - 16
                wrapMode: Text.Wrap
                visible: text.length > 0
                color: PropertiesPanelController.textColor
                font.pixelSize: 10
                text: ""
            }

            // Cancel (only while busy)
            Rectangle {
                width: Math.min(parent.width - 16, 80)
                height: 22
                radius: 3
                visible: MeshGenController.busy
                color: mgCancelMa.containsMouse
                    ? PropertiesPanelController.highlightColor
                    : PropertiesPanelController.headerColor
                border.color: PropertiesPanelController.borderColor
                border.width: 1
                Text {
                    anchors.centerIn: parent
                    text: "Cancel"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 11
                }
                MouseArea {
                    id: mgCancelMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: MeshGenController.cancel()
                }
            }

            Connections {
                target: MeshGenController
                function onProgress(stage, done, total) {
                    if (total > 0 && done >= 0) {
                        mgProgress.indeterminate = (stage === "prep" || stage === "background")
                        mgProgress.value = total > 0 ? (done / total) : 0
                    }
                }
                function onStatusMessage(msg) { mgStatus.text = msg }
                function onCompleted(result) {
                    mgProgress.value = 1
                    mgStatus.text = "Done: " + result.vertexCount + " verts, "
                        + result.triangleCount + " tris"
                }
                function onError(msg) { mgStatus.text = "Error: " + msg }
            }
        }
    }

    // ---- Skinning Tools Content (Animation mode) ----
    // Issue #402: auto skin weights. The "Compute Skin Weights…"
    // button opens the dialog; it disables on static meshes
    // (no skeleton). The operation is undoable (Ctrl+Z).
    Component {
        id: skinningToolsComponent

        Column {
            width: parent ? parent.width : 200
            padding: 8
            spacing: 6

            Text {
                width: parent.width - 16
                wrapMode: Text.Wrap
                opacity: 0.8
                color: PropertiesPanelController.textColor
                font.pixelSize: 10
                text: "Auto-generate per-vertex bone weights for the selected "
                    + "skinned mesh (inverse-distance smooth bind). Undoable."
            }

            Rectangle {
                id: skinBtn
                width: Math.min(parent.width - 16, skinLabel.implicitWidth + 16)
                height: 26
                radius: 3
                opacity: SkinWeightsController.hasSkinnedSelection ? 1.0 : 0.45
                color: skinMa.containsMouse && SkinWeightsController.hasSkinnedSelection
                    ? PropertiesPanelController.highlightColor
                    : PropertiesPanelController.headerColor
                // Keyboard accessibility: tab focus + Enter/Space opens
                // the dialog, with a focus-ring border so keyboard users
                // can see where they are.
                activeFocusOnTab: SkinWeightsController.hasSkinnedSelection
                Accessible.role: Accessible.Button
                Accessible.name: "Compute Skin Weights"
                Keys.onSpacePressed: if (SkinWeightsController.hasSkinnedSelection) root.openSkinWeightsDialog()
                Keys.onReturnPressed: if (SkinWeightsController.hasSkinnedSelection) root.openSkinWeightsDialog()
                Keys.onEnterPressed: if (SkinWeightsController.hasSkinnedSelection) root.openSkinWeightsDialog()
                border.color: skinBtn.activeFocus
                    ? PropertiesPanelController.highlightColor
                    : PropertiesPanelController.borderColor
                border.width: skinBtn.activeFocus ? 2 : 1

                Text {
                    id: skinLabel
                    anchors.centerIn: parent
                    text: "Compute Skin Weights…"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 11
                }
                MouseArea {
                    id: skinMa
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: SkinWeightsController.hasSkinnedSelection
                    cursorShape: SkinWeightsController.hasSkinnedSelection
                        ? Qt.PointingHandCursor : Qt.ForbiddenCursor
                    onClicked: root.openSkinWeightsDialog()
                    ToolTip.visible: containsMouse
                    ToolTip.delay: 500
                    ToolTip.text: SkinWeightsController.hasSkinnedSelection
                        ? "Compute per-vertex bone weights via inverse-distance to bone segments. Mesh must have a skeleton."
                        : "Select a skinned mesh (with a skeleton) first."
                }
            }
        }
    }

    // ---- Rigging Tools Content (Animation mode) ----
    // Issue #407: native auto-rig, inline in the Inspector (no modal dialog).
    // Smart show/hide:
    //   * marker mode active  → only the guidance + in-session controls show,
    //   * idle                → the two entry points (markers / template),
    //                           skin checkbox, and a collapsible "Advanced"
    //                           block (template + up-axis pickers).
    // All gated on AutoRigController.hasRiggableSelection (a static mesh).
    Component {
        id: riggingToolsComponent

        Column {
            id: rigCol
            width: parent ? parent.width : 200
            padding: 8
            spacing: 8

            readonly property bool canRig: AutoRigController.hasRiggableSelection
            readonly property bool marking: AutoRigController.markerMode
            // Mirror rigIdle.isUnirig so the intro text can branch on the backend.
            readonly property bool isUnirig: root.rigAlgos[root.rigAlgoIndex] === "unirig"

            Text {
                width: parent.width - 16
                wrapMode: Text.Wrap
                opacity: 0.8
                color: PropertiesPanelController.textColor
                font.pixelSize: 10
                text: rigCol.marking
                    ? "Click each highlighted point on the mesh in the viewport."
                    : (rigCol.canRig
                        ? "Embed a skeleton into this unrigged mesh. Optionally skin "
                          + "in one click."
                        : "Select a static (unrigged) mesh to enable rigging.")
            }

            // ── Marker mode: guidance + in-session controls only ──────────
            Column {
                width: parent.width - 16
                spacing: 6
                visible: rigCol.marking

                Text {
                    width: parent.width
                    wrapMode: Text.Wrap
                    font.pixelSize: 11
                    color: PropertiesPanelController.highlightColor
                    text: AutoRigController.currentMarkerLabel.length > 0
                        ? ("Place: " + AutoRigController.currentMarkerLabel
                           + "   (" + AutoRigController.markerCount + "/"
                           + AutoRigController.markerTotal + ")")
                        : ("All " + AutoRigController.markerCount + "/"
                           + AutoRigController.markerTotal
                           + " placed — click 'Rig from markers'")
                }

                Flow {
                    width: parent.width
                    spacing: 6
                    RigButton {
                        label: "Skip"
                        buttonEnabled: AutoRigController.currentMarkerLabel.length > 0
                        onClicked: AutoRigController.skipCurrentMarker()
                    }
                    RigButton {
                        label: "Undo"
                        buttonEnabled: AutoRigController.markerCount > 0
                        onClicked: AutoRigController.undoLastMarker()
                    }
                    RigButton {
                        label: "Cancel"
                        onClicked: AutoRigController.cancelMarkerPlacement()
                    }
                    RigButton {
                        label: AutoRigController.busy ? "Rigging…" : "Rig from markers"
                        buttonEnabled: !AutoRigController.busy
                            && AutoRigController.markerPlacedCount > 0
                        onClicked: root.runMarkerRig()
                    }
                }
            }

            // ── Idle: skeleton type + entry points + options ──────────────
            Column {
                id: rigIdle
                width: parent.width - 16
                spacing: 8
                // Hidden while marking OR while the worker rig is running (the
                // progress block below takes over) so the controls can't be
                // re-triggered mid-run.
                visible: !rigCol.marking && !AutoRigController.busy

                // True when the ML (UniRig) backend is selected. UniRig predicts
                // the whole skeleton from geometry, so the template type, markers
                // and up-axis (all template-only inputs) are hidden for it.
                readonly property bool isUnirig: root.rigAlgos[root.rigAlgoIndex] === "unirig"

                // Skeleton algorithm — Pinocchio (native template, offline) or
                // UniRig (ML, ONNX; falls back to the template when unavailable).
                Text {
                    text: "Algorithm"
                    color: PropertiesPanelController.textColor
                    opacity: 0.8
                    font.pixelSize: 10
                }
                Flow {
                    width: parent.width
                    spacing: 4
                    RigSegments {
                        options: root.rigAlgos
                        index: root.rigAlgoIndex
                        onPicked: function(i) { root.rigAlgoIndex = i }
                    }
                }
                // AI-powered notice — shown only for UniRig, makes the local-model
                // nature explicit.
                Text {
                    width: parent.width
                    visible: rigIdle.isUnirig
                    wrapMode: Text.Wrap
                    color: PropertiesPanelController.textColor
                    opacity: 0.7
                    font.pixelSize: 9
                    text: "✨ AI-powered. UniRig (MIT, SIGGRAPH 2025) predicts the full "
                        + "skeleton from the mesh geometry using a local ML model — no "
                        + "template or markers needed. The model (~1.4 GB) downloads once "
                        + "on first use and then runs entirely on your machine (offline). "
                        + "Falls back to the template rig if the model can't be loaded. "
                        + "Trained on Articulation-XL2.0 (CC-BY-4.0)."
                }
                // Marker hint — shown only for Pinocchio (the template backend),
                // mirroring the UniRig notice above.
                Text {
                    width: parent.width
                    visible: !rigIdle.isUnirig
                    wrapMode: Text.Wrap
                    color: PropertiesPanelController.textColor
                    opacity: 0.7
                    font.pixelSize: 9
                    text: "Use markers for a better fit (Mixamo-style): click the "
                        + "highlighted points on the mesh to anchor the joints, or "
                        + "skip them for a plain proportional template."
                }

                // ---- Template-only controls (hidden when UniRig is selected) ----
                // Skeleton type — used by Pinocchio (and as the UniRig fallback).
                Text {
                    visible: !rigIdle.isUnirig
                    text: "Skeleton type"
                    color: PropertiesPanelController.textColor
                    opacity: 0.8
                    font.pixelSize: 10
                }
                Flow {
                    width: parent.width
                    visible: !rigIdle.isUnirig
                    spacing: 4
                    RigSegments {
                        options: root.rigTemplates
                        index: root.rigTemplateIndex
                        onPicked: function(i) { root.rigTemplateIndex = i }
                    }
                }

                Flow {
                    width: parent.width
                    spacing: 6
                    RigButton {
                        // Markers are a humanoid TEMPLATE concept — UniRig predicts
                        // its own structure, so this is hidden for the ML backend.
                        visible: !rigIdle.isUnirig
                        label: "Place markers…"
                        buttonEnabled: rigCol.canRig && !AutoRigController.busy
                            && root.rigTemplates[root.rigTemplateIndex] === "humanoid"
                        onClicked: AutoRigController.beginMarkerPlacement(
                            root.rigUpAxes[root.rigUpAxisIndex])
                    }
                    RigButton {
                        // Label reflects the backend: AI prediction vs template embed.
                        label: AutoRigController.busy
                            ? "Rigging…"
                            : (rigIdle.isUnirig ? "Generate Rig (AI)" : "Auto-Rig (template)")
                        buttonEnabled: rigCol.canRig && !AutoRigController.busy
                        onClicked: root.runAutoRig()
                    }
                }

                RigCheckbox {
                    label: "Also compute skin weights"
                    checked: root.rigAlsoSkin
                    onToggled: root.rigAlsoSkin = !root.rigAlsoSkin
                }

                // Advanced options toggle — template-only (up-axis), hidden for UniRig.
                RigCheckbox {
                    visible: !rigIdle.isUnirig
                    label: "Advanced options"
                    checked: root.rigShowAdvanced
                    onToggled: root.rigShowAdvanced = !root.rigShowAdvanced
                }

                Column {
                    width: parent.width
                    spacing: 6
                    visible: root.rigShowAdvanced && !rigIdle.isUnirig

                    Text {
                        text: "Up axis (+Y is the in-app default)"
                        color: PropertiesPanelController.textColor
                        opacity: 0.8
                        font.pixelSize: 10
                    }
                    RigSegments {
                        options: root.rigUpAxes
                        index: root.rigUpAxisIndex
                        onPicked: function(i) { root.rigUpAxisIndex = i }
                    }
                }
            }

            // ── UniRig worker progress (busy) ─────────────────────────────
            // Shown while the ML rig runs on the worker thread. Determinate bar
            // over the decode steps, a "Downloading model…" phase before the
            // first step, and a Cancel button. The whole section stays mounted
            // (the idle controls above are gated on !busy where it matters via
            // buttonEnabled) so deleting the model mid-run isn't possible — the
            // worker holds the only handle and Cancel is the sanctioned exit.
            Column {
                width: parent.width - 16
                spacing: 6
                visible: AutoRigController.busy

                Text {
                    width: parent.width
                    wrapMode: Text.Wrap
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 10
                    text: AutoRigController.rigDownloading
                        ? "Downloading UniRig model (~1.4 GB, first use only)…"
                        : (AutoRigController.rigTotal > 0
                            ? ("Predicting skeleton… step "
                               + AutoRigController.rigProgress + " / "
                               + AutoRigController.rigTotal)
                            : "Preparing…")
                }

                // Determinate while decoding; indeterminate-looking (full-width
                // track, no fill) during download / prepare where we have no count.
                Rectangle {
                    width: parent.width
                    height: 6
                    radius: 3
                    color: PropertiesPanelController.inputColor
                    border.color: PropertiesPanelController.borderColor
                    border.width: 1
                    Rectangle {
                        height: parent.height - 2
                        y: 1; x: 1
                        radius: 2
                        color: PropertiesPanelController.highlightColor
                        readonly property real frac:
                            AutoRigController.rigTotal > 0
                                ? Math.max(0, Math.min(1,
                                    AutoRigController.rigProgress
                                    / AutoRigController.rigTotal))
                                : 0
                        width: (parent.width - 2) * frac
                        Behavior on width { NumberAnimation { duration: 120 } }
                    }
                }

                RigButton {
                    label: "Cancel"
                    buttonEnabled: true
                    onClicked: AutoRigController.cancelRig()
                }
            }

            // ── Status line (both modes) ──────────────────────────────────
            Text {
                width: parent.width - 16
                visible: root.rigStatus.length > 0
                wrapMode: Text.Wrap
                font.pixelSize: 10
                text: root.rigStatus
                color: root.rigStatusError ? "#cc4444" : "#3a8c3a"
            }
        }
    }

    // ---- Skeleton Tools Content (Animation mode) ----
    // Per-entity skeleton/bone visualization toggles, sourced from
    // PropertiesPanelController.skeletonData() (skeleton-bearing entities,
    // independent of animation clips). Refreshes on selectionChanged /
    // animationStateChanged so a just-auto-rigged mesh shows up immediately.
    Component {
        id: skeletonToolsComponent

        Column {
            id: skeletonToolsCol
            width: parent ? parent.width : 200
            padding: 8
            spacing: 8

            property var skelGroups: PropertiesPanelController.skeletonData()
            Connections {
                target: PropertiesPanelController
                function onAnimationStateChanged() {
                    skeletonToolsCol.skelGroups = PropertiesPanelController.skeletonData()
                }
                function onSelectionChanged() {
                    skeletonToolsCol.skelGroups = PropertiesPanelController.skeletonData()
                }
            }

            Text {
                width: parent.width - 16
                wrapMode: Text.Wrap
                opacity: 0.8
                color: PropertiesPanelController.textColor
                font.pixelSize: 10
                text: "Visualize the skeleton and per-vertex bone weights for the "
                    + "selected skinned mesh."
            }

            Repeater {
                model: skeletonToolsCol.skelGroups
                delegate: Column {
                    required property var modelData
                    width: skeletonToolsCol.width - 16
                    spacing: 4

                    // Entity name (only worth showing when multiple are selected).
                    Text {
                        visible: skeletonToolsCol.skelGroups.length > 1
                        text: modelData.entity
                        color: PropertiesPanelController.textColor
                        opacity: 0.7
                        font.pixelSize: 10
                    }

                    Row {
                        spacing: 8
                        Rectangle {
                            width: 14; height: 14; anchors.verticalCenter: parent.verticalCenter
                            border.color: PropertiesPanelController.borderColor; border.width: 1; radius: 2
                            color: modelData.showSkeleton ? PropertiesPanelController.highlightColor : PropertiesPanelController.controlBgColor
                            Text { anchors.centerIn: parent; text: modelData.showSkeleton ? "✓" : ""; color: "white"; font.pixelSize: 10 }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    PropertiesPanelController.toggleSkeletonDebug(modelData.entity, !modelData.showSkeleton)
                                    skeletonToolsCol.skelGroups = PropertiesPanelController.skeletonData()
                                }
                            }
                        }
                        Text { text: "Skeleton"; color: PropertiesPanelController.textColor; font.pixelSize: 11; anchors.verticalCenter: parent.verticalCenter }

                        Rectangle {
                            width: 14; height: 14; anchors.verticalCenter: parent.verticalCenter
                            border.color: PropertiesPanelController.borderColor; border.width: 1; radius: 2
                            color: modelData.showWeights ? PropertiesPanelController.highlightColor : PropertiesPanelController.controlBgColor
                            Text { anchors.centerIn: parent; text: modelData.showWeights ? "✓" : ""; color: "white"; font.pixelSize: 10 }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    PropertiesPanelController.toggleBoneWeights(modelData.entity, !modelData.showWeights)
                                    skeletonToolsCol.skelGroups = PropertiesPanelController.skeletonData()
                                }
                            }
                        }
                        Text { text: "Weights"; color: PropertiesPanelController.textColor; font.pixelSize: 11; anchors.verticalCenter: parent.verticalCenter }
                    }
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

            // AI "Select by Part" (#410) — predicts head/torso/arm/leg labels
            // and selects all faces matching the selected face's part (or the
            // largest part if nothing is selected). Runs the (slow first-use
            // model download + ONNX inference) on a WORKER thread so the UI stays
            // responsive; progress + result surface via EditModeController.
            property string selectByPartStatus: ""
            property bool selectByPartError: false

            Connections {
                target: EditModeController
                function onSegmentFinished(status, isError) {
                    editToolsCol.selectByPartStatus = status
                    editToolsCol.selectByPartError = isError
                }
            }

            Rectangle {
                id: sbpButton
                width: parent.width - 16; height: 26; radius: 3
                opacity: EditModeController.segmentBusy ? 0.5 : 1.0
                color: sbpMouse.pressed ? Qt.darker(PropertiesPanelController.highlightColor, 1.2)
                     : (sbpMouse.containsMouse || sbpButton.activeFocus)
                         ? Qt.lighter(PropertiesPanelController.highlightColor, 1.1)
                         : PropertiesPanelController.highlightColor
                // Keyboard accessibility: focusable, activatable with Space/Return,
                // and exposed to assistive tech as a button.
                activeFocusOnTab: !EditModeController.segmentBusy
                border.color: sbpButton.activeFocus ? "white"
                                                    : PropertiesPanelController.borderColor
                border.width: sbpButton.activeFocus ? 1 : 0
                Accessible.role: Accessible.Button
                Accessible.name: "Select by Part (AI)"
                Accessible.description: "Predict mesh parts and select the part under the current face selection"
                Accessible.onPressAction: sbpButton.activate()
                function activate() {
                    if (EditModeController.segmentBusy)
                        return
                    editToolsCol.selectByPartStatus = ""
                    EditModeController.selectByPart()
                }
                Keys.onPressed: function(event) {
                    if (event.key === Qt.Key_Space || event.key === Qt.Key_Return
                            || event.key === Qt.Key_Enter) {
                        sbpButton.activate()
                        event.accepted = true
                    }
                }
                Text { anchors.centerIn: parent
                       text: EditModeController.segmentBusy ? "Segmenting…" : "Select by Part (AI)"
                       color: "white"; font.pixelSize: 11 }
                MouseArea {
                    id: sbpMouse; anchors.fill: parent; hoverEnabled: true
                    enabled: !EditModeController.segmentBusy
                    cursorShape: EditModeController.segmentBusy ? Qt.ForbiddenCursor
                                                               : Qt.PointingHandCursor
                    onClicked: {
                        sbpButton.forceActiveFocus()
                        sbpButton.activate()
                    }
                }
            }

            // Progress: "Downloading model…" phase, then a determinate inference
            // bar, plus Cancel — shown only while the worker runs.
            Column {
                width: parent.width - 16
                spacing: 4
                visible: EditModeController.segmentBusy
                Text {
                    width: parent.width; wrapMode: Text.Wrap; font.pixelSize: 9
                    color: PropertiesPanelController.textColor
                    text: EditModeController.segmentDownloading
                        ? "Downloading segmentation model (first use only)…"
                        : (EditModeController.segmentTotal > 0
                            ? ("Segmenting… step " + EditModeController.segmentProgress
                               + " / " + EditModeController.segmentTotal)
                            : "Preparing…")
                }
                Rectangle {
                    width: parent.width; height: 6; radius: 3
                    color: PropertiesPanelController.inputColor
                    border.color: PropertiesPanelController.borderColor; border.width: 1
                    Rectangle {
                        height: parent.height - 2; y: 1; x: 1; radius: 2
                        color: PropertiesPanelController.highlightColor
                        readonly property real frac: EditModeController.segmentTotal > 0
                            ? Math.max(0, Math.min(1, EditModeController.segmentProgress
                                                      / EditModeController.segmentTotal))
                            : 0
                        width: (parent.width - 2) * frac
                        Behavior on width { NumberAnimation { duration: 120 } }
                    }
                }
                Rectangle {
                    width: cancelSbpTxt.implicitWidth + 16; height: 18; radius: 3
                    color: cancelSbpMa.containsMouse ? PropertiesPanelController.highlightColor
                                                     : PropertiesPanelController.headerColor
                    border.color: PropertiesPanelController.borderColor
                    Text { id: cancelSbpTxt; anchors.centerIn: parent; text: "Cancel"
                           font.pixelSize: 10; color: PropertiesPanelController.textColor }
                    MouseArea {
                        id: cancelSbpMa; anchors.fill: parent; hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: EditModeController.cancelSegment()
                    }
                }
            }

            Text {
                visible: editToolsCol.selectByPartStatus.length > 0
                         && !EditModeController.segmentBusy
                text: editToolsCol.selectByPartStatus
                color: editToolsCol.selectByPartError ? "#e06c6c"
                                                       : PropertiesPanelController.textColor
                font.pixelSize: 9; opacity: 0.85
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

            // Issue #401: Quad retopology via triangle pairing. Lives
            // in Edit Mode since this is a topology operation (turns
            // pairs of triangles into quads via the n-gon binding) —
            // not a material/texture operation.
            Rectangle {
                width: Math.min(parent.width - 16, retopoLabel.implicitWidth + 16)
                height: 26
                radius: 3
                opacity: QuadRetopoController.hasSelection ? 1.0 : 0.45
                color: retopoMa.containsMouse && QuadRetopoController.hasSelection
                    ? PropertiesPanelController.highlightColor
                    : PropertiesPanelController.headerColor
                border.color: PropertiesPanelController.borderColor
                border.width: 1

                Text {
                    id: retopoLabel
                    anchors.centerIn: parent
                    text: "Quad Retopology…"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 11
                }
                MouseArea {
                    id: retopoMa
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: QuadRetopoController.hasSelection
                    cursorShape: QuadRetopoController.hasSelection
                        ? Qt.PointingHandCursor : Qt.ForbiddenCursor
                    onClicked: root.openQuadRetopoDialog()
                    ToolTip.visible: containsMouse
                    ToolTip.delay: 500
                    ToolTip.text: QuadRetopoController.hasSelection
                        ? "Pair adjacent triangles into quad-dominant topology. Skin weights survive (no new vertices)."
                        : "Select a mesh first."
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

    // ---- Paint Brush Content (DEPRECATED — kept dormant) ----
    // Brush color/radius/strength/falloff now live exclusively on the
    // toolbar paint-brush popup. The component below is no longer
    // wired into any CollapsibleSection; left here only because
    // removing it would churn 100+ lines of unrelated diff.
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
                    from: 0.001; to: 2.0; stepSize: 0.001
                    value: brushCol.brushRadius
                    onMoved: TexturePaintController.setBrushRadius(value)
                }
                Text {
                    text: brushCol.brushRadius.toFixed(3)
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

    // ---- UV Edit (Material mode) ----
    Component {
        id: uvEditComponent

        Column {
            id: uvEditCol
            width: parent ? parent.width : 200
            padding: 8
            spacing: 8

            Component.onCompleted: {
                UVEditorController.setInspectorEmbedded(true)
                UVEditorController.refresh()
            }
            Component.onDestruction: UVEditorController.setInspectorEmbedded(false)

            Text {
                width: parent.width - 16
                text: "Edit UV layout for the selected mesh. Mark seams in Edit Mode " +
                      "(edge selection), then pin, split, sew, and unwrap here."
                color: PropertiesPanelController.textColor
                font.pixelSize: 10
                opacity: 0.75
                wrapMode: Text.Wrap
            }

            Row {
                spacing: 6
                Rectangle {
                    width: 150
                    height: 24
                    radius: 3
                    color: uvWinMa.containsMouse
                        ? Qt.lighter(PropertiesPanelController.panelColor, 1.5)
                        : PropertiesPanelController.headerColor
                    border.color: PropertiesPanelController.borderColor
                    border.width: 1
                    Text {
                        anchors.centerIn: parent
                        text: "\u2922  Open Editor Window"
                        color: PropertiesPanelController.textColor
                        font.pixelSize: 10
                    }
                    MouseArea {
                        id: uvWinMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: UVEditorController.openEditorWindow()
                    }
                }
                Text {
                    text: UVEditorController.statusText
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 10
                    opacity: 0.8
                    anchors.verticalCenter: parent.verticalCenter
                    elide: Text.ElideRight
                    width: Math.max(0, uvEditCol.width - 180)
                }
            }

            Row {
                spacing: 4
                visible: UVEditorController.hasMesh
                Text {
                    text: "Seams"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 10
                    opacity: 0.65
                    anchors.verticalCenter: parent.verticalCenter
                    width: 44
                }
                Repeater {
                    model: [
                        { label: "Pin", fn: function() { UVEditorController.pinSelection() } },
                        { label: "Unpin", fn: function() { UVEditorController.unpinSelection() } },
                        { label: "Sew", fn: function() { UVEditorController.sewSelectedEdges() } },
                        { label: "Split", fn: function() { UVEditorController.splitSelectedEdges() } },
                        { label: "Unwrap", fn: function() { UVEditorController.unwrapSelectedFaces() } }
                    ]
                    delegate: Rectangle {
                        width: modelData.label === "Unpin" ? 38 : (modelData.label === "Unwrap" ? 48 : 32)
                        height: 22
                        radius: 3
                        color: uvToolMa.pressed ? Qt.darker(PropertiesPanelController.inputColor, 1.12)
                             : uvToolMa.containsMouse ? Qt.lighter(PropertiesPanelController.inputColor, 1.08)
                             : PropertiesPanelController.inputColor
                        border.color: PropertiesPanelController.borderColor
                        border.width: 1
                        Text {
                            anchors.centerIn: parent
                            text: modelData.label
                            color: PropertiesPanelController.textColor
                            font.pixelSize: 9
                        }
                        MouseArea {
                            id: uvToolMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: modelData.fn()
                        }
                    }
                }
            }

            Row {
                spacing: 4
                visible: UVEditorController.hasMesh
                Text {
                    text: "Projection"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 10
                    opacity: 0.65
                    anchors.verticalCenter: parent.verticalCenter
                    width: 44
                }
                Repeater {
                    model: [
                        { label: "View", fn: function() { UVEditorController.projectUvFromView() } },
                        { label: "Box", fn: function() { UVEditorController.projectUvBox(1.0) } },
                        { label: "Cyl", fn: function() { UVEditorController.projectUvCylinder(1, 1.0) } },
                        { label: "Sph", fn: function() { UVEditorController.projectUvSphere(1) } },
                        { label: "Reset", fn: function() { UVEditorController.resetUvBox() } }
                    ]
                    delegate: Rectangle {
                        width: modelData.label === "Reset" ? 34 : 30
                        height: 22
                        radius: 3
                        color: uvProjMa.pressed ? Qt.darker(PropertiesPanelController.inputColor, 1.12)
                             : uvProjMa.containsMouse ? Qt.lighter(PropertiesPanelController.inputColor, 1.08)
                             : PropertiesPanelController.inputColor
                        border.color: PropertiesPanelController.borderColor
                        border.width: 1
                        Text {
                            anchors.centerIn: parent
                            text: modelData.label
                            color: PropertiesPanelController.textColor
                            font.pixelSize: 9
                        }
                        MouseArea {
                            id: uvProjMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: modelData.fn()
                        }
                    }
                }
            }

            Loader {
                id: uvPanelLoader
                width: parent.width - 16
                height: 300
                source: "qrc:/UVEditor/UVEditorPanel.qml"
                onLoaded: {
                    if (item) {
                        item.embedded = true
                        item.width = uvPanelLoader.width
                        item.height = uvPanelLoader.height
                        item.focus = true
                    }
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
            property int paintTarget: TexturePaintController.paintTarget
            // image://paintbuffer/current?v=N — served by PaintBufferImageProvider.
            // Switching from PNG-encoded data URIs eliminates the per-stroke
            // blink (each new base64 string was a fresh load) and drops the
            // PNG encode + base64 churn off the main thread.
            property string previewUri: TexturePaintController.fullResPreviewUrl
            property string maskOverlayUri: TexturePaintController.maskOverlayDataUri
            property bool hasMask: TexturePaintController.hasSelectionMask
            property int maskCount: TexturePaintController.selectedPixelCount
            property real smartTolerance: TexturePaintController.smartSelectTolerance
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
                function onFullResPreviewChanged() {
                    texPaintCol.previewUri = TexturePaintController.fullResPreviewUrl
                }
                function onBrushToolChanged() {
                    texPaintCol.brushTool = TexturePaintController.brushTool
                }
                function onPaintTargetChanged() {
                    texPaintCol.paintTarget = TexturePaintController.paintTarget
                }
                function onHoveredUVChanged(u, v) {
                    texPaintCol.hoverU = u
                    texPaintCol.hoverV = v
                }
                function onSmartSelectChanged() {
                    texPaintCol.maskOverlayUri = TexturePaintController.maskOverlayDataUri
                    texPaintCol.hasMask = TexturePaintController.hasSelectionMask
                    texPaintCol.maskCount = TexturePaintController.selectedPixelCount
                    texPaintCol.smartTolerance = TexturePaintController.smartSelectTolerance
                }
            }

            Text {
                width: parent.width - 16
                text: "Paint into the model — either as vertex colors " +
                      "(polypaint, exported with the mesh) or into the " +
                      "BaseColor texture. Pick the target below."
                color: PropertiesPanelController.textColor
                font.pixelSize: 10; opacity: 0.7
                wrapMode: Text.Wrap
            }

            // Paint target switch \u2014 picking a target also enables paint.
            // Three states: Off / Vertex / Texture. Defaults to Vertex.
            Row {
                spacing: 4
                Text {
                    text: "Paint:"
                    color: PropertiesPanelController.textColor; font.pixelSize: 11
                    anchors.verticalCenter: parent.verticalCenter
                    width: 50
                }
                Repeater {
                    model: [
                        { target: -1, label: "Off" },
                        { target: 1,  label: "Vertex" },
                        { target: 0,  label: "Texture" }
                    ]
                    Rectangle {
                        width: 70; height: 26; radius: 3
                        property bool isActive: modelData.target === -1
                            ? !texPaintCol.paintOn
                            : (texPaintCol.paintOn && texPaintCol.paintTarget === modelData.target)
                        color: isActive
                            ? PropertiesPanelController.highlightColor
                            : (tgtMa.containsMouse ? Qt.lighter(PropertiesPanelController.panelColor, 1.5)
                                                   : PropertiesPanelController.headerColor)
                        border.color: PropertiesPanelController.borderColor; border.width: 1
                        Text {
                            anchors.centerIn: parent
                            text: modelData.label
                            color: PropertiesPanelController.textColor
                            font.pixelSize: 10
                        }
                        MouseArea {
                            id: tgtMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (modelData.target === -1) {
                                    TexturePaintController.texturePaintEnabled = false
                                } else {
                                    TexturePaintController.paintTarget = modelData.target
                                    if (!texPaintCol.paintOn)
                                        TexturePaintController.texturePaintEnabled = true
                                }
                            }
                        }
                    }
                }
            }

            // Tool selector deliberately not in the right panel anymore.
            // All paint tools (Paint / Bucket / Eraser / Pick / Smudge
            // / Wand) live in the left toolbar \u2014 see mainwindow.cpp
            // \u2014 so the user picks the tool with the same buttons
            // regardless of which target they're painting.

            // Smart-select panel. Visible whenever there's an active
            // session \u2014 tolerance is always meaningful, and once the
            // mask is non-empty the action buttons go live.
            Column {
                spacing: 6
                visible: texPaintCol.hasSession
                width: parent.width - 16

                // Status row only — wand tolerance is no longer a
                // separate control. The user clicks the mesh / 2D
                // thumbnail with the Wand tool and drags horizontally
                // mid-stroke; the controller scrubs the tolerance live
                // and re-selects at the press seed. The current value
                // is shown here so the user can see what they're at.
                Row {
                    spacing: 10
                    Text {
                        text: texPaintCol.hasMask
                            ? (texPaintCol.maskCount + " px selected")
                            : "Wand: drag horizontally while painting to adjust tolerance"
                        color: PropertiesPanelController.textColor
                        font.pixelSize: 10
                        opacity: 0.75
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        text: "tol " + Math.round(texPaintCol.smartTolerance * 100) + "%"
                        color: PropertiesPanelController.textColor
                        font.pixelSize: 10
                        opacity: 0.55
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }

                // Detached editor window launcher. Same paint buffer,
                // bigger canvas, real-time sync with the 3D viewport.
                Row {
                    spacing: 6
                    Rectangle {
                        width: 140; height: 24; radius: 3
                        color: editorMa.containsMouse
                            ? Qt.lighter(PropertiesPanelController.panelColor, 1.5)
                            : PropertiesPanelController.headerColor
                        border.color: PropertiesPanelController.borderColor; border.width: 1
                        Text {
                            anchors.centerIn: parent
                            text: "⤢  Open Editor Window"
                            color: PropertiesPanelController.textColor
                            font.pixelSize: 10
                        }
                        MouseArea {
                            id: editorMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: TexturePaintController.openEditorWindow()
                        }
                    }
                }

                // Action buttons: act on the current mask.
                Flow {
                    width: parent.width
                    spacing: 4
                    Repeater {
                        model: [
                            { label: "Fill FG",   action: "fillFG",   needsMask: true,  hint: "Replace selection with foreground color" },
                            { label: "Fill BG",   action: "fillBG",   needsMask: true,  hint: "Replace selection with background color" },
                            { label: "Delete",    action: "delete",   needsMask: true,  hint: "Clear selection to transparent" },
                            { label: "Invert",    action: "invert",   needsMask: false, hint: "Invert the selection" },
                            { label: "All",       action: "all",      needsMask: false, hint: "Select every pixel" },
                            { label: "None",      action: "none",     needsMask: true,  hint: "Clear the selection" }
                        ]
                        Rectangle {
                            width: 56; height: 24; radius: 3
                            color: actionMa.containsMouse
                                ? Qt.lighter(PropertiesPanelController.panelColor, 1.5)
                                : PropertiesPanelController.headerColor
                            opacity: (modelData.needsMask && !texPaintCol.hasMask) ? 0.45 : 1.0
                            border.color: PropertiesPanelController.borderColor; border.width: 1
                            Text {
                                anchors.centerIn: parent
                                text: modelData.label
                                color: PropertiesPanelController.textColor
                                font.pixelSize: 10
                            }
                            MouseArea {
                                id: actionMa
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                                enabled: !modelData.needsMask || texPaintCol.hasMask
                                ToolTip.text: modelData.hint
                                ToolTip.visible: containsMouse
                                ToolTip.delay: 400
                                onClicked: {
                                    if (modelData.action === "fillFG")
                                        TexturePaintController.fillMaskWithFG()
                                    else if (modelData.action === "fillBG")
                                        TexturePaintController.fillMaskWithBG()
                                    else if (modelData.action === "delete")
                                        TexturePaintController.deleteMaskPixels()
                                    else if (modelData.action === "invert")
                                        TexturePaintController.invertSelectionMask()
                                    else if (modelData.action === "all")
                                        TexturePaintController.selectAllMask()
                                    else if (modelData.action === "none")
                                        TexturePaintController.clearSelectionMask()
                                }
                            }
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

                // Smart-select / magic-wand mask overlay. Yellow tint on
                // the selected area + black outline at the boundary —
                // matches the marching-ants idea without the animation.
                Image {
                    id: maskOverlayImg
                    anchors.fill: parent
                    anchors.margins: 1
                    visible: texPaintCol.hasMask
                    opacity: 0.85
                    source: texPaintCol.maskOverlayUri
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

                // Brush footprint outline — circle for Round, square
                // for Square. Diameter / side comes from the controller's
                // UV-space radius (same scaling the painter uses) so the
                // outline matches the actual stamp.
                Rectangle {
                    id: brushOutline
                    visible: texPaintCol.hoverU >= 0 && texPaintCol.hoverV >= 0
                             && TexturePaintController.texturePaintRadiusUV > 0
                    color: "transparent"
                    border.color: "#ff3030"
                    border.width: 1
                    // Diameter in pixels of the 254x254 inner image area
                    // (parent is 256x256 with 1px margins on each side).
                    property real diameterPx: Math.max(2,
                        Math.round(TexturePaintController.texturePaintRadiusUV * 2 * (parent.width - 2)))
                    width: diameterPx
                    height: diameterPx
                    radius: TexturePaintController.brushShape === 1 ? 0 : diameterPx / 2
                    // Centred on the hover point. width/2 is the offset
                    // so the outline is symmetric around the crosshair.
                    x: 1 + Math.round(texPaintCol.hoverU * (parent.width - 2)) - diameterPx / 2
                    y: 1 + Math.round(texPaintCol.hoverV * (parent.height - 2)) - diameterPx / 2
                }

                MouseArea {
                    id: paintArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.CrossCursor
                    // Stop parent layouts from stealing the drag — QML
                    // scrollers / containers may grab the press as a
                    // flick gesture after a few pixels of motion,
                    // killing the brush stroke. preventStealing keeps
                    // the grab locked here.
                    preventStealing: true
                    property bool dragging: false

                    function toUV(mx, my) {
                        const W = paintArea.width
                        const H = paintArea.height
                        return Qt.point(Math.max(0, Math.min(1, mx / W)),
                                        Math.max(0, Math.min(1, my / H)))
                    }
                    onPositionChanged: function(m) {
                        const uv = toUV(m.x, m.y)
                        // While dragging: keep painting regardless of
                        // pressed/buttons state. onPressed sets the
                        // flag; onReleased / onExited / onCanceled
                        // clear it. Some Qt6/macOS edge cases zero
                        // m.buttons mid-drag which used to break the
                        // stroke after the first move.
                        if (dragging) {
                            TexturePaintController.updateStrokeUV(uv.x, uv.y)
                        } else {
                            TexturePaintController.setHoveredUV(uv.x, uv.y)
                        }
                    }
                    onExited: {
                        // Don't end the stroke if we're still mid-drag —
                        // the user may drag off and back onto the panel
                        // in one motion. onReleased fires globally
                        // (mouse is grabbed by the press) so we don't
                        // need a defensive end-stroke here.
                        if (!dragging) TexturePaintController.clearHoveredUV()
                    }
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
                    onCanceled: {
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

                // Explicit "write back to the source texture file on
                // disk" button. Painting is otherwise non-destructive
                // (the strokes live only in the in-memory paint buffer
                // and the EmbeddedTextureCache used by exports), so
                // the user has to click this to overwrite the original
                // asset. Strong-warning hover text so it isn't mistaken
                // for the safe "Save\u2026" (which writes a new file).
                Rectangle {
                    width: 130; height: 24; radius: 3
                    color: saveOrigMa.containsMouse
                        ? Qt.lighter(PropertiesPanelController.panelColor, 1.5)
                        : PropertiesPanelController.headerColor
                    border.color: PropertiesPanelController.borderColor; border.width: 1
                    opacity: texPaintCol.hasSession ? 1.0 : 0.4
                    Text {
                        anchors.centerIn: parent
                        text: "Save to Original"
                        color: PropertiesPanelController.textColor
                        font.pixelSize: 10
                    }
                    MouseArea {
                        id: saveOrigMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        enabled: texPaintCol.hasSession
                        ToolTip.text: "Overwrite the texture's source file on disk.\nCannot be undone outside the editor."
                        ToolTip.visible: containsMouse
                        ToolTip.delay: 400
                        onClicked: TexturePaintController.bakeToOriginalFile()
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
                        || EditorModeController.currentMode === EditorModeController.ValidationMode
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

            // Backend selector (meshoptimizer vs Ogre's MeshLodGenerator).
            // Default `meshopt`: preserves UV seams and skin weights via
            // attribute-aware simplify. `ogre` kept for legacy round-trip.
            // Issue #398.
            Row {
                spacing: 6
                width: parent.width - 16
                Text {
                    text: "Backend:"
                    color: PropertiesPanelController.textColor; font.pixelSize: 11
                    anchors.verticalCenter: parent.verticalCenter
                }
                ThemedComboBox {
                    id: lodBackendCombo
                    width: 90; height: 26
                    model: ["ogre", "meshopt"]
                    currentIndex: 0
                    font.pixelSize: 11
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
                            // The Q_INVOKABLE 2-arg overload is what QML
                            // sees; it defaults to Meshopt internally. To
                            // pick the Ogre legacy backend instead, route
                            // through the controller's `algo` getter.
                            MeshLodController.generateLodsWithAlgo(
                                lodCountSelector.value, reductions,
                                lodBackendCombo.currentText)
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

            // Force Qt Quick Controls inside this Component to inherit
            // the dark theme. The CollapsibleSection above is set to
            // `expanded: true` so this Component instantiates during
            // initial QML evaluation alongside the LOD section — that
            // alone is what stopped controls from falling back to the
            // macOS Aqua chrome on lazy load, but the explicit palette
            // is kept as belt-and-suspenders so future refactors that
            // re-introduce lazy loading don't silently regress.
            palette {
                window: ThemeManager.panelColor
                windowText: ThemeManager.textColor
                base: ThemeManager.inputColor
                text: ThemeManager.textColor
                button: ThemeManager.headerColor
                buttonText: ThemeManager.textColor
                highlight: ThemeManager.highlightColor
                highlightedText: "white"
                mid: ThemeManager.borderColor
            }

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
                onTriggered: MeshDecimatorController.previewReductionWithAlgo(
                    decimateContent.reduction, decimateBackendCombo.currentText)
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

            // Backend selector — same options as the LOD section.
            // Default `ogre`; `meshopt` exposes meshoptimizer's
            // attribute-aware simplify for callers that need UV-seam
            // / skin-weight preservation.
            Row {
                spacing: 6; width: parent.width - 16
                Text {
                    text: "Backend:"
                    color: PropertiesPanelController.textColor; font.pixelSize: 11
                    anchors.verticalCenter: parent.verticalCenter
                }
                ThemedComboBox {
                    id: decimateBackendCombo
                    width: 90; height: 26
                    model: ["ogre", "meshopt"]
                    currentIndex: 0
                    font.pixelSize: 11
                    // Re-run the preview when the backend flips so the
                    // viewport always reflects what Apply would commit.
                    onCurrentIndexChanged: {
                        if (decimateContent.reduction > 0.0) {
                            previewDebounce.restart()
                        }
                    }
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
                        onClicked: MeshDecimatorController.applyReductionWithAlgo(
                            decimateContent.reduction, decimateBackendCombo.currentText)
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
            property var materialPreviewUris: ({})

            function rebuildMaterialPreviews() {
                var uris = {}
                for (var i = 0; i < materialNames.length; ++i) {
                    var n = materialNames[i]
                    if (n && n.length > 0)
                        uris[n] = MaterialEditorQML.materialPreview(n)
                }
                materialPreviewUris = uris
            }

            // Refresh the list when materials are imported/created.
            function refreshMaterialList() {
                materialNames = MaterialEditorQML.getMaterialList()
                rebuildMaterialPreviews()
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
                function onMaterialApplied() { materialToolCol.rebuildMaterialPreviews() }
            }

            // Also refresh when the scene's material set may have changed —
            // a model load or selection change — so newly-loaded materials
            // appear without needing the manual Refresh button.
            Connections {
                target: PropertiesPanelController.sceneTreeModel
                ignoreUnknownSignals: true
                function onMaterialsChanged() { materialToolCol.refreshMaterialList() }
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
                                        source: (modelData && materialToolCol.materialPreviewUris[modelData])
                                            ? materialToolCol.materialPreviewUris[modelData]
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

            // Issue #400: Auto UV unwrap via xatlas. Operates on the
            // currently selected entity (not disk files), so the
            // button disables itself when nothing is selected.
            Rectangle {
                width: Math.min(parent.width - 16, uvLabel.implicitWidth + 16)
                height: 26
                radius: 3
                opacity: UvUnwrapController.hasSelection ? 1.0 : 0.45
                color: uvMa.containsMouse && UvUnwrapController.hasSelection
                    ? PropertiesPanelController.highlightColor
                    : PropertiesPanelController.headerColor
                border.color: PropertiesPanelController.borderColor
                border.width: 1

                Text {
                    id: uvLabel
                    anchors.centerIn: parent
                    text: "Auto UV Unwrap…"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 11
                }
                MouseArea {
                    id: uvMa
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: UvUnwrapController.hasSelection
                    cursorShape: UvUnwrapController.hasSelection
                        ? Qt.PointingHandCursor : Qt.ForbiddenCursor
                    onClicked: root.openUvUnwrapDialog()
                    ToolTip.visible: containsMouse
                    ToolTip.delay: 500
                    ToolTip.text: UvUnwrapController.hasSelection
                        ? "Generate non-overlapping UVs for the selected mesh via xatlas. Skin weights survive the seam splits."
                        : "Select a mesh first."
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
                property string previewSource: ""

                function schedulePreviewRefresh() {
                    previewRefreshTimer.restart()
                }

                function refreshPreviewSource() {
                    const mat = materialToolCol.selectedMaterialName
                    if (!mat || mat.length === 0) {
                        previewSource = ""
                        return
                    }
                    const size = Math.min(Math.floor(previewHost.width), 256)
                    if (size < 8)
                        return
                    const uri = MaterialEditorQML.interactiveMaterialPreview(
                        mat, size, previewHost.previewShape, previewHost.previewYaw)
                    if (uri && uri.length > 0)
                        previewSource = uri
                }

                Timer {
                    id: previewRefreshTimer
                    interval: 80
                    repeat: false
                    onTriggered: previewHost.refreshPreviewSource()
                }

                Connections {
                    target: materialToolCol
                    function onSelectedMaterialNameChanged() { previewHost.schedulePreviewRefresh() }
                }
                Connections {
                    target: MaterialEditorQML
                    function onMaterialApplied() { previewHost.schedulePreviewRefresh() }
                }
                onPreviewShapeChanged: schedulePreviewRefresh()
                onPreviewYawChanged: schedulePreviewRefresh()
                onWidthChanged: schedulePreviewRefresh()
                Component.onCompleted: schedulePreviewRefresh()

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
                        source: previewHost.previewSource
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

    // Phase 6 slice E: Texture Atlas dialog. Same lazy-load pattern
    // as TextureChannelPackerDialog / NormalMapGeneratorDialog above.
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

    // Issue #400: xatlas auto UV unwrap dialog. Same lazy-load pattern
    // as TextureAtlasDialog / TextureChannelPackerDialog above.
    Loader {
        id: uvUnwrapLoader
        active: false
        anchors.centerIn: parent
        source: "qrc:/MaterialEditorQML/UvUnwrapDialog.qml"
        onLoaded: if (item && item.open) item.open()
    }
    function openUvUnwrapDialog() {
        if (!uvUnwrapLoader.active) {
            uvUnwrapLoader.active = true
        } else if (uvUnwrapLoader.item) {
            uvUnwrapLoader.item.open()
        }
    }

    // Issue #401: triangle-pairing quad retopology dialog. Same
    // lazy-load idiom as UvUnwrapDialog.
    Loader {
        id: quadRetopoLoader
        active: false
        anchors.centerIn: parent
        source: "qrc:/MaterialEditorQML/QuadRetopoDialog.qml"
        onLoaded: if (item && item.open) item.open()
    }
    function openQuadRetopoDialog() {
        if (!quadRetopoLoader.active) {
            quadRetopoLoader.active = true
        } else if (quadRetopoLoader.item) {
            quadRetopoLoader.item.open()
        }
    }

    // Issue #402: inverse-distance skin weights dialog. Same lazy-
    // load idiom as the sibling dialogs.
    Loader {
        id: skinWeightsLoader
        active: false
        anchors.centerIn: parent
        source: "qrc:/MaterialEditorQML/SkinWeightsDialog.qml"
        onLoaded: if (item && item.open) item.open()
    }
    function openSkinWeightsDialog() {
        if (!skinWeightsLoader.active) {
            skinWeightsLoader.active = true
        } else if (skinWeightsLoader.item) {
            skinWeightsLoader.item.open()
        }
    }

    // Issue #407: native auto-rig now lives inline in the Inspector Rigging
    // section (riggingToolsComponent) — no modal dialog. The old AutoRigDialog
    // Loader / openAutoRigDialog() were removed.

    Loader {
        id: isometricSpritesLoader
        active: false
        anchors.centerIn: parent
        source: "qrc:/MaterialEditorQML/IsometricSpritesDialog.qml"
        onLoaded: if (item && item.open) item.open()
        onStatusChanged: {
            if (status === Loader.Error)
                console.warn("IsometricSpritesDialog failed to load")
        }
    }
    function openIsometricSpritesDialog() {
        if (!isometricSpritesLoader.active) {
            isometricSpritesLoader.active = true
        } else if (isometricSpritesLoader.item) {
            isometricSpritesLoader.item.open()
        } else if (isometricSpritesLoader.status === Loader.Error) {
            isometricSpritesLoader.active = false
            isometricSpritesLoader.active = true
        }
    }

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

    // ---- Asset Folder Scan (Validation mode) ----
    Component {
        id: assetScanComponent

        Column {
            width: parent ? parent.width : 200
            padding: 8
            spacing: 6

            Text {
                width: parent.width - 16
                wrapMode: Text.Wrap
                font.pixelSize: 10
                color: PropertiesPanelController.textColor
                opacity: 0.75
                text: "Pick an assets folder below (or open Asset Browser \u2192 Browse\u2026). "
                    + "Scan uses the same rules as qtmesh scan --target in a separate process."
            }

            Row {
                spacing: 6
                width: parent.width - 16
                Text {
                    text: "Profile:"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 11
                    anchors.verticalCenter: parent.verticalCenter
                    width: 44
                }
                ThemedComboBox {
                    id: profileCombo
                    width: parent.width - 50
                    model: AssetScanController.profileLabels
                    currentIndex: {
                        const idx = AssetScanController.profileIds.indexOf(AssetScanController.selectedProfileId)
                        return idx >= 0 ? idx : 0
                    }
                    onActivated: function(index) {
                        if (index >= 0 && index < AssetScanController.profileIds.length)
                            AssetScanController.selectedProfileId = AssetScanController.profileIds[index]
                    }
                    ToolTip.visible: hovered && AssetScanController.profileDescription.length > 0
                    ToolTip.text: AssetScanController.profileDescription
                    ToolTip.delay: 400
                }
            }

            Text {
                width: parent.width - 16
                visible: AssetScanController.profileDescription.length > 0
                wrapMode: Text.Wrap
                font.pixelSize: 10
                color: PropertiesPanelController.textColor
                opacity: 0.7
                text: AssetScanController.profileDescription
            }

            Row {
                spacing: 6
                width: parent.width - 16

                Rectangle {
                    width: parent.width - 66
                    height: 24
                    radius: 3
                    color: PropertiesPanelController.inputColor
                    border.color: PropertiesPanelController.borderColor
                    border.width: 1
                    clip: true

                    Text {
                        anchors.fill: parent
                        anchors.leftMargin: 6
                        anchors.rightMargin: 6
                        verticalAlignment: Text.AlignVCenter
                        text: AssetBrowserController.rootPath
                        color: PropertiesPanelController.textColor
                        font.pixelSize: 10
                        elide: Text.ElideLeft
                    }
                }

                Rectangle {
                    width: 60
                    height: 24
                    radius: 3
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: "Browse asset folder"
                    color: folderBrowseMouse.containsMouse || activeFocus
                        ? Qt.lighter(PropertiesPanelController.inputColor, 1.3)
                        : PropertiesPanelController.inputColor
                    border.color: activeFocus ? PropertiesPanelController.highlightColor
                                              : PropertiesPanelController.borderColor
                    border.width: 1
                    Text {
                        anchors.centerIn: parent
                        text: "Browse\u2026"
                        color: PropertiesPanelController.textColor
                        font.pixelSize: 10
                    }
                    MouseArea {
                        id: folderBrowseMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: AssetBrowserController.browseForDirectory()
                    }
                    Keys.onSpacePressed: folderBrowseMouse.clicked(null)
                    Keys.onReturnPressed: folderBrowseMouse.clicked(null)
                    Keys.onEnterPressed: folderBrowseMouse.clicked(null)
                }
            }

            Rectangle {
                width: parent.width - 16; height: 24; radius: 3
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: "Open Asset Browser panel"
                color: browserMouse.containsMouse || activeFocus
                    ? Qt.lighter(PropertiesPanelController.controlBgColor, 1.08)
                    : PropertiesPanelController.controlBgColor
                border.color: activeFocus ? PropertiesPanelController.highlightColor
                                          : PropertiesPanelController.borderColor
                border.width: 1
                Text {
                    anchors.centerIn: parent
                    text: "Open Asset Browser panel"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 10
                }
                MouseArea {
                    id: browserMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: root.revealBottomTool("assetBrowser")
                }
                Keys.onSpacePressed: browserMouse.clicked(null)
                Keys.onReturnPressed: browserMouse.clicked(null)
                Keys.onEnterPressed: browserMouse.clicked(null)
            }

            Rectangle {
                width: parent.width - 16; height: 28; radius: 3
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: "Scan asset folder"
                color: scanMouse.pressed ? Qt.darker(PropertiesPanelController.highlightColor, 1.2)
                     : (scanMouse.containsMouse || activeFocus)
                       ? Qt.lighter(PropertiesPanelController.highlightColor, 1.1)
                       : PropertiesPanelController.highlightColor
                opacity: AssetScanController.scanning ? 0.55 : 1.0
                Text {
                    anchors.centerIn: parent
                    text: AssetScanController.scanning ? "Scanning\u2026" : "Scan Asset Folder"
                    color: "white"
                    font.pixelSize: 12
                }
                MouseArea {
                    id: scanMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: !AssetScanController.scanning
                    onClicked: AssetScanController.scanFolder(AssetBrowserController.rootPath)
                }
                Keys.onSpacePressed: if (!AssetScanController.scanning) scanMouse.clicked(null)
                Keys.onReturnPressed: if (!AssetScanController.scanning) scanMouse.clicked(null)
                Keys.onEnterPressed: if (!AssetScanController.scanning) scanMouse.clicked(null)
            }

            Text {
                width: parent.width - 16
                visible: AssetScanController.scanning
                text: "Running qtmesh scan in background\u2026"
                color: PropertiesPanelController.textColor
                font.pixelSize: 11
                font.italic: true
            }

            Text {
                width: parent.width - 16
                visible: AssetScanController.hasResults && !AssetScanController.scanning
                wrapMode: Text.Wrap
                font.pixelSize: 10
                color: PropertiesPanelController.textColor
                text: "Scanned " + AssetScanController.summaryScanned
                    + " \u2022 passed " + AssetScanController.summaryPassed
                    + " \u2022 warnings " + AssetScanController.summaryWarnings
                    + " \u2022 errors " + AssetScanController.summaryErrors
            }

            Column {
                width: parent.width - 16
                spacing: 3
                visible: AssetScanController.hasResults
                    && !AssetScanController.scanning
                    && AssetScanController.findings.length > 0

                Repeater {
                    model: AssetScanController.findings

                    Row {
                        spacing: 6
                        width: parent.width

                        Text {
                            text: modelData.severity === "error" ? "\u2718" : "\u26A0"
                            color: modelData.severity === "error" ? "#e05050" : "#e0a030"
                            font.pixelSize: 13
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Text {
                            text: (modelData.file ? modelData.file + ": " : "") + modelData.message
                            color: PropertiesPanelController.textColor
                            font.pixelSize: 10
                            wrapMode: Text.Wrap
                            width: parent.width - 24
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }
                }
            }

            Text {
                id: assetScanFeedback
                width: parent.width - 16
                wrapMode: Text.Wrap
                font.pixelSize: 10
                color: "#c06060"
                text: ""

                Connections {
                    target: AssetScanController
                    function onError(msg) {
                        assetScanFeedback.color = "#c06060"
                        assetScanFeedback.text = msg
                    }
                    function onScanFinished(ok, message) {
                        assetScanFeedback.color = ok ? "#60c060" : "#c06060"
                        assetScanFeedback.text = message
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

            // Optimize Geometry button — runs the full export-time
            // pipeline (vertex cache + overdraw + vertex fetch) via
            // `ExportOptimizer`. Mutates the index + vertex buffers
            // of every selected entity but never changes the actual
            // geometry. Issue #399.
            Rectangle {
                width: parent.width - 16; height: 28; radius: 3
                visible: MeshValidator.hasCacheOptimization
                color: cacheMouse.pressed ? Qt.darker("#5090d0", 1.2)
                     : cacheMouse.containsMouse ? Qt.lighter("#5090d0", 1.2)
                     : "#5090d0"
                Text { anchors.centerIn: parent
                       text: "Optimize Geometry (cache + overdraw + fetch)"
                       color: "white"; font.pixelSize: 11 }
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

            // ── Generate from text (#411, experimental) ──────────────────────
            // Lives in the Animations group and shows for any skeleton-bearing
            // selection (incl. a freshly auto-rigged mesh with no clips yet).
            Text {
                text: "Generate from text (experimental):"
                color: PropertiesPanelController.textColor; font.pixelSize: 11
            }
            Row {
                width: parent.width - 16; spacing: 6
                Rectangle {
                    width: parent.width - genBtn.width - 6; height: 24; radius: 3
                    color: PropertiesPanelController.inputColor
                    border.color: genPromptIn.activeFocus ? PropertiesPanelController.highlightColor
                                                           : PropertiesPanelController.borderColor
                    TextInput {
                        id: genPromptIn
                        anchors.fill: parent; anchors.leftMargin: 6; anchors.rightMargin: 6
                        verticalAlignment: TextInput.AlignVCenter
                        color: PropertiesPanelController.textColor; font.pixelSize: 11
                        clip: true
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: !genPromptIn.text && !genPromptIn.activeFocus
                            text: "e.g. walk, run, jump, wave…"
                            color: PropertiesPanelController.textColor; opacity: 0.4; font.pixelSize: 11
                        }
                        onAccepted: genBtn.run()
                    }
                }
                Rectangle {
                    id: genBtn
                    width: 74; height: 24; radius: 3
                    opacity: genBtnBusy ? 0.5 : 1.0
                    property bool genBtnBusy: false
                    color: genMa.pressed ? Qt.darker(PropertiesPanelController.highlightColor, 1.2)
                         : genMa.containsMouse ? Qt.lighter(PropertiesPanelController.highlightColor, 1.1)
                         : PropertiesPanelController.highlightColor
                    function run() {
                        if (genBtnBusy || !genPromptIn.text.trim()) return
                        genBtnBusy = true
                        genStatus.text = useModelChk.checked
                            ? "Generating (experimental model)…"
                            : "Generating… (first use downloads the motion library)"
                        genStatus.isError = false
                        AnimationControlController.generateMotion(genPromptIn.text, 0.0,
                                                                  useModelChk.checked)
                        genBtnBusy = false
                        // generateMotion adds an AnimationState synchronously, but
                        // it lives on AnimationControlController — the Inspector
                        // list (PropertiesPanelController.animationData) won't know
                        // until something re-queries. Refresh it directly so the
                        // new clip appears without reselecting the entity.
                        refreshAnimData()
                    }
                    Text { anchors.centerIn: parent; text: "Generate"; color: "white"; font.pixelSize: 11 }
                    MouseArea { id: genMa; anchors.fill: parent; hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor; onClicked: genBtn.run() }
                }
            }
            // Opt into the EXPERIMENTAL trained model (falls back to the template
            // library automatically). Default OFF = the reliable template retarget.
            Row {
                spacing: 6
                Rectangle {
                    id: useModelChk
                    property bool checked: false
                    width: 14; height: 14; radius: 2
                    anchors.verticalCenter: parent.verticalCenter
                    color: checked ? PropertiesPanelController.highlightColor
                                   : PropertiesPanelController.inputColor
                    border.color: PropertiesPanelController.borderColor
                    Text { anchors.centerIn: parent; visible: parent.checked
                           text: "✓"; color: "white"; font.pixelSize: 10 }
                    MouseArea { anchors.fill: parent
                                onClicked: useModelChk.checked = !useModelChk.checked }
                }
                Text {
                    text: "Use trained model (experimental)"
                    color: PropertiesPanelController.textColor; font.pixelSize: 10
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
            Text {
                id: genStatus
                property bool isError: false
                visible: text.length > 0
                width: parent.width - 16; wrapMode: Text.Wrap; font.pixelSize: 9; opacity: 0.85
                color: isError ? "#e06c6c" : PropertiesPanelController.textColor
            }
            Connections {
                target: AnimationControlController
                function onGenerateMotionStatus(message, isError) {
                    genStatus.text = message; genStatus.isError = isError
                }
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

                                    // Delete animation: trash icon → inline confirm (✓/✗) to avoid
                                    // accidental loss. Deleting is irreversible (removes the clip from
                                    // the skeleton), so the click ARMS a confirm rather than deleting.
                                    Rectangle {
                                        id: trashBtn
                                        property bool confirming: false
                                        width: confirming ? 40 : 20; height: 18; radius: 3
                                        anchors.verticalCenter: parent.verticalCenter
                                        color: "transparent"
                                        // idle: trash icon
                                        Text {
                                            anchors.centerIn: parent; visible: !trashBtn.confirming
                                            text: "🗑"; font.pixelSize: 12
                                            color: trashMouse.containsMouse ? "#e06c6c"
                                                 : PropertiesPanelController.textColor
                                        }
                                        MouseArea {
                                            id: trashMouse; anchors.fill: parent; hoverEnabled: true
                                            visible: !trashBtn.confirming
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: trashBtn.confirming = true
                                        }
                                        // armed: confirm (delete) / cancel
                                        Row {
                                            anchors.centerIn: parent; spacing: 4
                                            visible: trashBtn.confirming
                                            Text {
                                                text: "✓"; color: "#e06c6c"; font.pixelSize: 13; font.bold: true
                                                MouseArea { anchors.fill: parent; anchors.margins: -3
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: {
                                                        // emits animationStateChanged → the section's
                                                        // Connections refreshes the list automatically.
                                                        PropertiesPanelController.deleteAnimation(grp.entity, modelData.name)
                                                        trashBtn.confirming = false
                                                    }
                                                }
                                            }
                                            Text {
                                                text: "✗"; color: PropertiesPanelController.textColor; font.pixelSize: 13
                                                MouseArea { anchors.fill: parent; anchors.margins: -3
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: trashBtn.confirming = false
                                                }
                                            }
                                        }
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

                        // (Skeleton / Weights viz toggles moved to the dedicated
                        // "Skeleton" section so they surface for skinned meshes
                        // regardless of whether they have animation clips.)

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

            // ---- Morph Targets / Blend Shapes (slice A2) ----
            // Per-pose weight sliders, sourced from MorphAnimationManager.
            // Lives at the bottom of the Animations section, outside the
            // per-entity repeater above — morph data is read from the
            // SelectionSet's first entity to keep the surface focused.
            // Authoring (add/rename/delete) lands in A3.
            Rectangle {
                width: parent.width - 16
                visible: morphCol.targetCount > 0
                height: morphCol.implicitHeight + 12
                color: PropertiesPanelController.headerColor
                border.color: PropertiesPanelController.borderColor
                border.width: 1
                radius: 3

                Column {
                    id: morphCol
                    anchors.fill: parent
                    anchors.margins: 6
                    spacing: 4

                    // Defensive `|| []` so an unexpected null return
                    // doesn't crash the binding — the manager currently
                    // always returns a QStringList, but contracts drift.
                    property var targets: MorphAnimationManager.morphTargetsForSelection() || []
                    property int targetCount: targets.length
                    property string filter: ""
                    // Bumped on `morphWeightChanged`; sliders bind their
                    // `value` to a function call gated on this counter so
                    // weight changes from any code path (Reset all,
                    // dope-sheet scrubs in later slices, MCP, etc.) flow
                    // back into the UI rather than going stale until the
                    // delegate is recreated.
                    property int weightTick: 0

                    Connections {
                        target: MorphAnimationManager
                        function onMorphTargetsChanged() {
                            morphCol.targets = MorphAnimationManager.morphTargetsForSelection() || []
                            morphCol.weightTick = morphCol.weightTick + 1
                        }
                        function onMorphWeightChanged(entity, name, weight) {
                            morphCol.weightTick = morphCol.weightTick + 1
                        }
                    }

                    Row {
                        spacing: 4
                        width: parent.width
                        Text {
                            text: "Morph Targets (" + morphCol.targetCount + ")"
                            color: PropertiesPanelController.textColor
                            font.pixelSize: 11
                            font.bold: true
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Item { width: parent.width - 320; height: 1 }
                        // Add from current edit — captures the user's current
                        // edit-mode geometry minus the bind-pose baseline as
                        // a new morph target. Disabled (greyed out, forbidden
                        // cursor) when outside edit mode because
                        // EditableSubMesh::originalPositions is only
                        // populated by EditModeController and the C++ method
                        // would return false anyway.
                        Rectangle {
                            id: addBtn
                            property bool canAddFromEdit: EditModeController.editModeActive
                            width: 56; height: 20; radius: 3
                            opacity: canAddFromEdit ? 1.0 : 0.45
                            color: addMa.containsMouse && canAddFromEdit
                                   ? Qt.lighter(PropertiesPanelController.headerColor, 1.3)
                                   : PropertiesPanelController.controlBgColor
                            border.color: PropertiesPanelController.borderColor
                            anchors.verticalCenter: parent.verticalCenter
                            Text {
                                anchors.centerIn: parent
                                text: "+ Add…"
                                color: PropertiesPanelController.textColor
                                font.pixelSize: 9
                            }
                            MouseArea {
                                id: addMa
                                anchors.fill: parent
                                hoverEnabled: true
                                enabled: addBtn.canAddFromEdit
                                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ForbiddenCursor
                                onClicked: {
                                    addNameField.text = ""
                                    addError.text = ""
                                    addNamePopup.open()
                                }
                                ToolTip.visible: containsMouse && !enabled
                                ToolTip.text: "Enter Edit Mode (Tab) to add morph targets from current edit."
                            }
                        }
                        // Reset all: walks every target and sets weight to 0.
                        Rectangle {
                            width: 60; height: 20; radius: 3
                            color: resetMa.containsMouse
                                   ? Qt.lighter(PropertiesPanelController.headerColor, 1.3)
                                   : PropertiesPanelController.controlBgColor
                            border.color: PropertiesPanelController.borderColor
                            anchors.verticalCenter: parent.verticalCenter
                            Text {
                                anchors.centerIn: parent
                                text: "Reset all"
                                color: PropertiesPanelController.textColor
                                font.pixelSize: 9
                            }
                            MouseArea {
                                id: resetMa
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    for (var i = 0; i < morphCol.targets.length; ++i)
                                        MorphAnimationManager.setWeightForSelection(morphCol.targets[i], 0)
                                }
                            }
                        }
                    }

                    // Inline name-entry popup for "Add from edit…". Kept
                    // simple (no styled component) so a misbehaving custom
                    // dialog can't break the rest of the panel — Popup is
                    // a built-in Qt Quick Controls primitive with no
                    // singleton dependencies.
                    Popup {
                        id: addNamePopup
                        modal: true
                        focus: true
                        width: 240
                        contentItem: Column {
                            spacing: 6
                            Text {
                                text: "New morph target name:"
                                color: PropertiesPanelController.textColor
                                font.pixelSize: 11
                            }
                            TextField {
                                id: addNameField
                                width: 220
                                font.pixelSize: 11
                                onAccepted: addConfirmMa.confirm()
                                onTextChanged: addError.text = ""
                                Component.onCompleted: forceActiveFocus()
                            }
                            // Inline error: shown when the C++ side rejects
                            // the request (duplicate name, no vertex moved,
                            // not in edit mode, …). We deliberately keep the
                            // popup open so the user can fix the input
                            // without retyping.
                            Text {
                                id: addError
                                text: ""
                                visible: text.length > 0
                                color: "#d65d5d"
                                font.pixelSize: 10
                                width: 220
                                wrapMode: Text.Wrap
                            }
                            Row {
                                spacing: 6
                                Rectangle {
                                    width: 60; height: 20; radius: 3
                                    color: addConfirmMa.containsMouse
                                           ? Qt.lighter(PropertiesPanelController.headerColor, 1.3)
                                           : PropertiesPanelController.controlBgColor
                                    border.color: PropertiesPanelController.borderColor
                                    Text { anchors.centerIn: parent; text: "Save"; color: PropertiesPanelController.textColor; font.pixelSize: 10 }
                                    MouseArea {
                                        id: addConfirmMa
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        function confirm() {
                                            var n = addNameField.text.trim()
                                            if (n.length === 0) {
                                                addError.text = "Name cannot be empty."
                                                return
                                            }
                                            if (!EditModeController.editModeActive) {
                                                addError.text = "Enter Edit Mode (Tab) before saving."
                                                return
                                            }
                                            var ok = MorphAnimationManager.addMorphTargetFromCurrentEdit(n)
                                            if (ok) {
                                                addNamePopup.close()
                                            } else {
                                                // C++ rejected — likely name collision or
                                                // no vertex moved vs the bind baseline.
                                                addError.text = "Couldn't save: name already in use, or no vertex was edited."
                                            }
                                        }
                                        onClicked: confirm()
                                    }
                                }
                                Rectangle {
                                    width: 60; height: 20; radius: 3
                                    color: addCancelMa.containsMouse
                                           ? Qt.lighter(PropertiesPanelController.headerColor, 1.3)
                                           : PropertiesPanelController.controlBgColor
                                    border.color: PropertiesPanelController.borderColor
                                    Text { anchors.centerIn: parent; text: "Cancel"; color: PropertiesPanelController.textColor; font.pixelSize: 10 }
                                    MouseArea {
                                        id: addCancelMa
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: addNamePopup.close()
                                    }
                                }
                            }
                        }
                    }

                    // Filter / search — characters often have 50+ blend
                    // shapes, scanning a flat list is hopeless without
                    // a typeahead box.
                    TextField {
                        id: filterField
                        width: parent.width
                        placeholderText: "Filter targets…"
                        font.pixelSize: 10
                        onTextChanged: morphCol.filter = text
                        visible: morphCol.targetCount > 6
                    }

                    // One row per target. Hidden when filter doesn't match.
                    Repeater {
                        model: morphCol.targets
                        Row {
                            width: morphCol.width
                            spacing: 4
                            visible: morphCol.filter === ""
                                  || modelData.toLowerCase().indexOf(morphCol.filter.toLowerCase()) >= 0
                            height: visible ? 22 : 0

                            // Name — double-click to rename in place,
                            // matching the per-animation rename UX above.
                            Text {
                                id: morphNameText
                                visible: !morphNameEdit.visible
                                text: modelData
                                color: PropertiesPanelController.textColor
                                font.pixelSize: 10
                                width: 120
                                elide: Text.ElideRight
                                anchors.verticalCenter: parent.verticalCenter
                                MouseArea {
                                    anchors.fill: parent
                                    onDoubleClicked: {
                                        morphNameEdit.text = modelData
                                        morphNameEdit.visible = true
                                        morphNameEdit.forceActiveFocus()
                                        morphNameEdit.selectAll()
                                    }
                                }
                            }
                            TextInput {
                                id: morphNameEdit
                                visible: false
                                width: 120
                                color: PropertiesPanelController.textColor
                                font.pixelSize: 10
                                anchors.verticalCenter: parent.verticalCenter
                                selectByMouse: true
                                // Set by `Keys.onEscapePressed`; checked in
                                // `onEditingFinished` so that hiding the
                                // input on Escape (which causes focus loss
                                // and fires `editingFinished`) doesn't
                                // accidentally commit the rename.
                                property bool cancelled: false
                                Rectangle {
                                    anchors.fill: parent
                                    anchors.margins: -2
                                    z: -1
                                    color: PropertiesPanelController.inputColor
                                    border.color: PropertiesPanelController.highlightColor
                                    border.width: 1
                                    radius: 2
                                }
                                onEditingFinished: {
                                    if (cancelled) { cancelled = false; visible = false; return }
                                    var trimmed = text.trim()
                                    if (trimmed.length > 0 && trimmed !== modelData)
                                        MorphAnimationManager.renameMorphTarget(modelData, trimmed)
                                    visible = false
                                }
                                Keys.onEscapePressed: { cancelled = true; visible = false }
                            }
                            Slider {
                                id: weightSlider
                                from: 0; to: 1; stepSize: 0.01
                                width: parent.width - 222
                                // Bind to `weightTick` so changes that
                                // bypass user drag (Reset all, MCP, future
                                // dope-sheet scrubs) refresh the readout.
                                value: (morphCol.weightTick,
                                        MorphAnimationManager.weightForSelection(modelData))
                                anchors.verticalCenter: parent.verticalCenter
                                onMoved: MorphAnimationManager.setWeightForSelection(modelData, value)
                            }
                            Text {
                                text: weightSlider.value.toFixed(2)
                                color: PropertiesPanelController.textColor
                                font.pixelSize: 10
                                width: 36
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            // Delete (×) — drops the pose + animation
                            // through DeleteMorphTargetCommand so Ctrl+Z
                            // restores it.
                            Rectangle {
                                width: 18; height: 18; radius: 3
                                anchors.verticalCenter: parent.verticalCenter
                                color: morphDelMa.containsMouse
                                       ? Qt.lighter(PropertiesPanelController.headerColor, 1.3)
                                       : "transparent"
                                Text {
                                    anchors.centerIn: parent
                                    text: "×"
                                    color: PropertiesPanelController.textColor
                                    font.pixelSize: 12
                                    font.bold: true
                                }
                                MouseArea {
                                    id: morphDelMa
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: MorphAnimationManager.deleteMorphTarget(modelData)
                                }
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
