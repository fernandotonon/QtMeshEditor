import QtQuick
import QtQuick.Layouts
import QtQuick.Window
import MaterialEditorQML 1.0
import PropertiesPanel 1.0

// Slice H: top-level Window for generating tangent-space normal maps
// from a height/bump source. Same Inspector-styled idiom as
// TextureChannelPackerDialog (Rectangle + Text + MouseArea primitives
// over PropertiesPanelController.* colors).
Window {
    id: dialog
    title: "Generate Normal Map"
    width: 660
    height: 410
    minimumWidth: 540
    minimumHeight: 360
    flags: Qt.Dialog
    modality: Qt.ApplicationModal
    color: PropertiesPanelController.panelColor

    property string sourcePath: ""
    property real   strength:    2.0
    property bool   invertR:     false
    property bool   invertG:     false   // OpenGL +Y up by default
    property string outputPath:  ""
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

    function refreshPreview() {
        previewDataUrl = MaterialEditorQML.previewNormalMap(
            sourcePath, strength, invertR, invertG, 256)
    }
    onSourcePathChanged: refreshPreview()
    onStrengthChanged:   refreshPreview()
    onInvertRChanged:    refreshPreview()
    onInvertGChanged:    refreshPreview()

    // ── Inline Inspector primitives (same as TextureChannelPackerDialog) ──

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

    component InspectorCheckBox: Rectangle {
        id: cbRoot
        property bool checked: false
        property string label: ""
        signal toggled(bool newChecked)
        height: 22
        color: "transparent"
        Row {
            spacing: 6
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            Rectangle {
                width: 16; height: 16
                anchors.verticalCenter: parent.verticalCenter
                color: cbRoot.checked
                    ? PropertiesPanelController.highlightColor
                    : PropertiesPanelController.inputColor
                border.color: PropertiesPanelController.borderColor
                border.width: 1
                radius: 2
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
            }
        }
        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: cbRoot.toggled(!cbRoot.checked)
        }
    }

    // Strength slider — track + thumb + readout. Mirrors the inspector
    // primitive pattern but tailored to a 0..10 float range.
    component StrengthSlider: Rectangle {
        id: sl
        property real value: 2.0
        property real minValue: 0.0
        property real maxValue: 10.0
        signal newValue(real v)
        height: 22
        color: PropertiesPanelController.inputColor
        border.color: PropertiesPanelController.borderColor
        border.width: 1
        radius: 3
        function clampVal(v) { return Math.max(sl.minValue, Math.min(sl.maxValue, v)) }
        function pctToVal(p) { return sl.minValue + p * (sl.maxValue - sl.minValue) }
        function valToPct(v) { return (v - sl.minValue) / (sl.maxValue - sl.minValue) }

        Rectangle {
            id: fill
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.margins: 1
            width: Math.max(0, (parent.width - 2) * sl.valToPct(sl.value))
            color: PropertiesPanelController.highlightColor
            opacity: 0.5
            radius: 2
        }
        Text {
            anchors.centerIn: parent
            text: sl.value.toFixed(2)
            color: PropertiesPanelController.textColor
            font.pixelSize: 11
        }
        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            onPressed: mouse => {
                const pct = Math.max(0, Math.min(1, mouse.x / parent.width))
                sl.newValue(sl.clampVal(sl.pctToVal(pct)))
            }
            onPositionChanged: mouse => {
                if (pressed) {
                    const pct = Math.max(0, Math.min(1, mouse.x / parent.width))
                    sl.newValue(sl.clampVal(sl.pctToVal(pct)))
                }
            }
        }
    }

    // ── Layout ────────────────────────────────────────────────────

    RowLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 16

        // Left: form
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 10

            InspectorLabel {
                text: "Generate a tangent-space normal map from a grayscale " +
                      "height/bump source. Drop a file onto the source field, " +
                      "or click Browse."
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                opacity: 0.85
            }

            // Source row
            RowLayout {
                spacing: 8
                Layout.fillWidth: true
                InspectorLabel { text: "Source:"; Layout.preferredWidth: 64; font.bold: true }
                InspectorReadOnlyField {
                    Layout.fillWidth: true
                    text: dialog.sourcePath
                    placeholderText: "(drop a height/bump map or click Browse)"
                    DropArea {
                        anchors.fill: parent
                        onDropped: drop => {
                            if (drop.hasUrls && drop.urls.length > 0)
                                dialog.sourcePath = dialog.normaliseDroppedPath(drop.urls[0])
                        }
                    }
                }
                InspectorButton {
                    label: "Browse…"
                    Layout.preferredWidth: 80
                    onClicked: {
                        const picked = MaterialEditorQML.openFileDialog()
                        if (picked && picked.length > 0) dialog.sourcePath = picked
                    }
                }
            }

            // Strength row
            RowLayout {
                spacing: 8
                Layout.fillWidth: true
                InspectorLabel { text: "Strength:"; Layout.preferredWidth: 64; font.bold: true }
                StrengthSlider {
                    Layout.fillWidth: true
                    value: dialog.strength
                    onNewValue: dialog.strength = v
                }
            }

            // Invert toggles
            RowLayout {
                spacing: 16
                Layout.fillWidth: true
                InspectorCheckBox {
                    Layout.preferredWidth: 130
                    checked: dialog.invertR
                    label: "Invert R"
                    onToggled: dialog.invertR = newChecked
                }
                InspectorCheckBox {
                    Layout.preferredWidth: 200
                    checked: dialog.invertG
                    label: "Invert G (DirectX +Y down)"
                    onToggled: dialog.invertG = newChecked
                }
                Item { Layout.fillWidth: true }
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: PropertiesPanelController.borderColor }

            // Output path
            RowLayout {
                spacing: 8
                Layout.fillWidth: true
                InspectorLabel { text: "Output:"; Layout.preferredWidth: 64; font.bold: true }
                InspectorTextField {
                    Layout.fillWidth: true
                    text: dialog.outputPath
                    placeholderText: "/path/to/normal.png"
                    onEditedText: dialog.outputPath = newText
                }
                InspectorButton {
                    label: "Save As…"
                    Layout.preferredWidth: 80
                    onClicked: {
                        const picked = MaterialEditorQML.saveNormalMapDialog()
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
                    label: "Generate"
                    Layout.preferredWidth: 90
                    buttonEnabled: dialog.sourcePath !== "" && dialog.outputPath !== ""
                    onClicked: {
                        const err = MaterialEditorQML.generateNormalMap(
                            dialog.sourcePath, dialog.strength,
                            dialog.invertR, dialog.invertG,
                            dialog.outputPath)
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

        // Right: preview thumbnail
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
                    text: "(pick a source)"
                    opacity: 0.5
                }
            }
            Item { Layout.fillHeight: true }
        }
    }
}
