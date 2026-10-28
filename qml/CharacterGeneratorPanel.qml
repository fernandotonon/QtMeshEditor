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
    property string customSkin: ""   // #rrggbb picked by hand; wins over the named skin
    function reseed() { coeffs = CharacterController.seedBody(averageBox.checked ? -1 : seed, parseFloat(varietyField.text) || 1.0); liveUpdate() }
    // every coefficient is sent, zeros included: the sliders are authoritative (a zero must not fall back to the seed's random value)
    function bodyString() { var parts = []; for (var i = 0; i < coeffs.length; ++i) parts.push(i + "=" + coeffs[i].toFixed(3)); return parts.join(",") }
    function pick(box, names) { return box.currentIndex > 0 ? names[box.currentIndex - 1] : "" }
    function generate(replace) {
        seed = parseInt(seedField.text) || 0   // take the seed as typed even if the field has not lost focus yet
        CharacterController.generate(averageBox.checked ? -1 : seed, parseFloat(varietyField.text) || 1.0, bodyString(), "",
                                     (customSkin !== "" ? customSkin : pick(skinBox, CharacterController.skinNames)), pick(eyeBox, CharacterController.eyeNames), replace)
    }
    // live preview: swap the character this panel made (merged into one undo step) once a slider is released
    function liveUpdate() { if (live && CharacterController.hasCharacter()) generate(true) }
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

    // a colour chip that opens the picker when clicked
    component ColourSwatch: Rectangle {
        id: chip
        property string colour: "#ffffff"
        property string tip: "Pick a colour"
        signal picked(string value)
        width: 22; height: 22; radius: 3; color: colour; border.color: AnimationControlController.borderColor
        HoverHandler { id: chipHover; cursorShape: Qt.PointingHandCursor }
        ToolTip.visible: chipHover.hovered; ToolTip.delay: 400; ToolTip.text: tip
        function choose() { var v = CharacterController.pickColour(chip.colour); if (v !== "") chip.picked(v) }
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: tip
        Accessible.onPressAction: choose()
        Keys.onReturnPressed: choose()
        Keys.onEnterPressed: choose()
        Keys.onSpacePressed: choose()
        border.width: activeFocus ? 2 : 1
        MouseArea { anchors.fill: parent; onClicked: chip.choose() }
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
        ThemedComboBox {
            id: skinBox; width: sec.width - 60 - 26; height: 22; font.pixelSize: 11; model: ["(default)"].concat(CharacterController.skinNames)
            onActivated: { sec.customSkin = ""; sec.liveUpdate() }
        }
        ColourSwatch {
            colour: sec.customSkin !== "" ? sec.customSkin : (CharacterController.skinColourHex(sec.pick(skinBox, CharacterController.skinNames)) || "#c68863")
            tip: "Pick a custom skin colour"
            onPicked: function(value) { sec.customSkin = value; sec.liveUpdate() }
        }
    }
    Row {
        spacing: 4
        Label { width: 52; height: 22; text: "Eyes" }
        ThemedComboBox { id: eyeBox; width: sec.width - 60; height: 22; font.pixelSize: 11; model: ["(default)"].concat(CharacterController.eyeNames); onCurrentIndexChanged: sec.liveUpdate() }
    }
    Row {
        visible: CharacterController.hairNames.length > 0
        spacing: 4
        Label { width: 52; height: 22; text: "Hair" }
        ThemedComboBox {
            id: hairBox
            width: sec.width - 60; height: 22; font.pixelSize: 11
            model: ["(none)"].concat(CharacterController.hairNames)
            currentIndex: Math.max(0, model.indexOf(CharacterController.hairStyle))
            onActivated: { CharacterController.hairStyle = currentIndex > 0 ? model[currentIndex] : ""; sec.liveUpdate() }
        }
    }
    Row {
        visible: CharacterController.hairNames.length > 0
        spacing: 4
        Label { width: 52; height: 22; text: "Hair colour" }
        Flow {
            width: sec.width - 60
            spacing: 4
            Repeater {
                model: CharacterController.hairColourPresets
                delegate: Rectangle {
                    id: swatch
                    required property var modelData
                    width: 20; height: 20; radius: 3
                    color: modelData.colour
                    border.width: CharacterController.hairColour === modelData.colour ? 2 : 1
                    border.color: CharacterController.hairColour === modelData.colour ? AnimationControlController.highlightColor : AnimationControlController.borderColor
                    Accessible.role: Accessible.Button
                    Accessible.name: modelData.label
                    HoverHandler { id: swatchHover }
                    ToolTip.visible: swatchHover.hovered; ToolTip.delay: 400; ToolTip.text: modelData.label
                    MouseArea { anchors.fill: parent; onClicked: { CharacterController.hairColour = swatch.modelData.colour; sec.liveUpdate() } }
                }
            }
        }
    }
    Row {
        visible: CharacterController.hairNames.length > 0
        spacing: 4
        Label { width: 52; height: 22; text: "" }
        ColourSwatch {
            colour: CharacterController.hairColour; tip: "Pick a custom hair colour"
            onPicked: function(value) { CharacterController.hairColour = value; sec.liveUpdate() }
        }
        Field {
            id: hairHex
            width: 80
            text: CharacterController.hairColour
            maximumLength: 7
            onEditingFinished: { CharacterController.hairColour = text; text = Qt.binding(function() { return CharacterController.hairColour }); sec.liveUpdate() }
        }
        Label { height: 22; text: "any #rrggbb" ; opacity: 0.6 }
    }
    // one garment slot: a style picker and a colour row (swatches + a hex field)
    component ClothesRow: Column {
        id: slot
        required property string title
        required property var styles
        required property string style
        required property string colour
        signal styleChosen(string id)
        signal colourChosen(string value)
        width: sec.width
        spacing: 4
        Row {
            spacing: 4
            Label { width: 52; height: 22; text: slot.title }
            ThemedComboBox {
                width: sec.width - 60; height: 22; font.pixelSize: 11
                model: slot.styles
                currentIndex: Math.max(0, slot.styles.indexOf(slot.style))
                onActivated: slot.styleChosen(slot.styles[currentIndex])
            }
        }
        Row {
            visible: slot.style !== "none"
            spacing: 4
            Label { width: 52; height: 22; text: "" }
            Flow {
                width: sec.width - 60
                spacing: 4
                Repeater {
                    model: CharacterController.clothColours
                    delegate: Rectangle {
                        id: chip
                        required property var modelData
                        width: 18; height: 18; radius: 3
                        color: modelData.colour
                        border.width: slot.colour === modelData.colour ? 2 : 1
                        border.color: slot.colour === modelData.colour ? AnimationControlController.highlightColor : AnimationControlController.borderColor
                        Accessible.role: Accessible.Button
                        Accessible.name: modelData.name
                        HoverHandler { id: chipHover }
                        ToolTip.visible: chipHover.hovered; ToolTip.delay: 400; ToolTip.text: modelData.name
                        MouseArea { anchors.fill: parent; onClicked: slot.colourChosen(chip.modelData.colour) }
                    }
                }
            }
        }
        Row {
            visible: slot.style !== "none"
            spacing: 4
            Label { width: 52; height: 22; text: "" }
            ColourSwatch { colour: slot.colour; tip: "Pick a custom colour"; onPicked: function(value) { slot.colourChosen(value) } }
            Field {
                width: 80
                text: slot.colour
                maximumLength: 7
                onEditingFinished: { slot.colourChosen(text); text = Qt.binding(function() { return slot.colour }) }
            }
            Label { height: 22; text: "any #rrggbb"; opacity: 0.6 }
        }
    }
    ClothesRow {
        title: "Top"
        styles: CharacterController.topStyles
        style: CharacterController.topStyle
        colour: CharacterController.topColour
        onStyleChosen: function(id) { CharacterController.topStyle = id; sec.liveUpdate() }
        onColourChosen: function(value) { CharacterController.topColour = value; sec.liveUpdate() }
    }
    ClothesRow {
        title: "Bottom"
        styles: CharacterController.bottomStyles
        style: CharacterController.bottomStyle
        colour: CharacterController.bottomColour
        onStyleChosen: function(id) { CharacterController.bottomStyle = id; sec.liveUpdate() }
        onColourChosen: function(value) { CharacterController.bottomColour = value; sec.liveUpdate() }
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
    InspectorCheckBox { text: "Skin texture (procedural)"; checked: CharacterController.skinTexture; onToggled: { CharacterController.skinTexture = checked; sec.liveUpdate() } }
    Row {
        spacing: 6
        Label { width: 118; height: 22; text: "Body hair" }
        Slider {
            id: hairSlider
            width: sec.width - 118 - 36 - 12
            height: 22
            from: 0; to: 1; stepSize: 0.05
            value: CharacterController.bodyHair
            onMoved: CharacterController.bodyHair = value
            onPressedChanged: if (!pressed) sec.liveUpdate()
        }
        Label { width: 32; height: 22; horizontalAlignment: Text.AlignRight; text: hairSlider.value.toFixed(2) }
    }
    InspectorCheckBox { text: "Eyelashes"; checked: CharacterController.eyelashes; enabled: CharacterController.openEyes; onToggled: { CharacterController.eyelashes = checked; sec.liveUpdate() } }
    InspectorCheckBox { text: "Open eyes"; checked: CharacterController.openEyes; onToggled: { CharacterController.openEyes = checked; sec.liveUpdate() } }
    InspectorCheckBox { text: "Live preview (edits the character in place)"; checked: sec.live; onToggled: sec.live = checked }
    InspectorCheckBox { id: averageBox; text: "Average character (ignore seed)"; onToggled: sec.reseed() }

    ToolBtn {
        label: CharacterController.busy ? "Working…" : "Generate character"
        accent: true
        enabled: !CharacterController.busy
        onClicked: sec.generate(false)
    }
    // First-use asset download (~40 MB across body/head/licences/hair). Bound to
    // CharacterController so the nested ModelFetch event loop can paint it.
    Column {
        width: sec.width
        spacing: 2
        visible: CharacterController.downloadProgress >= 0
        Note {
            text: {
                var label = CharacterController.downloadLabel || "character assets"
                var r = CharacterController.downloadBytesReceived
                var t = CharacterController.downloadBytesTotal
                if (t > 0)
                    return "Downloading " + label + " — " + Math.round(r / 1048576) + " / " + Math.round(t / 1048576) + " MB"
                return "Downloading " + label + "…"
            }
            opacity: 1.0
        }
        Rectangle {
            width: sec.width; height: 8; radius: 3
            color: AnimationControlController.inputColor
            border.color: AnimationControlController.borderColor; border.width: 1
            Rectangle {
                anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                anchors.margins: 1
                width: Math.max(0, (parent.width - 2) * Math.min(1, Math.max(0, CharacterController.downloadProgress)))
                radius: 2
                color: AnimationControlController.highlightColor
            }
        }
    }
    Note {
        visible: CharacterController.status !== "" && CharacterController.downloadProgress < 0
        text: CharacterController.status
        opacity: 1.0
    }
    Note {
        visible: !CharacterController.assetsPresent && CharacterController.downloadProgress < 0
        text: "The character data (~40 MB) downloads on first use."
    }
}
