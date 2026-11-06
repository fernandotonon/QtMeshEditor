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
    // the raw MHR identity modes (Advanced): 1-19 shape the torso and hips (mode 0 is the Weight slider), 20-39 the head and neck.
    // Names and tooltips come from measuring each mode (CharacterController.shapeLabel / shapeHint).
    readonly property var bodyModes: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19]
    readonly property var headModes: [20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39]
    property bool showAdvancedShape: false
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
    HairStudio {}
    Row {
        visible: CharacterController.hairNames.length > 0 || CharacterController.hairStyle !== ""
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
        visible: CharacterController.hairNames.length > 0 || CharacterController.hairStyle !== ""
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
    // ── Hair Studio ─────────────────────────────────────────────────────────
    // One browser for the built-in, procedural and imported hairstyles (filterable), procedural controls (cards generated natively
    // from guide curves) and the manual fit of an imported style. Every change rebuilds the character in place when live preview is on.
    component FilterBox: ThemedComboBox {
        property string current: "all"
        property real cellWidth: 80
        width: cellWidth; height: 22; font.pixelSize: 11
        onActivated: current = model[currentIndex]
    }
    component HairStudio: Column {
        id: studio
        width: sec.width
        spacing: 4
        property bool open: false
        property string kindFilter: "all"
        property string lengthFilter: "all"
        property string textureFilter: "all"
        property string genderFilter: "all"
        property string styleFilter: ""
        property var tiles: []
        function refresh() { tiles = CharacterController.hairBrowser(kindFilter, lengthFilter, textureFilter, genderFilter, styleFilter) }
        onOpenChanged: if (open) refresh()
        onKindFilterChanged: refresh()
        onLengthFilterChanged: refresh()
        onTextureFilterChanged: refresh()
        onGenderFilterChanged: refresh()
        onStyleFilterChanged: refresh()
        Connections {
            target: CharacterController
            function onHairLibraryChanged() { studio.refresh() }
            function onHairStyleChanged() { studio.refresh() }
            function onHairNamesChanged() { studio.refresh() }
        }
        ToolBtn { label: (studio.open ? "▾ " : "▸ ") + "Hair Studio  (" + CharacterController.hairSource + ")"; onClicked: studio.open = !studio.open }
        Column {
            visible: studio.open
            width: studio.width
            spacing: 4
            Row {
                spacing: 4
                FilterBox { cellWidth: (studio.width - 8) / 3; model: ["all", "builtin", "procedural", "imported"]; onCurrentChanged: studio.kindFilter = current }
                FilterBox { cellWidth: (studio.width - 8) / 3; model: ["all", "short", "medium", "long"]; onCurrentChanged: studio.lengthFilter = current }
                FilterBox { cellWidth: (studio.width - 8) / 3; model: ["all", "straight", "wavy", "curly", "coily"]; onCurrentChanged: studio.textureFilter = current }
            }
            Row {
                spacing: 4
                FilterBox { cellWidth: (studio.width - 8) / 3; model: ["all", "feminine", "masculine", "unisex"]; onCurrentChanged: studio.genderFilter = current }
                Field { width: studio.width - (studio.width - 8) / 3 - 4; text: ""; placeholderText: "style: bob, ponytail, bun, afro, fade…"; onTextChanged: studio.styleFilter = text }
            }
            Flow {
                width: studio.width
                spacing: 4
                Repeater {
                    model: studio.tiles
                    delegate: Rectangle {
                        id: tile
                        required property var modelData
                        width: 66; height: 74; radius: 4
                        color: AnimationControlController.buttonColor
                        border.width: modelData.current ? 2 : 1
                        border.color: modelData.current ? AnimationControlController.highlightColor : AnimationControlController.borderColor
                        Accessible.role: Accessible.Button
                        Accessible.name: modelData.name
                        activeFocusOnTab: true
                        function select() { CharacterController.selectHair(tile.modelData.key); sec.liveUpdate() }
                        Accessible.onPressAction: select()
                        Keys.onReturnPressed: select()
                        Keys.onEnterPressed: select()
                        Keys.onSpacePressed: select()
                        HoverHandler { id: tileHover }
                        ToolTip.visible: tileHover.hovered; ToolTip.delay: 300
                        ToolTip.text: modelData.name + " · " + modelData.length + " " + modelData.texture + " " + modelData.gender
                                      + (modelData.license ? "\nLicence: " + modelData.license : "") + (modelData.author ? "\nBy: " + modelData.author : "")
                        Image { visible: modelData.thumbnail !== ""; anchors { top: parent.top; horizontalCenter: parent.horizontalCenter; topMargin: 3 } width: 54; height: 48; fillMode: Image.PreserveAspectFit; source: modelData.thumbnail }
                        Text {
                            visible: modelData.thumbnail === ""
                            anchors { top: parent.top; horizontalCenter: parent.horizontalCenter; topMargin: 8 }
                            text: modelData.kind === "procedural" ? "⌇⌇⌇" : "✂"
                            font.pixelSize: 22; color: AnimationControlController.textColor; opacity: 0.6
                        }
                        Text { anchors { bottom: parent.bottom; left: parent.left; right: parent.right; margins: 2 } height: 22; text: modelData.name; elide: Text.ElideRight; wrapMode: Text.WordWrap; maximumLineCount: 2
                               horizontalAlignment: Text.AlignHCenter; font.pixelSize: 9; color: AnimationControlController.textColor }
                        MouseArea { anchors.fill: parent; onClicked: tile.select() }
                    }
                }
            }
            Note { visible: studio.tiles.length === 0; text: "No hairstyle matches these filters." }
            Row {
                spacing: 4
                ToolBtn { label: "No hair"; onClicked: { CharacterController.clearHair(); sec.liveUpdate() } }
                ToolBtn { label: "Duplicate"; enabled: CharacterController.hairSource === "procedural" || CharacterController.hairSource === "library"; onClicked: { CharacterController.duplicateHair(dupName.text.length > 0 ? dupName.text : "My hairstyle") ; sec.liveUpdate() } }
                Field { id: dupName; width: studio.width - 150; placeholderText: "name (duplicate / save preset)" }
            }

            // ── procedural controls
            Column {
                visible: CharacterController.hairSource === "procedural"
                width: studio.width
                spacing: 2
                Label { text: "Procedural hair cards"; font.bold: true }
                Repeater {
                    model: CharacterController.hairParamSpecs
                    delegate: Row {
                        id: prow
                        required property var modelData
                        spacing: 4
                        Label { width: 92; height: 22; text: prow.modelData.label }
                        Slider {
                            id: pslider
                            width: studio.width - 92 - 44 - 12
                            height: 22
                            from: prow.modelData.min; to: prow.modelData.max; stepSize: prow.modelData.step
                            value: CharacterController.hairParams[prow.modelData.key] === undefined ? prow.modelData.min : CharacterController.hairParams[prow.modelData.key]
                            onMoved: CharacterController.setHairParam(prow.modelData.key, value)
                            onPressedChanged: if (!pressed) sec.liveUpdate()
                        }
                        Label { width: 40; height: 22; horizontalAlignment: Text.AlignRight; text: pslider.value.toFixed(prow.modelData.step >= 1 ? 0 : 3) }
                    }
                }
                InspectorCheckBox {
                    text: "Symmetric guides"
                    checked: CharacterController.hairParams["symmetric"] === 1
                    onToggled: { CharacterController.setHairParam("symmetric", checked ? 1 : 0); sec.liveUpdate() }
                }
                Row {
                    spacing: 4
                    ToolBtn { label: "Reset"; onClicked: { CharacterController.resetHairParams(); sec.liveUpdate() } }
                    ToolBtn { label: "Save as preset"; onClicked: CharacterController.saveHairPreset(dupName.text.length > 0 ? dupName.text : "My hairstyle") }
                }
                Flow {
                    width: studio.width
                    spacing: 4
                    Repeater {
                        model: CharacterController.savedHairPresets
                        delegate: ToolBtn { required property string modelData; label: "✕ " + modelData; onClicked: CharacterController.deleteHairPreset(modelData) }
                    }
                }
            }

            // ── imported hair: fit + attribution
            Column {
                visible: CharacterController.hairSource === "library"
                width: studio.width
                spacing: 2
                Label { text: "Fit to this head"; font.bold: true }
                Repeater {
                    model: [{k: "tx", l: "Move X (m)", a: -0.1, b: 0.1}, {k: "ty", l: "Move Y (m)", a: -0.1, b: 0.1}, {k: "tz", l: "Move Z (m)", a: -0.1, b: 0.1},
                            {k: "rx", l: "Pitch", a: -45, b: 45}, {k: "ry", l: "Yaw", a: -180, b: 180}, {k: "rz", l: "Roll", a: -45, b: 45},
                            {k: "scale", l: "Scale", a: 0.5, b: 2}, {k: "snug", l: "Snug to scalp", a: 0, b: 1}, {k: "clearance", l: "Clearance (m)", a: 0, b: 0.02}]
                    delegate: Row {
                        id: frow
                        required property var modelData
                        spacing: 4
                        Label { width: 92; height: 22; text: frow.modelData.l }
                        Slider {
                            id: fslider
                            width: studio.width - 92 - 44 - 12
                            height: 22
                            from: frow.modelData.a; to: frow.modelData.b
                            value: CharacterController.hairFitParams[frow.modelData.k] === undefined ? 0 : CharacterController.hairFitParams[frow.modelData.k]
                            onMoved: CharacterController.setHairFitParam(frow.modelData.k, value)
                            onPressedChanged: if (!pressed) sec.liveUpdate()
                        }
                        Label { width: 40; height: 22; horizontalAlignment: Text.AlignRight; text: fslider.value.toFixed(3) }
                    }
                }
                InspectorCheckBox { text: "Source faces the other way (turn 180°)"; checked: CharacterController.hairFitParams["flipFront"] === true; onToggled: { CharacterController.setHairFitParam("flip", checked ? 1 : 0); sec.liveUpdate() } }
                Row {
                    spacing: 4
                    ToolBtn { label: "Reset fit"; onClicked: { CharacterController.resetHairFit(); sec.liveUpdate() } }
                    ToolBtn { label: "Remove from library"; onClicked: { CharacterController.removeLibraryHair(CharacterController.hairStyle.substring(4)); sec.liveUpdate() } }
                }
            }

            // ── import
            Label { text: "Import hairstyles"; font.bold: true }
            Field { id: impPath; width: studio.width; placeholderText: "model file or folder (FBX, OBJ, GLB, DAE…) — paste the path" }
            Row {
                spacing: 4
                Field { id: impAuthor; width: (studio.width - 8) / 2; placeholderText: "author" }
                Field { id: impLicense; width: (studio.width - 8) / 2; placeholderText: "licence (e.g. CC-BY-4.0)" }
            }
            Field { id: impAttribution; width: studio.width; placeholderText: "attribution / source URL" }
            ToolBtn { label: "Import into library"; enabled: impPath.text.length > 0; onClicked: CharacterController.importHair(impPath.text, impAuthor.text, impLicense.text, impAttribution.text) }
            Note { visible: CharacterController.hairStudioStatus !== ""; text: CharacterController.hairStudioStatus }
            Note { text: "Imported styles are fitted to each head automatically (scale, scalp clearance); nothing is bundled or downloaded — enter the licence of what you import." }
        }
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
    component PropSlider: Row {
        id: prow
        required property int idx
        spacing: 6
        Label {
            width: 118; height: 22; elide: Text.ElideRight
            text: CharacterController.proportionLabel(prow.idx)
            HoverHandler { id: phh }
            ToolTip.visible: phh.hovered; ToolTip.delay: 500; ToolTip.text: CharacterController.proportionHint(prow.idx)
        }
        Slider {
            id: psl
            width: sec.width - 118 - 44 - 12
            height: 22
            from: 0.8; to: 1.2; stepSize: 0.01
            value: CharacterController.proportions[prow.idx]
            onMoved: CharacterController.setProportion(prow.idx, value)
            onPressedChanged: if (!pressed) sec.liveUpdate()
        }
        Label { width: 40; height: 22; horizontalAlignment: Text.AlignRight; text: Math.round(psl.value * 100) + "%" }
    }
    component MeasureSlider: Row {
        id: mrow
        required property int idx
        spacing: 6
        Label {
            width: 118; height: 22; elide: Text.ElideRight
            text: CharacterController.measureLabel(mrow.idx)
            HoverHandler { id: mhh }
            ToolTip.visible: mhh.hovered; ToolTip.delay: 500; ToolTip.text: CharacterController.measureHint(mrow.idx)
        }
        Slider {
            id: msl
            width: sec.width - 118 - 52 - 12
            height: 22
            from: -CharacterController.measureRange(mrow.idx); to: CharacterController.measureRange(mrow.idx); stepSize: 0.1
            value: CharacterController.measureOffset(mrow.idx, sec.coeffs)
            onMoved: sec.coeffs = CharacterController.coefficientsWithMeasure(mrow.idx, value, sec.coeffs)
            onPressedChanged: if (!pressed) sec.liveUpdate()
        }
        Label { width: 48; height: 22; horizontalAlignment: Text.AlignRight; text: (msl.value >= 0 ? "+" : "") + msl.value.toFixed(1) + " cm" }
    }
    Label { height: 18; font.bold: true; text: "Proportions" }
    Repeater { model: CharacterController.proportionCount(); delegate: PropSlider { required property int modelData; idx: modelData } }
    Label { height: 18; font.bold: true; text: "Body shape" }
    ShapeSlider { mode: 0 }
    Repeater { model: CharacterController.measureCount(); delegate: MeasureSlider { required property int modelData; idx: modelData } }
    CheckBox { height: 22; text: "Advanced: all shape modes"; checked: sec.showAdvancedShape; onToggled: sec.showAdvancedShape = checked }
    Repeater { model: sec.showAdvancedShape ? sec.bodyModes : []; delegate: ShapeSlider { required property int modelData; mode: modelData } }
    Repeater { model: sec.showAdvancedShape ? sec.headModes : []; delegate: ShapeSlider { required property int modelData; mode: modelData } }
    Row {
        spacing: 4
        ToolBtn { label: "Reset shape"; onClicked: { sec.coeffs = new Array(45).fill(0); CharacterController.resetProportions(); sec.liveUpdate() } }
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
