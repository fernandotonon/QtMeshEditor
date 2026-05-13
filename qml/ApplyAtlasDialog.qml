import QtQuick
import QtQuick.Layouts
import QtQuick.Window
import MaterialEditorQML 1.0
import PropertiesPanel 1.0

// Phase 6 slice E2: apply a previously-packed atlas manifest to a mesh.
// Same Inspector-styled idiom as TextureAtlasDialog / NormalMapGeneratorDialog
// (Rectangle + Text + MouseArea primitives over PropertiesPanelController.*
// colors). Three path inputs (mesh / manifest / atlas image), an output path,
// a match-mode toggle, a clamp toggle, and an Apply button.
Window {
    id: dialog
    title: "Apply Atlas to Mesh"
    width: 720
    height: 460
    minimumWidth: 600
    minimumHeight: 380
    flags: Qt.Dialog
    modality: Qt.ApplicationModal
    color: PropertiesPanelController.panelColor

    property string meshPath: ""
    property string manifestPath: ""
    property string atlasPath: ""
    property string outputPath: ""
    property string matchMode: "basename"     // or "fullpath"
    property bool   clampOutOfRangeUVs: true
    property bool   stripNonDiffuseTextures: true

    function open() {
        dialog.show()
        dialog.raise()
        dialog.requestActivate()
    }

    function normaliseDroppedPath(url) {
        const s = url.toString()
        return s.startsWith("file://") ? s.substring(7) : s
    }

    // ── Inspector primitives (matching TextureAtlasDialog patterns) ──

    component InspectorButton: Rectangle {
        id: btn
        property string label: ""
        property bool buttonEnabled: true
        signal clicked()
        height: 26
        radius: 3
        color: buttonEnabled
            ? (btnMa.containsMouse
                ? PropertiesPanelController.highlightColor
                : PropertiesPanelController.headerColor)
            : Qt.darker(PropertiesPanelController.headerColor, 1.2)
        border.color: PropertiesPanelController.borderColor
        border.width: 1
        opacity: buttonEnabled ? 1.0 : 0.5
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
            cursorShape: btn.buttonEnabled ? Qt.PointingHandCursor : Qt.ArrowCursor
            enabled: btn.buttonEnabled
            onClicked: btn.clicked()
        }
    }

    component InspectorLabel: Text {
        color: PropertiesPanelController.textColor
        font.pixelSize: 11
    }

    component InspectorPathField: RowLayout {
        id: row
        property string label: ""
        property string value: ""
        property string browseCaption: "Browse…"
        property string placeholder: ""
        signal browse()
        signal changed(string newValue)
        spacing: 6
        InspectorLabel {
            text: row.label
            Layout.preferredWidth: 90
        }
        Rectangle {
            Layout.fillWidth: true
            height: 24
            color: PropertiesPanelController.inputColor
            border.color: PropertiesPanelController.borderColor
            border.width: 1
            radius: 3
            TextInput {
                id: input
                anchors.fill: parent
                anchors.leftMargin: 6
                anchors.rightMargin: 6
                verticalAlignment: TextInput.AlignVCenter
                color: PropertiesPanelController.textColor
                selectByMouse: true
                clip: true
                text: row.value
                font.pixelSize: 11
                onEditingFinished: row.changed(input.text)
            }
            InspectorLabel {
                anchors.fill: parent
                anchors.leftMargin: 8
                verticalAlignment: Text.AlignVCenter
                text: row.placeholder
                opacity: 0.5
                visible: input.text.length === 0
            }
            DropArea {
                anchors.fill: parent
                onDropped: (drop) => {
                    if (drop.hasUrls && drop.urls.length > 0) {
                        const p = normaliseDroppedPath(drop.urls[0])
                        input.text = p
                        row.changed(p)
                    }
                }
            }
        }
        InspectorButton {
            label: row.browseCaption
            Layout.preferredWidth: 80
            onClicked: row.browse()
        }
    }

    // ── Layout ─────────────────────────────────────────────────────

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 10

        InspectorLabel {
            text: "Apply a previously-packed atlas manifest to a mesh. Rewrites "
                + "UV0 of every submesh whose diffuse texture matches a tile into "
                + "the tile's sub-rect and rebinds the diffuse texture to the atlas image. "
                + "Counterpart to Pack Atlas."
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
            opacity: 0.85
        }

        InspectorPathField {
            Layout.fillWidth: true
            label: "Mesh"
            value: dialog.meshPath
            placeholder: "Drop a mesh file or click Browse"
            onBrowse: {
                const picked = MaterialEditorQML.openMeshDialog()
                if (picked && picked.length > 0) dialog.meshPath = picked
            }
            onChanged: (v) => dialog.meshPath = v
        }

        InspectorPathField {
            Layout.fillWidth: true
            label: "Manifest"
            value: dialog.manifestPath
            placeholder: "Drop atlas.json or click Browse"
            onBrowse: {
                const picked = MaterialEditorQML.openManifestDialog()
                if (picked && picked.length > 0) dialog.manifestPath = picked
            }
            onChanged: (v) => dialog.manifestPath = v
        }

        InspectorPathField {
            Layout.fillWidth: true
            label: "Atlas image"
            value: dialog.atlasPath
            placeholder: "Drop atlas.png or click Browse"
            onBrowse: {
                const picked = MaterialEditorQML.openFileDialog()
                if (picked && picked.length > 0) dialog.atlasPath = picked
            }
            onChanged: (v) => dialog.atlasPath = v
        }

        InspectorPathField {
            Layout.fillWidth: true
            label: "Output"
            value: dialog.outputPath
            placeholder: "Where to write the atlased mesh"
            browseCaption: "Save as…"
            onBrowse: {
                const picked = MaterialEditorQML.saveAtlasedMeshDialog()
                if (picked && picked.length > 0) dialog.outputPath = picked
            }
            onChanged: (v) => dialog.outputPath = v
        }

        // Options row.
        RowLayout {
            Layout.fillWidth: true
            spacing: 14
            InspectorLabel { text: "Match by:" }
            // Mode toggle as two pill buttons.
            Repeater {
                model: [
                    { id: "basename", label: "Basename" },
                    { id: "fullpath", label: "Full path" }
                ]
                delegate: Rectangle {
                    required property var modelData
                    width: pillText.implicitWidth + 18
                    height: 22
                    radius: 11
                    color: dialog.matchMode === modelData.id
                        ? PropertiesPanelController.highlightColor
                        : PropertiesPanelController.headerColor
                    border.color: PropertiesPanelController.borderColor
                    border.width: 1
                    Text {
                        id: pillText
                        anchors.centerIn: parent
                        text: modelData.label
                        color: PropertiesPanelController.textColor
                        font.pixelSize: 10
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: dialog.matchMode = modelData.id
                    }
                }
            }
            Item { Layout.fillWidth: true }
            Rectangle {
                id: clampCheck
                width: 16; height: 16
                radius: 3
                color: PropertiesPanelController.inputColor
                border.color: PropertiesPanelController.borderColor
                border.width: 1
                Text {
                    anchors.centerIn: parent
                    text: dialog.clampOutOfRangeUVs ? "✓" : ""
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 11
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: dialog.clampOutOfRangeUVs = !dialog.clampOutOfRangeUVs
                }
            }
            InspectorLabel {
                text: "Clamp UVs outside [0..1]"
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: dialog.clampOutOfRangeUVs = !dialog.clampOutOfRangeUVs
                }
            }
        }

        // Second options row — auxiliary texture handling.
        RowLayout {
            Layout.fillWidth: true
            spacing: 14
            Rectangle {
                width: 16; height: 16
                radius: 3
                color: PropertiesPanelController.inputColor
                border.color: PropertiesPanelController.borderColor
                border.width: 1
                Text {
                    anchors.centerIn: parent
                    text: dialog.stripNonDiffuseTextures ? "✓" : ""
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 11
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: dialog.stripNonDiffuseTextures = !dialog.stripNonDiffuseTextures
                }
            }
            InspectorLabel {
                Layout.fillWidth: true
                text: "Strip normal/AO/emissive (UV0 is now diffuse-atlas-relative; auxiliary maps would sample the wrong region)"
                wrapMode: Text.WordWrap
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: dialog.stripNonDiffuseTextures = !dialog.stripNonDiffuseTextures
                }
            }
        }

        // Status line + actions.
        InspectorLabel {
            id: statusLabel
            Layout.fillWidth: true
            text: ""
            wrapMode: Text.WordWrap
        }
        Item { Layout.fillHeight: true }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            InspectorButton {
                label: "Close"
                Layout.preferredWidth: 80
                onClicked: dialog.close()
            }
            InspectorButton {
                label: "Apply Atlas"
                Layout.preferredWidth: 110
                buttonEnabled: dialog.meshPath !== ""
                    && dialog.manifestPath !== ""
                    && dialog.atlasPath !== ""
                    && dialog.outputPath !== ""
                onClicked: {
                    const err = MaterialEditorQML.applyAtlas(
                        dialog.meshPath,
                        dialog.manifestPath,
                        dialog.atlasPath,
                        dialog.outputPath,
                        dialog.matchMode,
                        dialog.clampOutOfRangeUVs,
                        dialog.stripNonDiffuseTextures)
                    if (err.length > 0) {
                        statusLabel.text = "Error: " + err
                        statusLabel.color = "#ee5555"
                    } else {
                        statusLabel.text = "Saved to " + dialog.outputPath
                        statusLabel.color = "#55cc55"
                    }
                }
            }
        }
    }
}
