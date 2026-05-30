import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import MaterialEditorQML 1.0
import PropertiesPanel 1.0

// Issue #401: top-level Window for triangle-pairing quad retopology.
// Same Inspector-styled idiom as UvUnwrapDialog (Rectangle + Text +
// MouseArea primitives over PropertiesPanelController.* colors).
// Operates on the currently selected entity — no input-path field.
// The result is committed in place (via the qtme.faces.<i> n-gon
// binding); unlike UV unwrap, no separate export step is needed
// because the triangle index buffer keeps a valid fan-triangulation.
Window {
    id: dialog
    title: "Quad Retopology"
    width: 540
    height: 420
    minimumWidth: 460
    minimumHeight: 380
    flags: Qt.Dialog
    modality: Qt.ApplicationModal
    color: PropertiesPanelController.panelColor

    // Knobs — defaults match QuadRetopoOptions.
    property int    targetFaces:       -1
    property double maxAngleDeg:       25.0
    property double shapeToleranceDeg: 65.0
    property double maxAspectRatio:    6.0

    // Last-run result, surfaced as a green status line at the bottom.
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

    // Pulled out of the button's onClicked so the same flow runs
    // whether triggered by mouse OR by Enter/Return at the Window
    // level (keyboard accessibility for the modal dialog — see
    // CodeRabbit review on PR #697).
    function runRetopo() {
        if (QuadRetopoController.busy) return
        if (!QuadRetopoController.hasSelection) return
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

        InspectorLabel {
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
                label: QuadRetopoController.busy ? "Retopologizing…" : "Retopologize"
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
    }
}
