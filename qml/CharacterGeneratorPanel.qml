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
    // slider rows: the MHR body-shape modes worth exposing (0-9 shape the torso, 20-27 the head and neck)
    readonly property var bodyModes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
    readonly property var headModes: [20, 21, 22, 23, 24, 25, 26, 27]
    property var coeffs: new Array(45).fill(0)
    property bool live: true
    function reseed() { coeffs = CharacterController.seedBody(averageBox.checked ? -1 : seed, parseFloat(varietyField.text) || 1.0); liveUpdate() }
    function bodyString() { var parts = []; for (var i = 0; i < coeffs.length; ++i) if (coeffs[i] !== 0) parts.push(i + "=" + coeffs[i].toFixed(3)); return parts.join(",") }
    function pick(box, names) { return box.currentIndex > 0 ? names[box.currentIndex - 1] : "" }
    function generate(replace) {
        CharacterController.generate(averageBox.checked ? -1 : seed, parseFloat(varietyField.text) || 1.0, bodyString(), "",
                                     pick(skinBox, CharacterController.skinNames), pick(eyeBox, CharacterController.eyeNames), replace)
    }
    // live preview: swap the character this panel made (merged into one undo step) once a slider is released
    function liveUpdate() { if (live && CharacterController.dnaPath === "" && CharacterController.hasCharacter()) generate(true) }
    Component.onCompleted: reseed()

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
    component InspectorCheckBox: CheckBox {
        id: icb
        spacing: 6
        indicator: Rectangle {
            x: icb.leftPadding
            y: icb.height / 2 - height / 2
            implicitWidth: 16; implicitHeight: 16; radius: 2
            color: icb.checked ? AnimationControlController.highlightColor : AnimationControlController.inputColor
            border.color: icb.activeFocus ? AnimationControlController.highlightColor : AnimationControlController.borderColor
            opacity: icb.enabled ? 1.0 : 0.45
            Text {
                anchors.centerIn: parent; visible: icb.checked; text: "✓"
                color: AnimationControlController.textColor; font.pixelSize: 12; font.bold: true
            }
        }
        contentItem: Text {
            visible: icb.text !== ""
            text: icb.text
            color: AnimationControlController.textColor
            font.pixelSize: 11
            leftPadding: icb.indicator.width + icb.spacing
            verticalAlignment: Text.AlignVCenter
            opacity: icb.enabled ? 1.0 : 0.45
        }
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
        Field { id: seedField; width: 60; text: String(sec.seed); inputMethodHints: Qt.ImhDigitsOnly; onEditingFinished: { sec.seed = parseInt(text) || 0; sec.reseed() } }
        ToolBtn { label: "Random"; onClicked: { sec.seed = Math.floor(Math.random() * 100000); seedField.text = String(sec.seed); sec.reseed() } }
        Label { height: 22; text: "Variety" }
        Field { id: varietyField; width: 40; text: "1.0"; onEditingFinished: sec.reseed() }
    }
    Row {
        spacing: 4
        Label { width: 52; height: 22; text: "Skin" }
        ThemedComboBox { id: skinBox; width: sec.width - 60; height: 22; font.pixelSize: 11; model: ["(default)"].concat(CharacterController.skinNames); onCurrentIndexChanged: sec.liveUpdate() }
    }
    Row {
        spacing: 4
        Label { width: 52; height: 22; text: "Eyes" }
        ThemedComboBox { id: eyeBox; width: sec.width - 60; height: 22; font.pixelSize: 11; model: ["(default)"].concat(CharacterController.eyeNames); onCurrentIndexChanged: sec.liveUpdate() }
    }
    component ShapeSlider: Row {
        id: row
        required property int mode
        spacing: 6
        Label {
            width: 118; height: 22; elide: Text.ElideRight
            text: CharacterController.shapeLabel(row.mode)
            HoverHandler { id: hh }
            ToolTip.visible: hh.hovered; ToolTip.delay: 500; ToolTip.text: CharacterController.shapeHint(row.mode)
        }
        Slider {
            id: sl
            width: sec.width - 118 - 36 - 12
            height: 22
            from: -3; to: 3; stepSize: 0.1
            value: sec.coeffs[row.mode]
            onMoved: { var v = sec.coeffs.slice(); v[row.mode] = value; sec.coeffs = v }
            onPressedChanged: if (!pressed) sec.liveUpdate()
        }
        Label { width: 32; height: 22; horizontalAlignment: Text.AlignRight; text: sl.value.toFixed(1) }
    }
    Label { height: 18; font.bold: true; text: "Body" }
    Repeater { model: sec.bodyModes; delegate: ShapeSlider { required property int modelData; mode: modelData } }
    Label { height: 18; font.bold: true; text: "Head and neck" }
    Repeater { model: sec.headModes; delegate: ShapeSlider { required property int modelData; mode: modelData } }
    Row {
        spacing: 4
        ToolBtn { label: "Reset shape"; onClicked: { sec.coeffs = new Array(45).fill(0); sec.liveUpdate() } }
        ToolBtn { label: "From seed"; onClicked: sec.reseed() }
    }
    Row {
        spacing: 4
        Label { width: 52; height: 22; text: "DNA" }
        Field { id: dnaField; width: sec.width - 60 - 66; text: CharacterController.dnaPath; placeholderText: "optional MetaHuman body .dna"; onEditingFinished: CharacterController.dnaPath = text }
        ToolBtn { label: "Browse…"; onClicked: CharacterController.browseDna() }
    }
    Note { text: "With a MetaHuman body DNA you own (not shipped with the editor), the generated shape is fitted onto it: you get its rig, skin and correctives." }
    InspectorCheckBox { text: "Open eyes"; checked: CharacterController.openEyes; onToggled: { CharacterController.openEyes = checked; sec.liveUpdate() } }
    InspectorCheckBox { text: "Live preview (edits the character in place)"; checked: sec.live; onToggled: sec.live = checked }
    InspectorCheckBox { id: averageBox; text: "Average character (ignore seed)"; onToggled: sec.reseed() }

    ToolBtn {
        label: CharacterController.busy ? "Working…" : "Generate character"
        accent: true
        enabled: !CharacterController.busy
        onClicked: sec.generate(false)
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
