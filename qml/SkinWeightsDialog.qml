import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import MaterialEditorQML 1.0
import PropertiesPanel 1.0

// Issue #402 (+ Skinning v2 #819): top-level Window for automatic
// skin weights. Same Inspector-styled idiom as QuadRetopoDialog /
// UvUnwrapDialog. Operates on the currently selected entity — the
// mesh must have a skeleton attached, otherwise the button
// disables itself.
Window {
    id: dialog
    title: "Skin Weights"
    width: 560
    height: 560
    minimumWidth: 480
    minimumHeight: 500
    flags: Qt.Dialog
    modality: Qt.ApplicationModal
    color: PropertiesPanelController.panelColor

    // Knobs — defaults match SkinWeightsOptions.
    property int    maxInfluences:        4
    property double falloff:              4.0
    property double maxInfluenceDistance: 0.5
    property bool   skipUnweightedBones:  false
    property bool   replaceExisting:      true
    property string algorithm:            "geodesic-voxel"
    property int    voxelResolution:      64
    property int    smoothIterations:     3

    readonly property var algorithmIds: ["geodesic-voxel", "inverse-distance", "unirig"]
    readonly property var algorithmLabels: [
        "Geodesic Voxel (default)",
        "Inverse Distance (legacy)",
        "UniRig ML (falls back to Geodesic)"
    ]

    property string lastStatus: ""
    property bool   lastWasError: false

    function open() {
        dialog.lastStatus = ""
        dialog.lastWasError = false
        dialog.show()
        dialog.raise()
        dialog.requestActivate()
        keyCapture.forceActiveFocus()
    }

    function runCompute() {
        if (SkinWeightsController.busy) return
        if (!SkinWeightsController.hasSkinnedSelection) return
        const r = SkinWeightsController.computeWeightsForSelected(
            dialog.maxInfluences,
            dialog.falloff,
            dialog.maxInfluenceDistance,
            dialog.skipUnweightedBones,
            dialog.replaceExisting,
            dialog.algorithm,
            dialog.voxelResolution,
            dialog.smoothIterations)
        if (r && r.applied) {
            let status =
                "Done (" + (r.algorithmUsed || dialog.algorithm) + "): "
                + r.totalBones + " bones, "
                + r.totalVerticesProcessed + " verts, "
                + r.totalAssignmentsBefore + " → "
                + r.totalAssignmentsAfter + " assignments"
            if (r.fallbackReason && r.fallbackReason.length > 0)
                status += " — " + r.fallbackReason
            dialog.lastStatus = status
            dialog.lastWasError = false
        } else {
            dialog.lastStatus = "Failed: " + (r && r.error ? r.error : "unknown error")
            dialog.lastWasError = true
        }
    }

    Item {
        id: keyCapture
        anchors.fill: parent
        focus: true
        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_Escape) {
                dialog.close()
                event.accepted = true
            } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                dialog.runCompute()
                event.accepted = true
            }
        }
    }

    // ── Inline Inspector primitives ─────────────────────────────────

    component InspectorButton: Rectangle {
        id: btn
        property string label: ""
        property bool buttonEnabled: true
        signal clicked()
        // Keyboard accessibility: tabbable + Enter/Space activates.
        activeFocusOnTab: buttonEnabled
        Accessible.role: Accessible.Button
        Accessible.name: btn.label
        Keys.onSpacePressed: if (buttonEnabled) btn.clicked()
        Keys.onReturnPressed: if (buttonEnabled) btn.clicked()
        Keys.onEnterPressed: if (buttonEnabled) btn.clicked()
        height: 26
        radius: 3
        color: btnMa.containsMouse && buttonEnabled
            ? PropertiesPanelController.highlightColor
            : PropertiesPanelController.headerColor
        // A subtle focus ring so keyboard users can see where they are.
        border.color: btn.activeFocus
            ? PropertiesPanelController.highlightColor
            : PropertiesPanelController.borderColor
        border.width: btn.activeFocus ? 2 : 1
        opacity: buttonEnabled ? 1.0 : 0.45
        Text {
            anchors.centerIn: parent
            text: btn.label
            color: PropertiesPanelController.textColor
            font.pixelSize: 11
        }
        MouseArea {
            id: btnMa
            anchors.fill: parent
            hoverEnabled: true
            enabled: btn.buttonEnabled
            cursorShape: btn.buttonEnabled ? Qt.PointingHandCursor : Qt.ForbiddenCursor
            onClicked: btn.clicked()
        }
    }

    component InspectorLabel: Text {
        color: PropertiesPanelController.textColor
        font.pixelSize: 11
    }

    component InspectorNumberField: Rectangle {
        id: nf
        property double value: 0
        property double minValue: 0
        property double maxValue: 1e9
        property bool isInt: false
        signal newValue(double v)
        height: 24
        color: PropertiesPanelController.inputColor
        border.color: ni.activeFocus
            ? PropertiesPanelController.highlightColor
            : PropertiesPanelController.borderColor
        border.width: 1
        radius: 3
        TextInput {
            id: ni
            anchors.fill: parent
            anchors.leftMargin: 6
            anchors.rightMargin: 6
            text: nf.isInt ? Math.round(nf.value).toString() : nf.value.toFixed(2)
            color: PropertiesPanelController.textColor
            font.pixelSize: 11
            verticalAlignment: TextInput.AlignVCenter
            selectByMouse: true
            onEditingFinished: {
                const n = nf.isInt ? parseInt(text, 10) : parseFloat(text)
                if (!isNaN(n) && n >= nf.minValue && n <= nf.maxValue)
                    nf.newValue(n)
                else
                    text = nf.isInt ? Math.round(nf.value).toString() : nf.value.toFixed(2)
            }
        }
    }

    component InspectorCheckbox: Rectangle {
        id: cb
        property string label: ""
        property bool checked: false
        signal toggled()
        // Keyboard accessibility: tabbable + Enter/Space toggles.
        activeFocusOnTab: true
        Accessible.role: Accessible.CheckBox
        Accessible.name: cb.label
        Accessible.checked: cb.checked
        Keys.onSpacePressed: cb.toggled()
        Keys.onReturnPressed: cb.toggled()
        Keys.onEnterPressed: cb.toggled()
        height: 16
        width: parent ? parent.width : 200
        color: "transparent"
        Row {
            spacing: 6
            Rectangle {
                width: 14; height: 14
                radius: 2
                color: PropertiesPanelController.inputColor
                // Focus ring on the box itself when the checkbox has focus.
                border.color: cb.activeFocus
                    ? PropertiesPanelController.highlightColor
                    : PropertiesPanelController.borderColor
                border.width: cb.activeFocus ? 2 : 1
                Text {
                    anchors.centerIn: parent
                    text: cb.checked ? "✓" : ""
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 11
                }
            }
            InspectorLabel { text: cb.label }
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: cb.toggled()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        InspectorLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.85
            text: "Compute per-vertex skin weights against the attached "
                + "skeleton. Geodesic Voxel (Maya's production bind — "
                + "distances travel through the mesh volume, so nearby limbs "
                + "never share weights) is the default; Inverse Distance is "
                + "the legacy straight-line heuristic, still used for "
                + "volume-less meshes (planes, cloth). Weights are smoothed "
                + "and pruned to the influence cap afterwards. Existing "
                + "assignments are replaced (or merged — see option)."
        }

        // Algorithm
        RowLayout {
            spacing: 8
            Layout.fillWidth: true
            InspectorLabel { text: "Algorithm:"; Layout.preferredWidth: 130 }
            ThemedComboBox {
                Layout.preferredWidth: 240
                height: 24
                font.pixelSize: 11
                model: dialog.algorithmLabels
                currentIndex: Math.max(0, dialog.algorithmIds.indexOf(dialog.algorithm))
                onActivated: function(index) {
                    dialog.algorithm = dialog.algorithmIds[index]
                }
            }
            InspectorLabel {
                text: "how weights are computed"
                opacity: 0.7
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
        }

        // Voxel resolution (geodesic algorithms only)
        RowLayout {
            spacing: 8
            Layout.fillWidth: true
            visible: dialog.algorithm !== "inverse-distance"
            InspectorLabel { text: "Voxel resolution:"; Layout.preferredWidth: 130 }
            InspectorNumberField {
                Layout.preferredWidth: 80
                value: dialog.voxelResolution
                minValue: 8
                maxValue: 256
                isInt: true
                onNewValue: dialog.voxelResolution = Math.round(v)
            }
            InspectorLabel {
                text: "grid cells along the longest axis (higher = resolves thinner parts)"
                opacity: 0.7
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
        }

        // Smoothing iterations
        RowLayout {
            spacing: 8
            Layout.fillWidth: true
            InspectorLabel { text: "Smoothing:"; Layout.preferredWidth: 130 }
            InspectorNumberField {
                Layout.preferredWidth: 80
                value: dialog.smoothIterations
                minValue: 0
                maxValue: 50
                isInt: true
                onNewValue: dialog.smoothIterations = Math.round(v)
            }
            InspectorLabel {
                text: "Laplacian relaxation iterations (0 = off; default 3)"
                opacity: 0.7
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
        }

        // Max influences
        RowLayout {
            spacing: 8
            Layout.fillWidth: true
            InspectorLabel { text: "Max influences:"; Layout.preferredWidth: 130 }
            InspectorNumberField {
                Layout.preferredWidth: 80
                value: dialog.maxInfluences
                minValue: 1
                maxValue: 8
                isInt: true
                onNewValue: dialog.maxInfluences = Math.round(v)
            }
            InspectorLabel {
                text: "bones per vertex (hardware skinning convention: 4)"
                opacity: 0.7
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
        }

        // Falloff
        RowLayout {
            spacing: 8
            Layout.fillWidth: true
            InspectorLabel { text: "Falloff:"; Layout.preferredWidth: 130 }
            InspectorNumberField {
                Layout.preferredWidth: 80
                value: dialog.falloff
                minValue: 0.5
                maxValue: 16.0
                onNewValue: dialog.falloff = v
            }
            InspectorLabel {
                text: "higher = sharper bind (more rigid); lower = smoother blends"
                opacity: 0.7
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
        }

        // Max influence distance
        RowLayout {
            spacing: 8
            Layout.fillWidth: true
            InspectorLabel { text: "Max distance:"; Layout.preferredWidth: 130 }
            InspectorNumberField {
                Layout.preferredWidth: 80
                value: dialog.maxInfluenceDistance
                minValue: 0
                maxValue: 10
                onNewValue: dialog.maxInfluenceDistance = v
            }
            InspectorLabel {
                text: "fraction of mesh diagonal (0 = no cap; default 0.5)"
                opacity: 0.7
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
        }

        // Skip unweighted bones
        RowLayout {
            spacing: 8
            Layout.fillWidth: true
            InspectorLabel { text: ""; Layout.preferredWidth: 130 }
            InspectorCheckbox {
                Layout.fillWidth: true
                label: "Skip bones with no existing weights (Mixamo helper bones)"
                checked: dialog.skipUnweightedBones
                onToggled: dialog.skipUnweightedBones = !dialog.skipUnweightedBones
            }
        }

        // Replace existing
        RowLayout {
            spacing: 8
            Layout.fillWidth: true
            InspectorLabel { text: ""; Layout.preferredWidth: 130 }
            InspectorCheckbox {
                Layout.fillWidth: true
                label: "Replace existing weights (unchecked = merge / fill-in mode)"
                checked: dialog.replaceExisting
                onToggled: dialog.replaceExisting = !dialog.replaceExisting
            }
        }

        Item { Layout.fillHeight: true }

        // Status line
        InspectorLabel {
            Layout.fillWidth: true
            visible: dialog.lastStatus.length > 0
            text: dialog.lastStatus
            wrapMode: Text.WordWrap
            color: dialog.lastWasError ? "#cc4444" : "#3a8c3a"
        }

        // Buttons
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            InspectorButton {
                label: "Close"
                Layout.preferredWidth: 90
                onClicked: dialog.close()
            }
            InspectorButton {
                label: SkinWeightsController.busy ? "Computing…" : "Compute Weights"
                Layout.preferredWidth: 160
                buttonEnabled: !SkinWeightsController.busy
                    && SkinWeightsController.hasSkinnedSelection
                onClicked: dialog.runCompute()
            }
        }
    }

    Connections {
        target: SkinWeightsController
        function onError(msg) {
            dialog.lastStatus = "Failed: " + msg
            dialog.lastWasError = true
        }
    }
}
