import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import PropertiesPanel 1.0
import AnimationControl 1.0

Rectangle {
    id: root
    color: PropertiesPanelController.panelColor

    ScrollView {
        anchors.fill: parent
        clip: true

        Column {
            width: root.width
            spacing: 0

            // ---- Edit Mode Indicator ----
            Rectangle {
                width: parent.width
                height: 32
                color: EditModeController.editModeActive
                    ? "#3d6b3d" : PropertiesPanelController.headerColor
                visible: true

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    spacing: 8

                    Text {
                        text: EditModeController.modeLabel
                        color: PropertiesPanelController.textColor
                        font.bold: true
                        font.pixelSize: 12
                        Layout.fillWidth: true
                    }

                    Text {
                        text: EditModeController.editModeActive
                            ? "V:" + EditModeController.vertexCount +
                              " T:" + EditModeController.triangleCount +
                              " S:" + EditModeController.subMeshCount
                            : ""
                        color: PropertiesPanelController.textColor
                        font.pixelSize: 10
                        opacity: 0.7
                        visible: EditModeController.editModeActive
                    }

                    Button {
                        text: EditModeController.editModeActive ? "Exit" : "Edit"
                        enabled: EditModeController.editModeActive || EditModeController.canEnterEditMode
                        implicitWidth: 48
                        implicitHeight: 24
                        font.pixelSize: 10
                        ToolTip.text: "Toggle Edit Mode (Tab)"
                        ToolTip.visible: hovered
                        onClicked: EditModeController.toggleEditMode()
                    }
                }
            }

            // ---- Edit Mode Tools ----
            CollapsibleSection {
                title: "Edit Mode Tools"
                sectionVisible: EditModeController.editModeActive
                expanded: true

                Component.onCompleted: content = editModeToolsComponent
            }

            // ---- Scene Outliner ----
            CollapsibleSection {
                title: "Scene"
                expanded: true

                Component.onCompleted: content = sceneOutlinerComponent
            }

            // ---- Transform ----
            CollapsibleSection {
                title: "Transform"
                sectionVisible: PropertiesPanelController.hasSelection

                Component.onCompleted: content = transformComponent
            }

            // ---- Snap Settings ----
            CollapsibleSection {
                title: "Snap Settings"
                expanded: false

                Component.onCompleted: content = snapSettingsComponent
            }

            // ---- Primitive Parameters ----
            CollapsibleSection {
                title: "Primitive: " + PropertiesPanelController.primitiveType
                sectionVisible: PropertiesPanelController.hasPrimitive

                Component.onCompleted: content = primitiveComponent
            }

            // ---- Animations ----
            CollapsibleSection {
                title: "Animations"
                sectionVisible: PropertiesPanelController.hasAnimations

                Component.onCompleted: content = animationComponent
            }

            // ---- Animation Control (keyframe editor) ----
            CollapsibleSection {
                title: "Animation Control"
                sectionVisible: AnimationControlController.hasAnimation
                expanded: false

                Component.onCompleted: content = animControlComponent
            }

            // ---- LOD Generation ----
            CollapsibleSection {
                title: "LOD Generation"
                sectionVisible: MeshLodController.hasSelection
                expanded: false

                Component.onCompleted: content = lodComponent
            }

            // ---- Material Presets ----
            CollapsibleSection {
                title: "Material Presets"
                sectionVisible: PropertiesPanelController.hasSelection
                expanded: false

                Component.onCompleted: content = materialPresetsComponent
            }

            // ---- Mesh Validation ----
            CollapsibleSection {
                title: "Mesh Validation"
                sectionVisible: MeshValidator.hasSelection
                expanded: false

                Component.onCompleted: content = validationComponent
            }

            // ---- Undo History ----
            CollapsibleSection {
                title: "Undo History"
                expanded: false

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

            // Scene header with reparent button
            Row {
                width: outlinerColumn.width
                height: 22
                spacing: 4

                Text {
                    text: "\u25A1 Scene (Root)"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 11; font.bold: true
                    anchors.verticalCenter: parent.verticalCenter
                    leftPadding: 4
                }

                Item { width: 1; height: 1; Layout.fillWidth: true }

                // "Move to Root" button — visible when a non-root node is selected
                Rectangle {
                    visible: PropertiesPanelController.selectionName !== "" &&
                             PropertiesPanelController.canReparentNode(PropertiesPanelController.selectionName, "root")
                    width: toRootText.implicitWidth + 10; height: 18; radius: 3
                    anchors.verticalCenter: parent.verticalCenter
                    color: toRootMa.containsMouse ? PropertiesPanelController.highlightColor : PropertiesPanelController.headerColor
                    border.color: PropertiesPanelController.borderColor; border.width: 1
                    Text { id: toRootText; anchors.centerIn: parent; text: "\u2191 to Root"; color: PropertiesPanelController.textColor; font.pixelSize: 9 }
                    MouseArea { id: toRootMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                        onClicked: PropertiesPanelController.reparentNode(PropertiesPanelController.selectionName, "root")
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

            Connections {
                target: EditModeController
                function onSelectionModeChanged() { editToolsCol.activeSelMode = EditModeController.selectionMode }
                function onSoftSelectionFalloffChanged() { editToolsCol.activeFalloff = EditModeController.softSelectionFalloff }
                function onNormalsModeChanged() { editToolsCol.activeNormals = EditModeController.normalsMode }
                function onSoftSelectionEnabledChanged() { editToolsCol.softSelOn = EditModeController.softSelectionEnabled }
                function onWireframeChanged() { editToolsCol.wireframeOn = EditModeController.wireframeEnabled }
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
                    color: editToolsCol.softSelOn ? PropertiesPanelController.highlightColor : "transparent"
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
                    color: editToolsCol.wireframeOn ? PropertiesPanelController.highlightColor : "transparent"
                    Behavior on color { ColorAnimation { duration: 50 } }
                    Text { anchors.centerIn: parent; text: editToolsCol.wireframeOn ? "\u2713" : ""; color: "white"; font.pixelSize: 10 }
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                        onClicked: { editToolsCol.wireframeOn = !editToolsCol.wireframeOn; EditModeController.wireframeEnabled = editToolsCol.wireframeOn }
                    }
                }
                Text { text: "Wireframe"; font.pixelSize: 11; color: PropertiesPanelController.textColor; anchors.verticalCenter: parent.verticalCenter }
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
                    color: snapCol.snapOn ? PropertiesPanelController.highlightColor : "transparent"
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

                ComboBox {
                    id: exportFormatCombo
                    width: 90; height: 26
                    model: ["gltf", "glb", "fbx", "obj", "mesh"]
                    background: Rectangle {
                        color: PropertiesPanelController.headerColor
                        border.color: PropertiesPanelController.borderColor; border.width: 1; radius: 3
                    }
                    contentItem: Text {
                        leftPadding: 6
                        text: exportFormatCombo.displayText
                        color: PropertiesPanelController.textColor; font.pixelSize: 11
                        verticalAlignment: Text.AlignVCenter
                    }
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
                { name: "Wireframe",       label: "Wireframe",cat: "Other",   diff: "#223322", spec: "#44dd44", shin: 0,   alpha: 1.0,  wire: true,  unlit: false }
            ]
            property var categories: ["Plastic", "Metal", "Wood", "Glass", "Other"]
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
                                : "\u2714"
                            color: modelData.type === "error" ? "#e05050"
                                 : modelData.type === "warning" ? "#e0a030"
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

            // Play/Pause button
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
                        visible: groupExpanded
                        width: parent.width
                        spacing: 2
                        leftPadding: 8

                        // Animation rows
                        Repeater {
                            model: grp.animations

                            Rectangle {
                                width: parent.width - 8; height: 22; color: "transparent"

                                Row {
                                    anchors.fill: parent; spacing: 6

                                    // Enable checkbox
                                    Rectangle {
                                        width: 14; height: 14; anchors.verticalCenter: parent.verticalCenter
                                        border.color: PropertiesPanelController.borderColor; border.width: 1; radius: 2
                                        color: modelData.enabled ? PropertiesPanelController.highlightColor : "transparent"
                                        Text { anchors.centerIn: parent; text: modelData.enabled ? "\u2713" : ""; color: "white"; font.pixelSize: 10 }
                                        MouseArea { anchors.fill: parent; onClicked: PropertiesPanelController.toggleAnimationEnabled(grp.entity, modelData.name, !modelData.enabled) }
                                    }

                                    // Loop checkbox
                                    Rectangle {
                                        width: 14; height: 14; anchors.verticalCenter: parent.verticalCenter
                                        border.color: PropertiesPanelController.borderColor; border.width: 1; radius: 7
                                        color: modelData.loop ? PropertiesPanelController.highlightColor : "transparent"
                                        Text { anchors.centerIn: parent; text: modelData.loop ? "\u21BB" : ""; color: "white"; font.pixelSize: 8 }
                                        MouseArea { anchors.fill: parent; onClicked: PropertiesPanelController.toggleAnimationLoop(grp.entity, modelData.name, !modelData.loop) }
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
                                color: grp.showSkeleton ? PropertiesPanelController.highlightColor : "transparent"
                                Text { anchors.centerIn: parent; text: grp.showSkeleton ? "\u2713" : ""; color: "white"; font.pixelSize: 10 }
                                MouseArea { anchors.fill: parent; onClicked: PropertiesPanelController.toggleSkeletonDebug(grp.entity, !grp.showSkeleton) }
                            }
                            Text { text: "Skeleton"; color: PropertiesPanelController.textColor; font.pixelSize: 10; anchors.verticalCenter: parent.verticalCenter }

                            Rectangle {
                                width: 14; height: 14; anchors.verticalCenter: parent.verticalCenter
                                border.color: PropertiesPanelController.borderColor; border.width: 1; radius: 2
                                color: grp.showWeights ? PropertiesPanelController.highlightColor : "transparent"
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
}
