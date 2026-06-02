import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import MaterialEditorQML 1.0
import PropertiesPanel 1.0

// Issue #403: mesh-aware (depth-conditioned) texture generation.
// Renders a depth map of the selected mesh and feeds it to sd.cpp
// as a ControlNet conditioning image so the generated texture
// follows the mesh's silhouette + form. Result is applied to the
// active material's diffuse slot (via the existing SD-complete
// path in MaterialEditorQML).
//
// Same Inspector-styled idiom as SkinWeightsDialog / QuadRetopoDialog.
Window {
    id: dialog
    title: "Generate Texture from Mesh"
    width: 580
    height: 440
    minimumWidth: 500
    minimumHeight: 400
    flags: Qt.Dialog
    modality: Qt.ApplicationModal
    color: PropertiesPanelController.panelColor

    property string prompt: ""
    property string controlNetPath: ""
    property double controlStrength: 0.9
    property int    depthSize: 512

    property string lastStatus: ""
    property bool   lastWasError: false

    function open() {
        dialog.lastStatus = MeshTextureController.sdAvailable
            ? ""
            : "This build has no AI texture generation (rebuild with -DENABLE_STABLE_DIFFUSION=ON)."
        dialog.lastWasError = !MeshTextureController.sdAvailable
        dialog.show(); dialog.raise(); dialog.requestActivate()
        keyCapture.forceActiveFocus()
    }

    function run() {
        if (!MeshTextureController.sdAvailable) return
        if (dialog.prompt.trim().length === 0) {
            dialog.lastStatus = "Enter a prompt first."
            dialog.lastWasError = true
            return
        }
        const err = MeshTextureController.generateForSelected(
            dialog.prompt, dialog.controlNetPath,
            dialog.controlStrength, dialog.depthSize)
        if (err && err.length > 0) {
            dialog.lastStatus = "Failed: " + err
            dialog.lastWasError = true
        } else {
            dialog.lastStatus = "Generating… (progress shows in the Material Editor)"
            dialog.lastWasError = false
        }
    }

    Item {
        id: keyCapture
        anchors.fill: parent
        focus: true
        Keys.onPressed: function(e) {
            if (e.key === Qt.Key_Escape) { dialog.close(); e.accepted = true }
            else if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter) { dialog.run(); e.accepted = true }
        }
    }

    component InspectorLabel: Text {
        color: PropertiesPanelController.textColor
        font.pixelSize: 11
    }

    component InspectorButton: Rectangle {
        id: btn
        property string label: ""
        property bool buttonEnabled: true
        signal clicked()
        activeFocusOnTab: buttonEnabled
        Accessible.role: Accessible.Button
        Accessible.name: btn.label
        Keys.onSpacePressed: if (buttonEnabled) btn.clicked()
        Keys.onReturnPressed: if (buttonEnabled) btn.clicked()
        Keys.onEnterPressed: if (buttonEnabled) btn.clicked()
        height: 26; radius: 3
        color: btnMa.containsMouse && buttonEnabled
            ? PropertiesPanelController.highlightColor
            : PropertiesPanelController.headerColor
        border.color: btn.activeFocus
            ? PropertiesPanelController.highlightColor
            : PropertiesPanelController.borderColor
        border.width: btn.activeFocus ? 2 : 1
        opacity: buttonEnabled ? 1.0 : 0.45
        Text { anchors.centerIn: parent; text: btn.label
               color: PropertiesPanelController.textColor; font.pixelSize: 11 }
        MouseArea { id: btnMa; anchors.fill: parent; hoverEnabled: true
            enabled: btn.buttonEnabled
            cursorShape: btn.buttonEnabled ? Qt.PointingHandCursor : Qt.ForbiddenCursor
            onClicked: btn.clicked() }
    }

    component InspectorInput: Rectangle {
        id: inp
        property alias text: ti.text
        property string placeholder: ""
        height: 24
        color: PropertiesPanelController.inputColor
        border.color: ti.activeFocus
            ? PropertiesPanelController.highlightColor
            : PropertiesPanelController.borderColor
        border.width: 1; radius: 3
        TextInput {
            id: ti
            anchors.fill: parent; anchors.leftMargin: 6; anchors.rightMargin: 6
            color: PropertiesPanelController.textColor; font.pixelSize: 11
            verticalAlignment: TextInput.AlignVCenter; selectByMouse: true; clip: true
        }
        Text {
            anchors.left: parent.left; anchors.leftMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            text: inp.placeholder; color: PropertiesPanelController.textColor
            opacity: 0.4; font.pixelSize: 11; visible: ti.text.length === 0
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 10

        InspectorLabel {
            Layout.fillWidth: true; wrapMode: Text.WordWrap; opacity: 0.85
            text: "Render the selected mesh's depth map and generate a texture "
                + "that follows its shape (depth-conditioned ControlNet). The "
                + "result is applied to the active material's diffuse slot.\n\n"
                + "Requires: a loaded base SD model, and (for shape conditioning) "
                + "a ControlNet depth model. Tip: run UV Unwrap first if the mesh "
                + "has no UV layout."
        }

        InspectorLabel { text: "Prompt:" }
        InspectorInput {
            Layout.fillWidth: true
            placeholder: "e.g. rusty bronze armor, weathered metal"
            onTextChanged: dialog.prompt = text
        }

        InspectorLabel { text: "ControlNet model (optional — empty = plain txt2img):" }
        RowLayout {
            Layout.fillWidth: true; spacing: 6
            InspectorInput {
                id: cnInput
                Layout.fillWidth: true
                placeholder: "path to control_v11f1p_sd15_depth…"
                onTextChanged: dialog.controlNetPath = text
            }
            InspectorButton {
                label: "Browse…"; Layout.preferredWidth: 80
                onClicked: {
                    const p = MaterialEditorQML.openFileDialog()
                    if (p && p.length > 0) { cnInput.text = p; dialog.controlNetPath = p }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true; spacing: 8
            InspectorLabel { text: "Strength:"; Layout.preferredWidth: 70 }
            Slider {
                Layout.fillWidth: true
                from: 0; to: 1; value: dialog.controlStrength
                onValueChanged: dialog.controlStrength = value
            }
            InspectorLabel {
                text: (Math.round(dialog.controlStrength * 100) / 100).toFixed(2)
                Layout.preferredWidth: 36
            }
        }

        Item { Layout.fillHeight: true }

        InspectorLabel {
            Layout.fillWidth: true
            visible: dialog.lastStatus.length > 0
            text: dialog.lastStatus; wrapMode: Text.WordWrap
            color: dialog.lastWasError ? "#cc4444" : "#3a8c3a"
        }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            InspectorButton { label: "Close"; Layout.preferredWidth: 90
                onClicked: dialog.close() }
            InspectorButton {
                label: "Generate"; Layout.preferredWidth: 130
                buttonEnabled: MeshTextureController.sdAvailable
                    && MeshTextureController.hasSelection
                onClicked: dialog.run()
            }
        }
    }
}
