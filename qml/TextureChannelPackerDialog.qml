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
    width: 580
    height: 380
    minimumWidth: 480
    minimumHeight: 350
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

    function open() {
        dialog.show()
        dialog.raise()
        dialog.requestActivate()
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

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 10

        InspectorLabel {
            text: "Pack 1–4 grayscale source images into a single RGBA texture. " +
                  "Each output channel takes either a source image (sampled as luminance) " +
                  "or a constant 0–100% value when no path is set."
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
            opacity: 0.85
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
                placeholderText: "(empty → use constant)"
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
                placeholderText: "(empty → use constant)"
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
                placeholderText: "(empty → use constant)"
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
                placeholderText: "(empty → use constant)"
                fieldEnabled: dialog.includeAlpha
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
    }
}
