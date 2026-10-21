pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import AnimationControl 1.0
import PropertiesPanel 1.0

// ── Character generator ───────────────────────────────────────────────────
// A rigged human (MHR body + ICT-FaceKit head as separate parts, 52-joint skeleton, 51 ARKit morph targets). Random seed or
// hand-set shape coefficients ("index=value,..."), skin and eye colour. Generate adds a new node; Ctrl+Z removes it.
Column {
    id: sec
    width: parent ? parent.width : 300
    spacing: 6
    property int seed: 1

    component Label: Text {
        color: AnimationControlController.textColor
        font.pixelSize: 11
        verticalAlignment: Text.AlignVCenter
    }
    component Note: Text {
        width: sec.width
        wrapMode: Text.WordWrap
        color: AnimationControlController.textColor
        opacity: 0.7
        font.pixelSize: 11
    }
    component ToolBtn: Rectangle {
        property string label: ""
        property bool enabled: true
        property bool accent: false
        signal clicked()
        width: Math.max(28, lblT.implicitWidth + 12); height: 22; radius: 3
        color: accent ? AnimationControlController.highlightColor
             : maT.pressed ? Qt.darker(AnimationControlController.buttonColor, 1.3)
             : maT.containsMouse ? Qt.lighter(AnimationControlController.buttonColor, 1.15)
             : AnimationControlController.buttonColor
        border.color: AnimationControlController.borderColor; border.width: 1
        opacity: enabled ? 1.0 : 0.4
        Accessible.role: Accessible.Button
        Accessible.name: label
        Text { id: lblT; anchors.centerIn: parent; text: parent.label; color: AnimationControlController.buttonTextColor; font.pixelSize: 11 }
        MouseArea { id: maT; anchors.fill: parent; hoverEnabled: true; enabled: parent.enabled; onClicked: parent.clicked() }
    }
    component Field: TextField {
        height: 22; font.pixelSize: 11
        color: AnimationControlController.textColor
        background: Rectangle { color: AnimationControlController.buttonColor; border.color: AnimationControlController.borderColor; radius: 3 }
    }

    Note { text: "Generates a rigged human: body + head as separate parts, standard 52-joint skeleton, 51 ARKit expression shapes." }

    Row {
        spacing: 4
        Label { width: 52; height: 22; text: "Seed" }
        Field { id: seedField; width: 60; text: String(sec.seed); inputMethodHints: Qt.ImhDigitsOnly; onEditingFinished: sec.seed = parseInt(text) || 0 }
        ToolBtn { label: "Random"; onClicked: { sec.seed = Math.floor(Math.random() * 100000); seedField.text = String(sec.seed) } }
        Label { height: 22; text: "Variety" }
        Field { id: varietyField; width: 40; text: "1.0" }
    }
    Row {
        spacing: 4
        Label { width: 52; height: 22; text: "Skin" }
        ThemedComboBox { id: skinBox; width: sec.width - 60; height: 22; font.pixelSize: 11; model: ["(default)"].concat(CharacterController.skinNames) }
    }
    Row {
        spacing: 4
        Label { width: 52; height: 22; text: "Eyes" }
        ThemedComboBox { id: eyeBox; width: sec.width - 60; height: 22; font.pixelSize: 11; model: ["(default)"].concat(CharacterController.eyeNames) }
    }
    Row {
        spacing: 4
        Label { width: 52; height: 22; text: "Body" }
        Field { id: bodyField; width: sec.width - 60; placeholderText: "0=1.2,3=-0.5 (45 modes)" }
    }
    Row {
        spacing: 4
        Label { width: 52; height: 22; text: "Face" }
        Field { id: faceField; width: sec.width - 60; placeholderText: "0=1,4=-0.8 (100 modes)" }
    }
    CheckBox { id: averageBox; text: "Average character (ignore seed)"; font.pixelSize: 11; palette.windowText: AnimationControlController.textColor }

    ToolBtn {
        label: CharacterController.busy ? "Working…" : "Generate character"
        accent: true
        enabled: !CharacterController.busy
        onClicked: CharacterController.generate(averageBox.checked ? -1 : sec.seed, parseFloat(varietyField.text) || 1.0,
                                                bodyField.text, faceField.text,
                                                skinBox.currentIndex > 0 ? skinBox.currentText : "",
                                                eyeBox.currentIndex > 0 ? eyeBox.currentText : "")
    }
    Note {
        visible: CharacterController.status !== ""
        text: CharacterController.status
        opacity: 1.0
    }
    Note {
        visible: !CharacterController.assetsPresent
        text: "The character data (~40 MB) downloads on first use."
    }
}
