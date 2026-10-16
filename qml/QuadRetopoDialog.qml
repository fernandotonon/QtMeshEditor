import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import MaterialEditorQML 1.0
import PropertiesPanel 1.0

// Issue #401: top-level Window for quad retopology — triangle pairing
// (in place) or a QuadriFlow field-aligned remesh (worker thread).
// Same Inspector-styled idiom as UvUnwrapDialog (Rectangle + Text +
// MouseArea primitives over PropertiesPanelController.* colors).
// Operates on the currently selected entity — no input-path field.
// The result is committed in place (via the qtme.faces.<i> n-gon
// binding); unlike UV unwrap, no separate export step is needed
// because the triangle index buffer keeps a valid fan-triangulation.
Window {
    id: dialog
    title: "Quad Retopology"
    width: 560
    height: 520
    minimumWidth: 480
    minimumHeight: 460
    flags: Qt.Dialog
    modality: Qt.ApplicationModal
    color: PropertiesPanelController.panelColor

    // Knobs — defaults match QuadRetopoOptions.
    property int    targetFaces:       -1
    property double maxAngleDeg:       25.0
    property double shapeToleranceDeg: 65.0
    property double maxAspectRatio:    6.0

    // "pair" = triangle pairing (in place, keeps every vertex/UV/weight);
    // "quadriflow" = field-aligned all-quad remesh (new topology; textures
    // re-baked and skin weights transferred; runs on a worker thread).
    property string method: "pair"
    property int    qfTarget: 5000
    property bool   qfSharp: true
    property bool   qfAdaptive: true
    property bool   qfBoundary: false
    property bool   qfBake: true
    property bool   qfDetailNormal: true
    property string qfVoxel: "auto"

    // Last-run result, surfaced as a green status line at the bottom.
    property string lastStatus: ""
    property bool   lastWasError: false

    function open() {
        dialog.lastStatus = ""
        dialog.lastWasError = false
        dialog.qfTarget = QuadRetopoController.suggestedQuadriFlowTarget()
        dialog.show()
        dialog.raise()
        dialog.requestActivate()
        keyCapture.forceActiveFocus()
    }

    // Pulled out of the button's onClicked so the same flow runs
    // whether triggered by mouse OR by Enter/Return at the Window
    // level (keyboard accessibility for the modal dialog — see
    // CodeRabbit review on PR #697).
    function runRetopo() {
        if (QuadRetopoController.busy) return
        if (!QuadRetopoController.hasSelection) return
        if (dialog.method === "quadriflow") {
            dialog.lastStatus = ""
            QuadRetopoController.remeshSelectedQuadriFlow({
                targetFaces: dialog.qfTarget,
                preserveSharp: dialog.qfSharp,
                adaptive: dialog.qfAdaptive,
                preserveBoundary: dialog.qfBoundary,
                bakeTextures: dialog.qfBake,
                detailNormal: dialog.qfDetailNormal,
                voxel: dialog.qfVoxel
            })
            return
        }
        const r = QuadRetopoController.retopologizeSelected(
            dialog.targetFaces,
            dialog.maxAngleDeg,
            dialog.shapeToleranceDeg,
            dialog.maxAspectRatio)
        if (r && r.applied) {
            const dom = Math.round((r.quadDominance || 0) * 1000) / 10
            dialog.lastStatus =
                "Done: " + r.totalTrianglesBefore + " tris → "
                + r.totalFacesAfter + " faces ("
                + r.totalQuadsAfter + " quads, "
                + r.totalTrianglesAfter + " tris, "
                + dom + "% quad dominance)"
            dialog.lastWasError = false
        } else {
            dialog.lastStatus = "Failed: " + (r && r.error ? r.error : "unknown error")
            dialog.lastWasError = true
        }
    }

    // Window-level key handler — invisible Item that owns active
    // focus when the dialog opens. Enter / Return runs the retopo,
    // Escape closes the dialog. Without this, keyboard users
    // couldn't drive the modal at all because InspectorButton
    // doesn't accept tab focus (it's a Rectangle + MouseArea, not
    // a QtQuick.Controls.Button).
    Item {
        id: keyCapture
        anchors.fill: parent
        focus: true
        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_Escape) {
                dialog.close()
                event.accepted = true
            } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                dialog.runRetopo()
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
        height: 26
        radius: 3
        color: btnMa.containsMouse && buttonEnabled
            ? PropertiesPanelController.highlightColor
            : PropertiesPanelController.headerColor
        border.color: PropertiesPanelController.borderColor
        border.width: 1
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

    component InspectorCheck: Rectangle {
        id: ck
        property string label: ""
        property bool checked: false
        signal toggled(bool on)
        implicitHeight: 22
        implicitWidth: ckRow.implicitWidth
        color: "transparent"
        Row {
            id: ckRow
            spacing: 6
            anchors.verticalCenter: parent.verticalCenter
            Rectangle {
                width: 14; height: 14; radius: 2
                anchors.verticalCenter: parent.verticalCenter
                color: ck.checked ? PropertiesPanelController.highlightColor
                                  : PropertiesPanelController.inputColor
                border.color: PropertiesPanelController.borderColor
                Text {
                    anchors.centerIn: parent
                    visible: ck.checked
                    text: "✓"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 10
                }
            }
            Text {
                text: ck.label
                color: PropertiesPanelController.textColor
                font.pixelSize: 11
                anchors.verticalCenter: parent.verticalCenter
            }
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: ck.toggled(!ck.checked)
        }
    }

    component SegmentButton: InspectorButton {
        property bool selected: false
        color: selected ? PropertiesPanelController.highlightColor
                        : PropertiesPanelController.headerColor
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

    // ── Layout ───────────────────────────────────────────────────────

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            spacing: 4
            InspectorLabel { text: "Method:"; Layout.preferredWidth: 110 }
            SegmentButton {
                label: "Triangle pairing"
                Layout.preferredWidth: 140
                selected: dialog.method === "pair"
                buttonEnabled: !QuadRetopoController.busy
                onClicked: dialog.method = "pair"
            }
            SegmentButton {
                label: "QuadriFlow remesh"
                Layout.preferredWidth: 140
                selected: dialog.method === "quadriflow"
                buttonEnabled: !QuadRetopoController.busy && QuadRetopoController.quadriFlowAvailable
                onClicked: dialog.method = "quadriflow"
            }
            Item { Layout.fillWidth: true }
        }

        // ── QuadriFlow options ────────────────────────────────────────
        ColumnLayout {
            visible: dialog.method === "quadriflow"
            Layout.fillWidth: true
            spacing: 8

            InspectorLabel {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                opacity: 0.85
                text: "Rebuild the surface as a clean, field-aligned all-quad mesh "
                    + "(QuadriFlow). New topology: the textures are re-baked onto a fresh "
                    + "UV atlas and skin weights are copied from the closest point on the "
                    + "original, so a rigged mesh stays rigged. Non-manifold meshes get a "
                    + "voxel rebuild first. One undo step. Export as FBX or OBJ to keep the quads."
            }
            RowLayout {
                spacing: 8
                Layout.fillWidth: true
                InspectorLabel { text: "Target quads:"; Layout.preferredWidth: 110 }
                InspectorNumberField {
                    Layout.preferredWidth: 100
                    value: dialog.qfTarget
                    minValue: 16
                    maxValue: 200000
                    isInt: true
                    onNewValue: dialog.qfTarget = Math.round(v)
                }
                InspectorLabel {
                    text: "approximate — more quads keep more of the original form"
                    opacity: 0.7
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }
            }
            RowLayout {
                spacing: 16
                Layout.fillWidth: true
                Item { Layout.preferredWidth: 110 }
                InspectorCheck {
                    label: "Re-bake textures"
                    checked: dialog.qfBake
                    onToggled: function(on) { dialog.qfBake = on }
                }
                InspectorCheck {
                    label: "Detail normal map"
                    checked: dialog.qfDetailNormal
                    opacity: dialog.qfBake ? 1.0 : 0.45
                    onToggled: function(on) { dialog.qfDetailNormal = on }
                }
            }
            RowLayout {
                spacing: 16
                Layout.fillWidth: true
                Item { Layout.preferredWidth: 110 }
                InspectorCheck {
                    label: "Preserve sharp edges"
                    checked: dialog.qfSharp
                    onToggled: function(on) { dialog.qfSharp = on }
                }
                InspectorCheck {
                    label: "Preserve open borders"
                    checked: dialog.qfBoundary
                    onToggled: function(on) { dialog.qfBoundary = on }
                }
            }
            RowLayout {
                spacing: 16
                Layout.fillWidth: true
                Item { Layout.preferredWidth: 110 }
                InspectorCheck {
                    label: "Adaptive density (keeps small features)"
                    checked: dialog.qfAdaptive
                    onToggled: function(on) { dialog.qfAdaptive = on }
                }
            }
            RowLayout {
                spacing: 4
                Layout.fillWidth: true
                InspectorLabel { text: "Voxel rebuild:"; Layout.preferredWidth: 110 }
                Repeater {
                    model: [ ["auto", "Auto"], ["on", "Always"], ["off", "Never"] ]
                    delegate: SegmentButton {
                        required property var modelData
                        label: modelData[1]
                        Layout.preferredWidth: 70
                        selected: dialog.qfVoxel === modelData[0]
                        onClicked: dialog.qfVoxel = modelData[0]
                    }
                }
                Item { Layout.fillWidth: true }
            }
            // Progress while the worker runs.
            ColumnLayout {
                visible: QuadRetopoController.busy
                Layout.fillWidth: true
                spacing: 4
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 6
                    radius: 3
                    color: PropertiesPanelController.inputColor
                    border.color: PropertiesPanelController.borderColor
                    Rectangle {
                        width: parent.width * Math.max(0, Math.min(1, QuadRetopoController.progress))
                        height: parent.height
                        radius: 3
                        color: PropertiesPanelController.highlightColor
                    }
                }
                InspectorLabel {
                    text: QuadRetopoController.progressText
                    opacity: 0.8
                }
            }
        }

        InspectorLabel {
            visible: dialog.method === "pair"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.85
            text: "Pair adjacent triangles into quads where the merge is "
                + "coplanar, near-rectangular, and within the aspect-ratio "
                + "gate. The mesh is rewritten in place — quads are kept "
                + "via the n-gon binding so the FBX / glTF exporter round-"
                + "trips them. No new vertices are introduced; skin "
                + "weights and UVs survive unchanged."
        }

        // Target faces
        RowLayout {
            visible: dialog.method === "pair"
            spacing: 8
            Layout.fillWidth: true
            InspectorLabel { text: "Target faces:"; Layout.preferredWidth: 110 }
            InspectorNumberField {
                Layout.preferredWidth: 100
                value: dialog.targetFaces
                minValue: -1
                maxValue: 10000000
                isInt: true
                onNewValue: dialog.targetFaces = Math.round(v)
            }
            InspectorLabel {
                text: "-1 = pair every viable candidate (~50% reduction max)"
                opacity: 0.7
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
        }

        // Max angle
        RowLayout {
            visible: dialog.method === "pair"
            spacing: 8
            Layout.fillWidth: true
            InspectorLabel { text: "Max angle:"; Layout.preferredWidth: 110 }
            InspectorNumberField {
                Layout.preferredWidth: 100
                value: dialog.maxAngleDeg
                minValue: 0
                maxValue: 180
                onNewValue: dialog.maxAngleDeg = v
            }
            InspectorLabel {
                text: "deg between adjacent triangle normals (lower = more curvature-preserving)"
                opacity: 0.7
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
        }

        // Shape tolerance
        RowLayout {
            visible: dialog.method === "pair"
            spacing: 8
            Layout.fillWidth: true
            InspectorLabel { text: "Shape tol:"; Layout.preferredWidth: 110 }
            InspectorNumberField {
                Layout.preferredWidth: 100
                value: dialog.shapeToleranceDeg
                minValue: 0
                maxValue: 90
                onNewValue: dialog.shapeToleranceDeg = v
            }
            InspectorLabel {
                text: "deg deviation per interior angle from 90 (lower = stricter quad)"
                opacity: 0.7
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
        }

        // Max aspect ratio
        RowLayout {
            visible: dialog.method === "pair"
            spacing: 8
            Layout.fillWidth: true
            InspectorLabel { text: "Max aspect:"; Layout.preferredWidth: 110 }
            InspectorNumberField {
                Layout.preferredWidth: 100
                value: dialog.maxAspectRatio
                minValue: 1
                maxValue: 100
                onNewValue: dialog.maxAspectRatio = v
            }
            InspectorLabel {
                text: "longest edge / shortest edge (lower = more square-like)"
                opacity: 0.7
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
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
                visible: QuadRetopoController.busy && dialog.method === "quadriflow"
                label: "Cancel"
                Layout.preferredWidth: 90
                onClicked: QuadRetopoController.cancelRemesh()
            }
            InspectorButton {
                label: QuadRetopoController.busy
                    ? (dialog.method === "quadriflow" ? "Remeshing…" : "Retopologizing…")
                    : (dialog.method === "quadriflow" ? "Remesh" : "Retopologize")
                Layout.preferredWidth: 160
                buttonEnabled: !QuadRetopoController.busy
                    && QuadRetopoController.hasSelection
                onClicked: dialog.runRetopo()
            }
        }
    }

    Connections {
        target: QuadRetopoController
        function onError(msg) {
            dialog.lastStatus = "Failed: " + msg
            dialog.lastWasError = true
        }
        function onRemeshFinished(r) {
            let msg = "Done: " + r.inputTriangles + " tris → " + r.quads + " quads"
                + (r.triangles > 0 ? " + " + r.triangles + " tris" : "")
                + ", " + r.vertices + " vertices"
            if (r.voxelized) msg += " (voxel rebuild)"
            if (r.skinned) msg += ", skin weights transferred"
            if (r.bakedMaps && r.bakedMaps.length > 0)
                msg += ", re-baked " + r.textureWidth + "×" + r.textureHeight + " textures"
            dialog.lastStatus = msg + ". Ctrl+Z to undo."
            dialog.lastWasError = false
        }
    }
}
