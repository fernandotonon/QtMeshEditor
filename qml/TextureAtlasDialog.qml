import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import MaterialEditorQML 1.0
import PropertiesPanel 1.0

// Phase 6 slice E: pack N input textures into a single atlas image plus
// a JSON manifest of per-tile UV remaps. Mirrors the Inspector-styled
// idiom of TextureChannelPackerDialog / NormalMapGeneratorDialog
// (Rectangle + Text + MouseArea primitives over
// PropertiesPanelController.* colors).
Window {
    id: dialog
    title: "Pack Texture Atlas"
    width: 780
    height: 560
    minimumWidth: 640
    minimumHeight: 480
    flags: Qt.Dialog
    modality: Qt.ApplicationModal
    color: PropertiesPanelController.panelColor

    // Live state. sourcePaths is the ordered list of every input the
    // user has added (via drag-and-drop, the Add button, or pasted CSV).
    property var sourcePaths: []
    property int atlasWidth:  2048
    property int atlasHeight: 2048
    property int padding:     2
    property string outputPath:   ""
    property string manifestPath: ""
    property string previewDataUrl: ""

    function open() {
        dialog.show()
        dialog.raise()
        dialog.requestActivate()
        refreshPreview()
    }

    function normaliseDroppedPath(url) {
        const s = url.toString()
        return s.startsWith("file://") ? s.substring(7) : s
    }

    function addPath(path) {
        if (!path || path.length === 0) return
        const existing = dialog.sourcePaths.slice()
        if (existing.indexOf(path) !== -1) return    // de-dup
        existing.push(path)
        dialog.sourcePaths = existing
    }

    function removePathAt(idx) {
        const existing = dialog.sourcePaths.slice()
        if (idx < 0 || idx >= existing.length) return
        existing.splice(idx, 1)
        dialog.sourcePaths = existing
    }

    function clearPaths() {
        dialog.sourcePaths = []
    }

    function refreshPreview() {
        previewDataUrl = MaterialEditorQML.previewAtlas(
            dialog.sourcePaths,
            dialog.atlasWidth,
            dialog.atlasHeight,
            dialog.padding,
            256)
    }
    onSourcePathsChanged: refreshPreview()
    onAtlasWidthChanged:  refreshPreview()
    onAtlasHeightChanged: refreshPreview()
    onPaddingChanged:     refreshPreview()

    // ── Inline Inspector primitives ──────────────────────────────────

    component InspectorButton: Rectangle {
        id: btn
        property string label: ""
        property bool buttonEnabled: true
        signal clicked()
        height: 24
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

    component InspectorReadOnlyField: Rectangle {
        property alias text: t.text
        property string placeholderText: ""
        height: 24
        color: PropertiesPanelController.inputColor
        border.color: PropertiesPanelController.borderColor
        border.width: 1
        radius: 3
        Text {
            id: t
            anchors.fill: parent
            anchors.leftMargin: 6
            anchors.rightMargin: 6
            color: PropertiesPanelController.textColor
            font.pixelSize: 11
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideMiddle
        }
        Text {
            text: parent.placeholderText
            visible: t.text.length === 0
            anchors.fill: parent
            anchors.leftMargin: 6
            color: PropertiesPanelController.textColor
            opacity: 0.45
            font.pixelSize: 11
            verticalAlignment: Text.AlignVCenter
        }
    }

    component InspectorTextField: Rectangle {
        id: tfRoot
        property alias text: input.text
        property string placeholderText: ""
        signal editedText(string newText)
        height: 24
        color: PropertiesPanelController.inputColor
        border.color: input.activeFocus
            ? PropertiesPanelController.highlightColor
            : PropertiesPanelController.borderColor
        border.width: 1
        radius: 3
        TextInput {
            id: input
            anchors.fill: parent
            anchors.leftMargin: 6
            anchors.rightMargin: 6
            color: PropertiesPanelController.textColor
            font.pixelSize: 11
            verticalAlignment: TextInput.AlignVCenter
            selectByMouse: true
            clip: true
            onTextEdited: tfRoot.editedText(text)
        }
        Text {
            text: tfRoot.placeholderText
            visible: input.text.length === 0 && !input.activeFocus
            anchors.fill: parent
            anchors.leftMargin: 6
            color: PropertiesPanelController.textColor
            opacity: 0.45
            font.pixelSize: 11
            verticalAlignment: Text.AlignVCenter
        }
    }

    component InspectorNumberField: Rectangle {
        id: nf
        property int value: 0
        property int minValue: 1
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
            validator: IntValidator { bottom: nf.minValue; top: 16384 }
            onEditingFinished: {
                const n = parseInt(text, 10)
                if (!isNaN(n) && n >= nf.minValue)
                    nf.newValue(n)
                else
                    text = nf.value.toString()
            }
        }
    }

    // ── Layout ───────────────────────────────────────────────────────

    RowLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 16

        // Left column: form
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 10

            InspectorLabel {
                text: "Pack multiple textures into a single atlas image + UV manifest. " +
                      "Drag and drop files onto the list below, or click Add. " +
                      "Shelf bin-pack: tiles sorted by height descending, no rotation. " +
                      "The manifest JSON carries per-tile UV remaps for downstream tools."
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                opacity: 0.85
            }

            // Source list with drop area
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                InspectorLabel { text: "Inputs:"; Layout.preferredWidth: 64; font.bold: true }
                Item { Layout.fillWidth: true }
                InspectorButton {
                    label: "Add…"
                    Layout.preferredWidth: 70
                    onClicked: {
                        const picked = MaterialEditorQML.openFileDialog()
                        if (picked && picked.length > 0) dialog.addPath(picked)
                    }
                }
                InspectorButton {
                    label: "Clear"
                    Layout.preferredWidth: 70
                    buttonEnabled: dialog.sourcePaths.length > 0
                    onClicked: dialog.clearPaths()
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 160
                color: PropertiesPanelController.inputColor
                border.color: PropertiesPanelController.borderColor
                border.width: 1
                radius: 3

                ListView {
                    id: pathList
                    anchors.fill: parent
                    anchors.margins: 4
                    clip: true
                    spacing: 2
                    model: dialog.sourcePaths
                    delegate: Rectangle {
                        width: pathList.width
                        height: 22
                        color: rowMa.containsMouse
                            ? PropertiesPanelController.headerColor
                            : "transparent"
                        radius: 2
                        Row {
                            anchors.fill: parent
                            anchors.leftMargin: 4
                            anchors.rightMargin: 4
                            spacing: 6
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width - 24
                                text: modelData
                                color: PropertiesPanelController.textColor
                                font.pixelSize: 11
                                elide: Text.ElideMiddle
                            }
                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                width: 18; height: 18
                                radius: 2
                                color: trashMa.containsMouse
                                    ? "#cc4444"
                                    : "transparent"
                                Text {
                                    anchors.centerIn: parent
                                    text: "🗑"
                                    color: PropertiesPanelController.textColor
                                    font.pixelSize: 12
                                }
                                MouseArea {
                                    id: trashMa
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: dialog.removePathAt(index)
                                }
                            }
                        }
                        MouseArea {
                            id: rowMa
                            anchors.fill: parent
                            hoverEnabled: true
                            propagateComposedEvents: true
                            // No click action — hover-only for visual feedback;
                            // the trash icon handles removal.
                        }
                    }
                }

                // Empty-state hint when no inputs yet.
                InspectorLabel {
                    visible: dialog.sourcePaths.length === 0
                    anchors.centerIn: parent
                    text: "(drop image files here, or click Add)"
                    opacity: 0.5
                }

                // Dialog-wide drop area so the whole list rectangle accepts
                // file drops, not just the empty space below the last item.
                DropArea {
                    anchors.fill: parent
                    onDropped: drop => {
                        if (!drop.hasUrls) return
                        for (let i = 0; i < drop.urls.length; ++i)
                            dialog.addPath(dialog.normaliseDroppedPath(drop.urls[i]))
                    }
                }
            }

            // Atlas size + padding row
            RowLayout {
                spacing: 8
                Layout.fillWidth: true
                InspectorLabel { text: "Size:"; Layout.preferredWidth: 64; font.bold: true }
                InspectorNumberField {
                    Layout.preferredWidth: 80
                    value: dialog.atlasWidth
                    onNewValue: dialog.atlasWidth = v
                }
                InspectorLabel { text: "×" }
                InspectorNumberField {
                    Layout.preferredWidth: 80
                    value: dialog.atlasHeight
                    onNewValue: dialog.atlasHeight = v
                }
                InspectorLabel { text: "Padding:"; Layout.leftMargin: 12 }
                InspectorNumberField {
                    Layout.preferredWidth: 60
                    value: dialog.padding
                    minValue: 0
                    onNewValue: dialog.padding = v
                }
                Item { Layout.fillWidth: true }
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: PropertiesPanelController.borderColor }

            // Output paths
            RowLayout {
                spacing: 8
                Layout.fillWidth: true
                InspectorLabel { text: "Output:"; Layout.preferredWidth: 64; font.bold: true }
                InspectorTextField {
                    Layout.fillWidth: true
                    text: dialog.outputPath
                    placeholderText: "/path/to/atlas.png"
                    onEditedText: dialog.outputPath = newText
                }
                InspectorButton {
                    label: "Save As…"
                    Layout.preferredWidth: 80
                    onClicked: {
                        const picked = MaterialEditorQML.saveAtlasDialog()
                        if (picked && picked.length > 0) dialog.outputPath = picked
                    }
                }
            }

            RowLayout {
                spacing: 8
                Layout.fillWidth: true
                InspectorLabel { text: "Manifest:"; Layout.preferredWidth: 64 }
                InspectorTextField {
                    Layout.fillWidth: true
                    text: dialog.manifestPath
                    placeholderText: "(optional) /path/to/atlas.json"
                    onEditedText: dialog.manifestPath = newText
                }
                InspectorButton {
                    label: "Save As…"
                    Layout.preferredWidth: 80
                    onClicked: {
                        const picked = MaterialEditorQML.saveAtlasManifestDialog()
                        if (picked && picked.length > 0) dialog.manifestPath = picked
                    }
                }
            }

            InspectorLabel {
                id: statusLabel
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            Item { Layout.fillHeight: true }

            // Action buttons
            RowLayout {
                spacing: 8
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                InspectorButton {
                    label: "Close"
                    Layout.preferredWidth: 80
                    onClicked: dialog.close()
                }
                // Phase 6 slice E2: open the Apply Atlas dialog. Niche
                // follow-up tool — surfaced here instead of the panel
                // so it doesn't clutter the general UI. Auto-fills the
                // atlas image + manifest paths from this dialog's state
                // when they're set, so the typical "pack → apply" flow
                // is one extra click after Pack.
                InspectorButton {
                    label: "Apply to Mesh…"
                    Layout.preferredWidth: 120
                    onClicked: dialog.openApplyAtlasDialog()
                    MouseArea {
                        id: applyBtnMa
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.NoButton // tooltip-only; the InspectorButton's own MouseArea handles clicks
                        ToolTip.visible: containsMouse
                        ToolTip.delay: 500
                        ToolTip.text: "Consume the manifest above and apply it to a mesh: remap UV0 + rebind the diffuse texture."
                    }
                }
                InspectorButton {
                    label: "Pack Atlas"
                    Layout.preferredWidth: 100
                    buttonEnabled: dialog.sourcePaths.length > 0
                        && dialog.outputPath !== ""
                    onClicked: {
                        const err = MaterialEditorQML.packAtlas(
                            dialog.sourcePaths,
                            dialog.atlasWidth,
                            dialog.atlasHeight,
                            dialog.padding,
                            dialog.outputPath,
                            dialog.manifestPath)
                        if (err.length > 0) {
                            statusLabel.text = "Error: " + err
                            statusLabel.color = "#ee5555"
                        } else {
                            statusLabel.text = "Saved to " + dialog.outputPath
                                + (dialog.manifestPath.length > 0
                                    ? " (manifest: " + dialog.manifestPath + ")"
                                    : "")
                            statusLabel.color = "#55cc55"
                        }
                    }
                }
            }
        }

        // Right column: preview thumbnail
        ColumnLayout {
            Layout.preferredWidth: 256
            Layout.fillHeight: true
            spacing: 8
            InspectorLabel {
                text: "Preview"
                font.bold: true
                Layout.alignment: Qt.AlignHCenter
            }
            Rectangle {
                Layout.preferredWidth: 256
                Layout.preferredHeight: 256
                Layout.alignment: Qt.AlignHCenter
                color: PropertiesPanelController.inputColor
                border.color: PropertiesPanelController.borderColor
                border.width: 1
                radius: 3
                Image {
                    anchors.fill: parent
                    anchors.margins: 1
                    source: dialog.previewDataUrl
                    fillMode: Image.PreserveAspectFit
                    smooth: false
                    cache: false
                    asynchronous: true
                }
                InspectorLabel {
                    visible: dialog.previewDataUrl.length === 0
                    anchors.centerIn: parent
                    text: dialog.sourcePaths.length === 0
                        ? "(add some inputs)"
                        : "(packing failed — inputs may exceed atlas)"
                    opacity: 0.5
                    horizontalAlignment: Text.AlignHCenter
                    width: parent.width - 20
                    wrapMode: Text.WordWrap
                }
            }
            InspectorLabel {
                Layout.alignment: Qt.AlignHCenter
                text: dialog.sourcePaths.length > 0
                    ? dialog.sourcePaths.length + " input(s)"
                    : ""
                opacity: 0.7
            }
            Item { Layout.fillHeight: true }
        }
    }

    // Phase 6 slice E2: child Apply Atlas dialog. Loaded on demand to
    // keep startup cost down; the parent pack dialog auto-fills the
    // freshly-saved atlas + manifest paths so a "pack → apply" flow
    // is one click after Pack.
    Loader {
        id: applyAtlasLoader
        active: false
        source: "qrc:/MaterialEditorQML/ApplyAtlasDialog.qml"
        onLoaded: {
            if (!item) return
            if (dialog.outputPath.length > 0)   item.atlasPath    = dialog.outputPath
            if (dialog.manifestPath.length > 0) item.manifestPath = dialog.manifestPath
            if (item.open) item.open()
        }
    }
    function openApplyAtlasDialog() {
        if (!applyAtlasLoader.active) {
            applyAtlasLoader.active = true
        } else if (applyAtlasLoader.item) {
            if (dialog.outputPath.length > 0)   applyAtlasLoader.item.atlasPath    = dialog.outputPath
            if (dialog.manifestPath.length > 0) applyAtlasLoader.item.manifestPath = dialog.manifestPath
            applyAtlasLoader.item.open()
        }
    }
}
