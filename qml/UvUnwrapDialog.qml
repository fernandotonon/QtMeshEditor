import QtQuick
import QtQuick.Layouts
import QtQuick.Window
import MaterialEditorQML 1.0
import PropertiesPanel 1.0

// Issue #400: top-level Window for xatlas-backed auto UV unwrap. Same
// Inspector-styled idiom as TextureChannelPackerDialog / NormalMap
// GeneratorDialog — Rectangle + Text + MouseArea primitives over
// PropertiesPanelController.* colors. Unlike those, this operates on
// the *currently selected entity* (not disk files), so the dialog
// has no input-path field. Just sliders + Apply.
Window {
    id: dialog
    title: "Auto UV Unwrap"
    width: 520
    height: 360
    minimumWidth: 460
    minimumHeight: 320
    flags: Qt.Dialog
    modality: Qt.ApplicationModal
    color: PropertiesPanelController.panelColor

    // Knobs — defaults match the headless `qtmesh uv --unwrap` CLI
    // and `UvUnwrapOptions`.
    property int    resolution: 1024
    property int    padding:    4
    property int    channel:    0
    property bool   preserveOriginalAsBackup: true
    property string outputPath: ""

    // Last-run result, surfaced as a green status line at the bottom.
    property string lastStatus: ""
    property bool   lastWasError: false

    function open() {
        dialog.lastStatus = ""
        dialog.lastWasError = false
        dialog.show()
        dialog.raise()
        dialog.requestActivate()
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
        property int value: 0
        property int minValue: 1
        property int maxValue: 16384
        signal newValue(int v)
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
            text: nf.value.toString()
            color: PropertiesPanelController.textColor
            font.pixelSize: 11
            verticalAlignment: TextInput.AlignVCenter
            selectByMouse: true
            validator: IntValidator { bottom: nf.minValue; top: nf.maxValue }
            onEditingFinished: {
                const n = parseInt(text, 10)
                if (!isNaN(n) && n >= nf.minValue && n <= nf.maxValue)
                    nf.newValue(n)
                else
                    text = nf.value.toString()
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
            text: "Auto-unwrap the selected mesh via xatlas (the library Blender and "
                + "Godot use). Vertices along chart seams are split so each chart has "
                + "its own UV. Skin weights and other vertex attributes survive the "
                + "split via xref remap.\n\nThe unwrap is written to a new file — your "
                + "on-screen mesh is left untouched. Open the exported file to see the "
                + "result."
        }

        // Resolution
        RowLayout {
            spacing: 8
            Layout.fillWidth: true
            InspectorLabel { text: "Resolution:"; Layout.preferredWidth: 100 }
            InspectorNumberField {
                Layout.preferredWidth: 100
                value: dialog.resolution
                minValue: 64
                maxValue: 8192
                onNewValue: dialog.resolution = v
            }
            InspectorLabel {
                text: "texels (atlas size hint, xatlas estimates texelsPerUnit to match)"
                opacity: 0.7
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
        }

        // Padding
        RowLayout {
            spacing: 8
            Layout.fillWidth: true
            InspectorLabel { text: "Padding:"; Layout.preferredWidth: 100 }
            InspectorNumberField {
                Layout.preferredWidth: 100
                value: dialog.padding
                minValue: 0
                maxValue: 64
                onNewValue: dialog.padding = v
            }
            InspectorLabel {
                text: "texels around each chart (4 = safe up to MIP level 2)"
                opacity: 0.7
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
        }

        // UV channel
        RowLayout {
            spacing: 8
            Layout.fillWidth: true
            InspectorLabel { text: "UV channel:"; Layout.preferredWidth: 100 }
            InspectorNumberField {
                Layout.preferredWidth: 100
                value: dialog.channel
                minValue: 0
                maxValue: 7
                onNewValue: dialog.channel = v
            }
            InspectorLabel {
                text: "0 overwrites primary UVs; 1+ keeps UV0 and writes a lightmap UV"
                opacity: 0.7
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
        }

        // Preserve checkbox
        RowLayout {
            spacing: 8
            Layout.fillWidth: true
            InspectorLabel { text: ""; Layout.preferredWidth: 100 }
            Rectangle {
                width: 16; height: 16
                radius: 2
                color: PropertiesPanelController.inputColor
                border.color: PropertiesPanelController.borderColor
                border.width: 1
                Text {
                    anchors.centerIn: parent
                    text: dialog.preserveOriginalAsBackup ? "✓" : ""
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 12
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: dialog.preserveOriginalAsBackup = !dialog.preserveOriginalAsBackup
                }
            }
            InspectorLabel {
                text: "Preserve previous UVs on UV{channel+1} (when overwriting an existing channel)"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: dialog.preserveOriginalAsBackup = !dialog.preserveOriginalAsBackup
                }
            }
        }

        // Output file picker
        RowLayout {
            spacing: 8
            Layout.fillWidth: true
            InspectorLabel { text: "Output:"; Layout.preferredWidth: 100 }
            Rectangle {
                Layout.fillWidth: true
                height: 24
                color: PropertiesPanelController.inputColor
                border.color: PropertiesPanelController.borderColor
                border.width: 1
                radius: 3
                Text {
                    anchors.fill: parent
                    anchors.leftMargin: 6
                    anchors.rightMargin: 6
                    text: dialog.outputPath.length > 0
                        ? dialog.outputPath
                        : "(click Browse… to choose)"
                    color: PropertiesPanelController.textColor
                    opacity: dialog.outputPath.length > 0 ? 1.0 : 0.45
                    font.pixelSize: 11
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideLeft
                }
            }
            InspectorButton {
                label: "Browse…"
                Layout.preferredWidth: 90
                onClicked: {
                    const picked = UvUnwrapController.chooseOutputPath()
                    if (picked && picked.length > 0) dialog.outputPath = picked
                }
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
                label: UvUnwrapController.busy ? "Unwrapping…" : "Export Unwrapped"
                Layout.preferredWidth: 160
                buttonEnabled: !UvUnwrapController.busy
                    && UvUnwrapController.hasSelection
                    && dialog.outputPath.length > 0
                onClicked: {
                    const r = UvUnwrapController.unwrapSelectedToFile(
                        dialog.outputPath,
                        dialog.resolution,
                        dialog.padding,
                        dialog.channel,
                        dialog.preserveOriginalAsBackup)
                    if (r && r.applied) {
                        dialog.lastStatus =
                            "Exported " + r.meshName +
                            ": " + r.verticesBefore + " → " + r.verticesAfter + " verts, " +
                            r.chartCount + " charts, " +
                            (r.atlasWidth + "×" + r.atlasHeight) + " atlas, " +
                            (Math.round(r.utilization * 1000) / 10) + "% utilization"
                        dialog.lastWasError = false
                    } else {
                        dialog.lastStatus = "Failed: " + (r && r.error ? r.error : "unknown error")
                        dialog.lastWasError = true
                    }
                }
            }
        }
    }

    Connections {
        target: UvUnwrapController
        function onError(msg) {
            dialog.lastStatus = "Failed: " + msg
            dialog.lastWasError = true
        }
    }
}
