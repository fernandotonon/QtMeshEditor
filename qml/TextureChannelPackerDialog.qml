import QtQuick
import QtQuick.Layouts
import QtQuick.Window
import MaterialEditorQML 1.0
import PropertiesPanel 1.0

// Top-level Window so the dialog has its own OS-level frame and isn't
// constrained by the docked PropertiesPanel widget. Slice G surfaces
// this from Material Mode → Mode Tools → "Pack Texture Channels…".
//
// Styled to match the Inspector look (Rectangle + Text + MouseArea
// primitives over PropertiesPanelController.* colors), not the Material
// Editor's QtQuick.Controls look.
Window {
    id: dialog
    title: "Pack Texture Channels"
    width: 760
    height: 460
    minimumWidth: 640
    minimumHeight: 420
    flags: Qt.Dialog
    modality: Qt.ApplicationModal
    color: PropertiesPanelController.panelColor

    // Per-channel state.
    property string redPath:   ""
    property string greenPath: ""
    property string bluePath:  ""
    property string alphaPath: ""
    property real redConstant:   0.0
    property real greenConstant: 0.0
    property real blueConstant:  0.0
    property real alphaConstant: 1.0
    property bool invertRed:   false
    property bool invertGreen: false
    property bool invertBlue:  false
    property bool invertAlpha: false
    property bool includeAlpha: true
    property string outputPath: ""

    // Slice G2: live preview of the pack output. Holds a base64 PNG
    // data URL produced by MaterialEditorQML.previewPackedTextureChannels.
    property string previewDataUrl: ""

    function open() {
        dialog.show()
        dialog.raise()
        dialog.requestActivate()
        refreshPreview()
    }

    // Strip the `file://` scheme some drag/drop sources prepend so the
    // packer's QImageReader sees a plain filesystem path.
    function normaliseDroppedPath(url) {
        const s = url.toString()
        return s.startsWith("file://") ? s.substring(7) : s
    }

    function refreshPreview() {
        previewDataUrl = MaterialEditorQML.previewPackedTextureChannels(
            redPath, greenPath, bluePath, alphaPath,
            redConstant, greenConstant, blueConstant, alphaConstant,
            invertRed, invertGreen, invertBlue, invertAlpha,
            includeAlpha, 256)
    }

    onRedPathChanged:     refreshPreview()
    onGreenPathChanged:   refreshPreview()
    onBluePathChanged:    refreshPreview()
    onAlphaPathChanged:   refreshPreview()
    onRedConstantChanged:   refreshPreview()
    onGreenConstantChanged: refreshPreview()
    onBlueConstantChanged:  refreshPreview()
    onAlphaConstantChanged: refreshPreview()
    onInvertRedChanged:     refreshPreview()
    onInvertGreenChanged:   refreshPreview()
    onInvertBlueChanged:    refreshPreview()
    onInvertAlphaChanged:   refreshPreview()
    onIncludeAlphaChanged:  refreshPreview()

    // Slice G3: one-click presets that wire the typical channel mapping
    // for the named convention. The user still picks the source files;
    // presets only set which file goes in which channel + invert flags
    // + include-alpha. Each preset is idempotent — calling it overrides
    // the current channel layout.
    function applyUnityOrmPreset() {
        // Unity ORM: AO → R, Roughness → G, Metallic → B. Alpha slot
        // typically unused; keep alpha included with constant 1.0.
        const oldAo = redPath, oldRough = greenPath, oldMetal = bluePath
        // The user may have placed sources under any of the three
        // channels — try to detect by filename heuristic, otherwise
        // leave them where they are.
        const lower = function(p) { return p.toLowerCase() }
        const all = [oldAo, oldRough, oldMetal, alphaPath]
        const find = function(needle) {
            for (var i = 0; i < all.length; ++i)
                if (all[i] && lower(all[i]).indexOf(needle) >= 0) return all[i]
            return ""
        }
        const ao = find("ao") || find("occlusion") || oldAo
        const rough = find("rough") || oldRough
        const metal = find("metal") || oldMetal
        redPath = ao
        greenPath = rough
        bluePath = metal
        alphaPath = ""
        redConstant = 1.0   // sensible default if AO source is missing
        greenConstant = 0.5
        blueConstant = 0.0
        alphaConstant = 1.0
        invertRed = false
        invertGreen = false
        invertBlue = false
        invertAlpha = false
        includeAlpha = true
    }

    function applyUnrealMrPreset() {
        // Unreal MR: Metallic → R, Roughness → G, no alpha.
        const lower = function(p) { return p.toLowerCase() }
        const all = [redPath, greenPath, bluePath, alphaPath]
        const find = function(needle) {
            for (var i = 0; i < all.length; ++i)
                if (all[i] && lower(all[i]).indexOf(needle) >= 0) return all[i]
            return ""
        }
        redPath = find("metal") || redPath
        greenPath = find("rough") || greenPath
        bluePath = ""
        alphaPath = ""
        redConstant = 0.0
        greenConstant = 0.5
        blueConstant = 0.0
        alphaConstant = 1.0
        invertRed = false
        invertGreen = false
        invertBlue = false
        invertAlpha = false
        includeAlpha = false
    }

    function applySpecGlossInvertPreset() {
        // Convert a Roughness map to a Glossiness map by inverting R.
        // Common spec-gloss → metal-rough conversion.
        const lower = function(p) { return p.toLowerCase() }
        const all = [redPath, greenPath, bluePath, alphaPath]
        const find = function(needle) {
            for (var i = 0; i < all.length; ++i)
                if (all[i] && lower(all[i]).indexOf(needle) >= 0) return all[i]
            return ""
        }
        const rough = find("rough") || redPath || greenPath
        redPath = rough
        greenPath = ""
        bluePath = ""
        alphaPath = ""
        redConstant = 0.0
        greenConstant = 0.0
        blueConstant = 0.0
        alphaConstant = 1.0
        invertRed = true     // ← the actual conversion
        invertGreen = false
        invertBlue = false
        invertAlpha = false
        includeAlpha = false
    }

    // ── Inline styled primitives ───────────────────────────────────

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

    // Read-only path field with focus highlight.
    component InspectorReadOnlyField: Rectangle {
        property alias text: t.text
        property string placeholderText: ""
        property bool fieldEnabled: true
        height: 24
        color: PropertiesPanelController.inputColor
        border.color: PropertiesPanelController.borderColor
        border.width: 1
        radius: 3
        opacity: fieldEnabled ? 1.0 : 0.5
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

    // Editable text field (used for the output path).
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

    // Integer percent field with up/down arrows. Same idiom as
    // TransformField but simpler (0..100 only, no decimals).
    component InspectorPercentField: Rectangle {
        id: pctRoot
        property int value: 0
        property bool fieldEnabled: true
        signal newValue(int v)
        height: 24
        color: PropertiesPanelController.inputColor
        border.color: pctInput.activeFocus
            ? PropertiesPanelController.highlightColor
            : PropertiesPanelController.borderColor
        border.width: 1
        radius: 3
        opacity: fieldEnabled ? 1.0 : 0.5

        function clamp(v) { return Math.max(0, Math.min(100, v)) }

        TextInput {
            id: pctInput
            anchors.left: parent.left
            anchors.right: arrows.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.leftMargin: 6
            anchors.rightMargin: 4
            text: pctRoot.value
            color: PropertiesPanelController.textColor
            font.pixelSize: 11
            verticalAlignment: TextInput.AlignVCenter
            horizontalAlignment: TextInput.AlignRight
            selectByMouse: true
            readOnly: !pctRoot.fieldEnabled
            validator: IntValidator { bottom: 0; top: 100 }
            onEditingFinished: {
                const v = pctRoot.clamp(parseInt(text) || 0)
                text = v
                pctRoot.newValue(v)
            }
            Keys.onUpPressed:   { const v = pctRoot.clamp(pctRoot.value + 5); pctRoot.newValue(v) }
            Keys.onDownPressed: { const v = pctRoot.clamp(pctRoot.value - 5); pctRoot.newValue(v) }
        }

        Text {
            visible: pctRoot.fieldEnabled
            anchors.right: arrows.left
            anchors.verticalCenter: parent.verticalCenter
            anchors.rightMargin: 1
            text: "%"
            color: PropertiesPanelController.textColor
            font.pixelSize: 10
            opacity: 0.55
        }

        Column {
            id: arrows
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 14

            Rectangle {
                width: parent.width
                height: parent.height / 2
                color: upMa.containsMouse && pctRoot.fieldEnabled
                    ? Qt.lighter(PropertiesPanelController.panelColor, 1.2)
                    : PropertiesPanelController.panelColor
                border.color: PropertiesPanelController.borderColor
                border.width: 1
                Text {
                    anchors.centerIn: parent
                    text: "▲"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 6
                }
                MouseArea {
                    id: upMa
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: pctRoot.fieldEnabled
                    onClicked: pctRoot.newValue(pctRoot.clamp(pctRoot.value + 5))
                }
            }

            Rectangle {
                width: parent.width
                height: parent.height / 2
                color: downMa.containsMouse && pctRoot.fieldEnabled
                    ? Qt.lighter(PropertiesPanelController.panelColor, 1.2)
                    : PropertiesPanelController.panelColor
                border.color: PropertiesPanelController.borderColor
                border.width: 1
                Text {
                    anchors.centerIn: parent
                    text: "▼"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 6
                }
                MouseArea {
                    id: downMa
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: pctRoot.fieldEnabled
                    onClicked: pctRoot.newValue(pctRoot.clamp(pctRoot.value - 5))
                }
            }
        }
    }

    component InspectorCheckBox: Rectangle {
        id: cbRoot
        property bool checked: false
        property string label: ""
        property bool boxEnabled: true
        signal toggled(bool newChecked)
        // Hit area for both the box and label.
        height: 22
        color: "transparent"
        // When there's a label, the box+label sits at the left edge so
        // labels read naturally. When there's no label (table column
        // usage), the box is centered horizontally.
        Row {
            spacing: 6
            anchors.left: cbRoot.label.length > 0 ? parent.left : undefined
            anchors.verticalCenter: parent.verticalCenter
            anchors.horizontalCenter: cbRoot.label.length === 0 ? parent.horizontalCenter : undefined
            Rectangle {
                width: 16; height: 16
                anchors.verticalCenter: parent.verticalCenter
                color: cbRoot.checked && cbRoot.boxEnabled
                    ? PropertiesPanelController.highlightColor
                    : PropertiesPanelController.inputColor
                border.color: PropertiesPanelController.borderColor
                border.width: 1
                radius: 2
                opacity: cbRoot.boxEnabled ? 1.0 : 0.45
                Text {
                    anchors.centerIn: parent
                    visible: cbRoot.checked
                    text: "✓"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 12
                    font.bold: true
                }
            }
            Text {
                visible: cbRoot.label.length > 0
                anchors.verticalCenter: parent.verticalCenter
                text: cbRoot.label
                color: PropertiesPanelController.textColor
                font.pixelSize: 11
                opacity: cbRoot.boxEnabled ? 1.0 : 0.45
            }
        }
        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            enabled: cbRoot.boxEnabled
            cursorShape: cbRoot.boxEnabled ? Qt.PointingHandCursor : Qt.ForbiddenCursor
            onClicked: cbRoot.toggled(!cbRoot.checked)
        }
    }

    // ── Layout ────────────────────────────────────────────────────

    // Two-column split: channel rows on the left, live preview on the right.
    RowLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 16

        // ── Left column: presets + channel rows + output ─────────
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 10

            InspectorLabel {
                text: "Pack 1–4 grayscale source images into a single RGBA texture. " +
                      "Drag files onto a row, click Browse, or use a preset below."
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                opacity: 0.85
            }

            // Slice G3: one-click presets
            RowLayout {
                spacing: 8
                Layout.fillWidth: true
                InspectorLabel { text: "Presets:"; Layout.preferredWidth: 64; font.bold: true }
                InspectorButton {
                    label: "Unity ORM"
                    Layout.preferredWidth: 100
                    onClicked: dialog.applyUnityOrmPreset()
                }
                InspectorButton {
                    label: "Unreal MR"
                    Layout.preferredWidth: 100
                    onClicked: dialog.applyUnrealMrPreset()
                }
                InspectorButton {
                    label: "Spec → Gloss"
                    Layout.preferredWidth: 110
                    onClicked: dialog.applySpecGlossInvertPreset()
                }
                Item { Layout.fillWidth: true }
            }

            // Header row labels — column widths must match the rows below.
            RowLayout {
                spacing: 8
                Layout.fillWidth: true
                InspectorLabel { text: "Ch.";       Layout.preferredWidth: 24; font.bold: true }
                InspectorLabel { text: "Source";    Layout.fillWidth: true; font.bold: true }
                Item {                              Layout.preferredWidth: 80 }
                InspectorLabel { text: "Const";     Layout.preferredWidth: 92; horizontalAlignment: Text.AlignHCenter; font.bold: true }
                InspectorLabel { text: "Invert";    Layout.preferredWidth: 56; horizontalAlignment: Text.AlignHCenter; font.bold: true }
                Item {                              Layout.preferredWidth: 28 }
            }

        // Red row
        RowLayout {
            spacing: 8
            Layout.fillWidth: true
            Rectangle {
                width: 24; height: 22
                radius: 2
                color: "#ee7777"
                Layout.preferredWidth: 24
                Text { anchors.centerIn: parent; text: "R"; color: "white"; font.pixelSize: 11; font.bold: true }
            }
            InspectorReadOnlyField {
                Layout.fillWidth: true
                text: dialog.redPath
                placeholderText: "(empty → use constant or drop a file)"
                DropArea {
                    anchors.fill: parent
                    onDropped: drop => {
                        if (drop.hasUrls && drop.urls.length > 0)
                            dialog.redPath = dialog.normaliseDroppedPath(drop.urls[0])
                    }
                }
            }
            InspectorButton {
                label: "Browse…"
                Layout.preferredWidth: 80
                onClicked: {
                    const picked = MaterialEditorQML.openFileDialog()
                    if (picked && picked.length > 0) dialog.redPath = picked
                }
            }
            InspectorPercentField {
                Layout.preferredWidth: 92
                value: Math.round(dialog.redConstant * 100)
                fieldEnabled: dialog.redPath === ""
                onNewValue: dialog.redConstant = v / 100.0
            }
            InspectorCheckBox {
                Layout.preferredWidth: 56
                checked: dialog.invertRed
                onToggled: dialog.invertRed = newChecked
            }
            InspectorButton {
                label: "🗑"
                Layout.preferredWidth: 28
                buttonEnabled: dialog.redPath !== "" || dialog.redConstant > 0 || dialog.invertRed
                onClicked: {
                    dialog.redPath = ""
                    dialog.redConstant = 0.0
                    dialog.invertRed = false
                }
            }
        }

        // Green row
        RowLayout {
            spacing: 8
            Layout.fillWidth: true
            Rectangle {
                width: 24; height: 22
                radius: 2
                color: "#77cc77"
                Layout.preferredWidth: 24
                Text { anchors.centerIn: parent; text: "G"; color: "white"; font.pixelSize: 11; font.bold: true }
            }
            InspectorReadOnlyField {
                Layout.fillWidth: true
                text: dialog.greenPath
                placeholderText: "(empty → use constant or drop a file)"
                DropArea {
                    anchors.fill: parent
                    onDropped: drop => {
                        if (drop.hasUrls && drop.urls.length > 0)
                            dialog.greenPath = dialog.normaliseDroppedPath(drop.urls[0])
                    }
                }
            }
            InspectorButton {
                label: "Browse…"
                Layout.preferredWidth: 80
                onClicked: {
                    const picked = MaterialEditorQML.openFileDialog()
                    if (picked && picked.length > 0) dialog.greenPath = picked
                }
            }
            InspectorPercentField {
                Layout.preferredWidth: 92
                value: Math.round(dialog.greenConstant * 100)
                fieldEnabled: dialog.greenPath === ""
                onNewValue: dialog.greenConstant = v / 100.0
            }
            InspectorCheckBox {
                Layout.preferredWidth: 56
                checked: dialog.invertGreen
                onToggled: dialog.invertGreen = newChecked
            }
            InspectorButton {
                label: "🗑"
                Layout.preferredWidth: 28
                buttonEnabled: dialog.greenPath !== "" || dialog.greenConstant > 0 || dialog.invertGreen
                onClicked: {
                    dialog.greenPath = ""
                    dialog.greenConstant = 0.0
                    dialog.invertGreen = false
                }
            }
        }

        // Blue row
        RowLayout {
            spacing: 8
            Layout.fillWidth: true
            Rectangle {
                width: 24; height: 22
                radius: 2
                color: "#7799ee"
                Layout.preferredWidth: 24
                Text { anchors.centerIn: parent; text: "B"; color: "white"; font.pixelSize: 11; font.bold: true }
            }
            InspectorReadOnlyField {
                Layout.fillWidth: true
                text: dialog.bluePath
                placeholderText: "(empty → use constant or drop a file)"
                DropArea {
                    anchors.fill: parent
                    onDropped: drop => {
                        if (drop.hasUrls && drop.urls.length > 0)
                            dialog.bluePath = dialog.normaliseDroppedPath(drop.urls[0])
                    }
                }
            }
            InspectorButton {
                label: "Browse…"
                Layout.preferredWidth: 80
                onClicked: {
                    const picked = MaterialEditorQML.openFileDialog()
                    if (picked && picked.length > 0) dialog.bluePath = picked
                }
            }
            InspectorPercentField {
                Layout.preferredWidth: 92
                value: Math.round(dialog.blueConstant * 100)
                fieldEnabled: dialog.bluePath === ""
                onNewValue: dialog.blueConstant = v / 100.0
            }
            InspectorCheckBox {
                Layout.preferredWidth: 56
                checked: dialog.invertBlue
                onToggled: dialog.invertBlue = newChecked
            }
            InspectorButton {
                label: "🗑"
                Layout.preferredWidth: 28
                buttonEnabled: dialog.bluePath !== "" || dialog.blueConstant > 0 || dialog.invertBlue
                onClicked: {
                    dialog.bluePath = ""
                    dialog.blueConstant = 0.0
                    dialog.invertBlue = false
                }
            }
        }

        // Alpha row
        RowLayout {
            spacing: 8
            Layout.fillWidth: true
            Rectangle {
                width: 24; height: 22
                radius: 2
                color: PropertiesPanelController.headerColor
                border.color: PropertiesPanelController.borderColor
                border.width: 1
                Layout.preferredWidth: 24
                opacity: dialog.includeAlpha ? 1.0 : 0.5
                Text { anchors.centerIn: parent; text: "A"; color: PropertiesPanelController.textColor; font.pixelSize: 11; font.bold: true }
            }
            InspectorReadOnlyField {
                Layout.fillWidth: true
                text: dialog.alphaPath
                placeholderText: "(empty → use constant or drop a file)"
                fieldEnabled: dialog.includeAlpha
                DropArea {
                    anchors.fill: parent
                    enabled: dialog.includeAlpha
                    onDropped: drop => {
                        if (drop.hasUrls && drop.urls.length > 0)
                            dialog.alphaPath = dialog.normaliseDroppedPath(drop.urls[0])
                    }
                }
            }
            InspectorButton {
                label: "Browse…"
                Layout.preferredWidth: 80
                buttonEnabled: dialog.includeAlpha
                onClicked: {
                    const picked = MaterialEditorQML.openFileDialog()
                    if (picked && picked.length > 0) dialog.alphaPath = picked
                }
            }
            InspectorPercentField {
                Layout.preferredWidth: 92
                value: Math.round(dialog.alphaConstant * 100)
                fieldEnabled: dialog.includeAlpha && dialog.alphaPath === ""
                onNewValue: dialog.alphaConstant = v / 100.0
            }
            InspectorCheckBox {
                Layout.preferredWidth: 56
                checked: dialog.invertAlpha
                boxEnabled: dialog.includeAlpha
                onToggled: dialog.invertAlpha = newChecked
            }
            InspectorButton {
                label: "🗑"
                Layout.preferredWidth: 28
                buttonEnabled: dialog.includeAlpha &&
                    (dialog.alphaPath !== "" || dialog.alphaConstant !== 1.0 || dialog.invertAlpha)
                onClicked: {
                    dialog.alphaPath = ""
                    // Alpha defaults to 1.0 (opaque) — that's the "neutral"
                    // value for the alpha channel, distinct from R/G/B's 0.
                    dialog.alphaConstant = 1.0
                    dialog.invertAlpha = false
                }
            }
        }

        // Include-alpha master toggle
        InspectorCheckBox {
            Layout.fillWidth: true
            checked: dialog.includeAlpha
            label: "Include alpha channel (RGBA8) — uncheck for RGB888"
            onToggled: dialog.includeAlpha = newChecked
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: PropertiesPanelController.borderColor }

        // Output path
        RowLayout {
            spacing: 8
            Layout.fillWidth: true
            InspectorLabel {
                text: "Output:"
                Layout.preferredWidth: 64
                font.bold: true
            }
            InspectorTextField {
                Layout.fillWidth: true
                text: dialog.outputPath
                placeholderText: "/path/to/packed.png"
                onEditedText: dialog.outputPath = newText
            }
            InspectorButton {
                label: "Save As…"
                Layout.preferredWidth: 80
                onClicked: {
                    const picked = MaterialEditorQML.savePackedTextureDialog()
                    if (picked && picked.length > 0) dialog.outputPath = picked
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
            InspectorButton {
                label: "Pack"
                Layout.preferredWidth: 80
                buttonEnabled: dialog.outputPath !== ""
                onClicked: {
                    const err = MaterialEditorQML.packTextureChannels(
                        dialog.redPath, dialog.greenPath, dialog.bluePath, dialog.alphaPath,
                        dialog.redConstant, dialog.greenConstant, dialog.blueConstant, dialog.alphaConstant,
                        dialog.invertRed, dialog.invertGreen, dialog.invertBlue, dialog.invertAlpha,
                        dialog.includeAlpha, dialog.outputPath)
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
        }   // ── end left column ──

        // ── Right column: live preview thumbnail ─────────────────
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

                // Subtle checkerboard so transparent pixels are obvious.
                Image {
                    anchors.fill: parent
                    anchors.margins: 1
                    source: "qrc:/MaterialEditorQML/checker_bg.png"
                    fillMode: Image.Tile
                    visible: false   // (no checker asset shipped — left for a future iteration)
                }

                Image {
                    id: previewImage
                    anchors.fill: parent
                    anchors.margins: 1
                    source: dialog.previewDataUrl
                    fillMode: Image.PreserveAspectFit
                    smooth: false   // crisp 1:1 channel preview
                    cache: false    // re-decode every time the data URL flips
                    asynchronous: true
                }

                InspectorLabel {
                    visible: dialog.previewDataUrl.length === 0
                    anchors.centerIn: parent
                    text: "(no preview)"
                    opacity: 0.5
                }
            }
            InspectorLabel {
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignHCenter
                horizontalAlignment: Text.AlignHCenter
                text: "Updates live as inputs change. " +
                      "Output written when you click Pack."
                opacity: 0.7
                font.pixelSize: 10
            }
            Item { Layout.fillHeight: true }
        }
    }
}
