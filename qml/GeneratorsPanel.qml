pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import AnimationControl 1.0
import PropertiesPanel 1.0

// ── Procedural generators (#524) ─────────────────────────────────────────────
// Fill an animatable property from a formula: Sine (hover, breathing), Noise
// (camera shake, sway), Ramp, Spring (chase a value) and Follow path (travel
// along a curve edited in the viewport). Every generator ADDS to the target's
// base animation (follow-path replaces the position). Bone / node / morph
// targets are written into the real track; pose / light / material targets
// are driven every frame. Bake turns a generator into ordinary keyframes.
// Every action is one undo step (AnimGeneratorManager pushes the command).
Column {
    id: genSection
    width: parent ? parent.width : 300
    spacing: 6

    property string selectedId: ""
    property var details: ({})
    property int refreshTick: 0

    function refreshDetails() {
        details = selectedId !== "" ? AnimGeneratorManager.details(selectedId) : ({})
    }
    // Selecting a follow-path generator shows its path in the viewport right
    // away (points draggable with the Select tool); selecting anything else
    // hides it.
    onSelectedIdChanged: {
        refreshDetails()
        if (details.type === "follow-path") AnimGeneratorManager.beginPathEdit(selectedId)
        else if (AnimGeneratorManager.pathEditId !== "") AnimGeneratorManager.endPathEdit()
    }
    Connections {
        target: AnimGeneratorManager
        function onGeneratorsChanged() {
            var rows = AnimGeneratorManager.generators
            var still = false
            for (var i = 0; i < rows.length; ++i) if (rows[i].id === genSection.selectedId) still = true
            if (!still) genSection.selectedId = ""
            genSection.refreshDetails()
        }
    }

    component ToolBtn: Rectangle {
        property string label: ""
        property bool enabled: true
        property bool accent: false
        property string tip: ""
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
        ToolTip.visible: tip !== "" && maT.containsMouse
        ToolTip.text: tip
        ToolTip.delay: 600
        Text {
            id: lblT; anchors.centerIn: parent; text: parent.label
            color: AnimationControlController.buttonTextColor; font.pixelSize: 11
        }
        MouseArea {
            id: maT; anchors.fill: parent; hoverEnabled: true
            enabled: parent.enabled; onClicked: parent.clicked()
        }
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

    component Label: Text {
        color: AnimationControlController.textColor
        font.pixelSize: 11
        verticalAlignment: Text.AlignVCenter
    }

    // A numeric/text parameter field that commits on Enter or focus loss.
    component ParamField: Row {
        id: pf
        property string key: ""
        property string label: key
        property var value: undefined
        spacing: 4
        Label { width: 92; height: 22; text: pf.label }
        Rectangle {
            width: genSection.width - 120; height: 22; radius: 2
            color: AnimationControlController.inputColor
            border.color: pfIn.activeFocus ? AnimationControlController.highlightColor : AnimationControlController.borderColor
            TextInput {
                id: pfIn
                anchors.fill: parent; anchors.margins: 4
                color: AnimationControlController.textColor
                font.pixelSize: 11
                verticalAlignment: TextInput.AlignVCenter
                selectByMouse: true; clip: true
                text: pf.value === undefined ? "" : String(pf.value)
                function commit() {
                    if (pf.value !== undefined && text === String(pf.value)) return
                    AnimGeneratorManager.setParamFromUi(genSection.selectedId, pf.key, text)
                }
                onAccepted: commit()
                onActiveFocusChanged: if (!activeFocus) commit()
            }
        }
    }

    Text {
        width: parent.width - 8
        wrapMode: Text.Wrap
        opacity: 0.8
        color: AnimationControlController.textColor
        font.pixelSize: 10
        text: "Drive a property from a formula — sine, noise, ramp, spring or a path. "
            + "Generators add to the existing animation; Bake turns one into keyframes."
    }

    // ---- Add ----
    Column {
        id: addCol
        width: parent.width
        spacing: 4

        property string kind: kindBox.currentText
        property string type: AnimGeneratorManager.typeIds[typeBox.currentIndex] || "sine"
        property bool needsSub: kind === "bone" || kind === "morph" || kind === "pose"
        property bool hasClip: kind === "bone" || kind === "node" || kind === "morph"

        Row {
            spacing: 4
            Label { width: 52; height: 22; text: "Type" }
            ThemedComboBox {
                id: typeBox
                width: genSection.width - 60; height: 22; font.pixelSize: 11
                model: AnimGeneratorManager.typeIds.map(function(t) { return AnimGeneratorManager.typeLabel(t) })
            }
        }
        Row {
            spacing: 4
            Label { width: 52; height: 22; text: "Target" }
            ThemedComboBox {
                id: kindBox
                width: 86; height: 22; font.pixelSize: 11
                model: addCol.type === "follow-path" ? ["node", "bone"] : AnimGeneratorManager.kindIds
            }
            ThemedComboBox {
                id: objectBox
                width: genSection.width - 154; height: 22; font.pixelSize: 11
                model: { genSection.refreshTick; return AnimGeneratorManager.objectsFor(addCol.kind) }
            }
        }
        Row {
            visible: addCol.needsSub
            spacing: 4
            Label {
                width: 52; height: 22
                text: addCol.kind === "bone" ? "Bone" : addCol.kind === "morph" ? "Shape" : "Pose"
            }
            ThemedComboBox {
                id: subBox
                width: genSection.width - 60; height: 22; font.pixelSize: 11
                model: { genSection.refreshTick; return AnimGeneratorManager.subsFor(addCol.kind, objectBox.currentText) }
            }
        }
        Row {
            spacing: 4
            Label { width: 52; height: 22; text: "Channel" }
            ThemedComboBox {
                id: channelBox
                width: addCol.hasClip ? (genSection.width - 60) / 2 - 2 : genSection.width - 60
                height: 22; font.pixelSize: 11
                model: AnimGeneratorManager.channelsFor(addCol.kind, addCol.type)
            }
            ThemedComboBox {
                id: clipBox
                visible: addCol.hasClip
                width: (genSection.width - 60) / 2 - 2; height: 22; font.pixelSize: 11
                model: { genSection.refreshTick; return AnimGeneratorManager.clipsFor(addCol.kind, objectBox.currentText) }
            }
        }
        Row {
            spacing: 4
            ToolBtn {
                label: "Add generator"
                enabled: objectBox.currentText !== "" && channelBox.currentText !== ""
                         && (!addCol.needsSub || subBox.currentText !== "")
                tip: "Adds the generator live; it is one undo step"
                onClicked: {
                    var t = addCol.kind + ":" + objectBox.currentText
                    if (addCol.needsSub) t += "/" + subBox.currentText
                    t += "/" + channelBox.currentText
                    if (addCol.hasClip && clipBox.currentText !== "") t += "@" + clipBox.currentText
                    if (AnimGeneratorManager.addFromUi(addCol.type, t, {})) {
                        var rows = AnimGeneratorManager.generators
                        if (rows.length > 0) genSection.selectedId = rows[rows.length - 1].id
                    }
                }
            }
            ToolBtn {
                label: "↻"
                tip: "Refresh the object lists"
                onClicked: genSection.refreshTick++
            }
        }
    }

    Rectangle { width: parent.width - 8; height: 1; color: AnimationControlController.borderColor; opacity: 0.6 }

    // ---- List ----
    Text {
        visible: AnimGeneratorManager.count === 0
        color: AnimationControlController.disabledTextColor
        font.pixelSize: 10
        text: "No generators yet."
    }
    Repeater {
        model: AnimGeneratorManager.generators
        delegate: Rectangle {
            id: rowRect
            required property var modelData
            width: genSection.width - 8
            height: 26
            radius: 3
            color: modelData.id === genSection.selectedId
                ? Qt.rgba(AnimationControlController.highlightColor.r, AnimationControlController.highlightColor.g,
                          AnimationControlController.highlightColor.b, 0.25)
                : "transparent"
            border.color: AnimationControlController.borderColor
            border.width: modelData.id === genSection.selectedId ? 1 : 0
            Row {
                anchors.fill: parent; anchors.leftMargin: 2; spacing: 4
                InspectorCheckBox {
                    anchors.verticalCenter: parent.verticalCenter
                    checked: rowRect.modelData.enabled
                    ToolTip.visible: hovered
                    ToolTip.text: rowRect.modelData.baked
                        ? "Baked. Check to un-bake: the track goes back to how it was before the bake, so you can change it and bake again"
                        : "Live (unchecked = muted: the base animation plays alone)"
                    onToggled: AnimGeneratorManager.setEnabledFromUi(rowRect.modelData.id, checked)
                }
                Text {
                    // "Sine · position.y · Generators" reads at a glance; the
                    // full target is in the tooltip.
                    function shortTarget(t) {
                        var at = t.lastIndexOf("@")
                        var clip = at >= 0 ? t.substring(at + 1) : ""
                        var path = at >= 0 ? t.substring(0, at) : t
                        var channel = path.substring(path.lastIndexOf("/") + 1)
                        return channel + (clip !== "" ? " · " + clip : "")
                    }
                    width: genSection.width - 120
                    anchors.verticalCenter: parent.verticalCenter
                    elide: Text.ElideRight
                    color: rowRect.modelData.bound ? AnimationControlController.textColor : "#e08060"
                    font.pixelSize: 11
                    text: rowRect.modelData.typeLabel + " · " + shortTarget(rowRect.modelData.target)
                          + (rowRect.modelData.baked ? "  [baked]" : "")
                          + (rowRect.modelData.bound ? "" : "  [target missing]")
                    MouseArea {
                        id: rowMa
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: genSection.selectedId = rowRect.modelData.id
                    }
                    ToolTip.visible: rowMa.containsMouse
                    ToolTip.text: rowRect.modelData.target
                    ToolTip.delay: 500
                }
                ToolBtn {
                    anchors.verticalCenter: parent.verticalCenter
                    label: "Bake"; enabled: !rowRect.modelData.baked
                    tip: "Turn the generator into keyframes. Editing it afterwards un-bakes it, so you can change it and bake again"
                    onClicked: AnimGeneratorManager.bakeFromUi(rowRect.modelData.id)
                }
                ToolBtn {
                    anchors.verticalCenter: parent.verticalCenter
                    label: "✕"; tip: "Remove the generator"
                    onClicked: AnimGeneratorManager.removeFromUi(rowRect.modelData.id)
                }
            }
        }
    }

    // ---- Parameters of the selected generator ----
    Column {
        visible: genSection.selectedId !== "" && genSection.details.type !== undefined
        width: parent.width
        spacing: 3
        property string t: genSection.details.type || ""

        Text {
            color: AnimationControlController.textColor; font.pixelSize: 11; font.bold: true
            text: (genSection.details.typeLabel || "") + " parameters"
        }
        ParamField { key: "start"; label: "Start (s)"; value: genSection.details.start }
        ParamField { key: "duration"; label: "Duration (s)"; value: genSection.details.duration }
        ParamField { key: "fps"; label: "Bake fps"; value: genSection.details.fps }
        ParamField { visible: parent.t !== "follow-path"; key: "offset"; label: "Offset"; value: genSection.details.offset }
        ParamField { visible: parent.t === "sine" || parent.t === "noise"; key: "amplitude"; label: "Amplitude"; value: genSection.details.amplitude }
        ParamField { visible: parent.t === "sine"; key: "frequency"; label: "Frequency (Hz)"; value: genSection.details.frequency }
        ParamField { visible: parent.t === "sine"; key: "phase"; label: "Phase (°)"; value: genSection.details.phase }
        ParamField { visible: parent.t === "noise"; key: "noise_frequency"; label: "Detail (per s)"; value: genSection.details.noise_frequency }
        ParamField { visible: parent.t === "noise"; key: "octaves"; label: "Octaves"; value: genSection.details.octaves }
        ParamField { visible: parent.t === "noise"; key: "seed"; label: "Seed"; value: genSection.details.seed }
        ParamField { visible: parent.t === "ramp" || parent.t === "spring"; key: "from"; label: "From"; value: genSection.details.from }
        ParamField { visible: parent.t === "ramp" || parent.t === "spring"; key: "to"; label: "To"; value: genSection.details.to }
        ParamField { visible: parent.t === "spring"; key: "stiffness"; label: "Stiffness (rad/s)"; value: genSection.details.stiffness }
        ParamField { visible: parent.t === "spring"; key: "damping"; label: "Damping ratio"; value: genSection.details.damping }
        Row {
            visible: parent.t === "ramp"
            spacing: 4
            Label { width: 92; height: 22; text: "Ease" }
            ThemedComboBox {
                width: 120; height: 22; font.pixelSize: 11
                model: ["linear", "smooth"]
                currentIndex: genSection.details.ease === "smooth" ? 1 : 0
                onActivated: function(i) { AnimGeneratorManager.setParamFromUi(genSection.selectedId, "ease", i === 1 ? "smooth" : "linear") }
            }
        }

        // Follow path
        Column {
            visible: parent.t === "follow-path"
            width: parent.width
            spacing: 3
            ParamField { key: "loops"; label: "Loops"; value: genSection.details.loops }
            Row {
                spacing: 8
                InspectorCheckBox {
                    text: "Closed"; checked: genSection.details.closed === true
                    onToggled: AnimGeneratorManager.setParamFromUi(genSection.selectedId, "closed", checked ? "true" : "false")
                }
                InspectorCheckBox {
                    text: "Even speed"; checked: genSection.details.constant_speed !== false
                    onToggled: AnimGeneratorManager.setParamFromUi(genSection.selectedId, "constant_speed", checked ? "true" : "false")
                }
                InspectorCheckBox {
                    text: "Face path"; checked: genSection.details.orient === true
                    onToggled: AnimGeneratorManager.setParamFromUi(genSection.selectedId, "orient", checked ? "true" : "false")
                }
            }
            Text {
                width: genSection.width - 8
                wrapMode: Text.Wrap
                font.pixelSize: 10
                opacity: 0.8
                color: AnimationControlController.textColor
                text: AnimGeneratorManager.pathEditId === genSection.selectedId
                      ? "The path is shown in the viewport (orange line, blue points). Drag a point with the Select tool (Q), or type coordinates below."
                      : "Show the path to see and drag its points in the viewport."
            }
            Row {
                spacing: 4
                ToolBtn {
                    label: AnimGeneratorManager.pathEditId === genSection.selectedId ? "Hide path" : "Show path"
                    accent: AnimGeneratorManager.pathEditId === genSection.selectedId
                    tip: "Shows the path in the viewport; drag its points with the Select tool (Q)"
                    onClicked: AnimGeneratorManager.pathEditId === genSection.selectedId
                               ? AnimGeneratorManager.endPathEdit()
                               : AnimGeneratorManager.beginPathEdit(genSection.selectedId)
                }
                ToolBtn { label: "+ Point"; onClicked: AnimGeneratorManager.addPathPoint(genSection.selectedId) }
            }
            Repeater {
                model: genSection.details.points || []
                delegate: Row {
                    id: ptRow
                    required property var modelData
                    required property int index
                    spacing: 3
                    Label {
                        width: 20; height: 22; text: ptRow.index
                        color: ptRow.index === AnimGeneratorManager.selectedPathPoint
                               ? AnimationControlController.highlightColor : AnimationControlController.textColor
                    }
                    Repeater {
                        model: 3
                        delegate: Rectangle {
                            id: coordBox
                            required property int index
                            width: (genSection.width - 60) / 3 - 3; height: 22; radius: 2
                            color: AnimationControlController.inputColor
                            border.color: cIn.activeFocus ? AnimationControlController.highlightColor : AnimationControlController.borderColor
                            TextInput {
                                id: cIn
                                anchors.fill: parent; anchors.margins: 4
                                color: AnimationControlController.textColor; font.pixelSize: 11
                                verticalAlignment: TextInput.AlignVCenter
                                selectByMouse: true; clip: true
                                text: Number(ptRow.modelData[coordBox.index]).toFixed(3)
                                onActiveFocusChanged: if (activeFocus) AnimGeneratorManager.selectPathPoint(ptRow.index)
                                onAccepted: {
                                    var p = [ptRow.modelData[0], ptRow.modelData[1], ptRow.modelData[2]]
                                    p[coordBox.index] = Number(text)
                                    if (!isNaN(p[coordBox.index]))
                                        AnimGeneratorManager.setPathPoint(genSection.selectedId, ptRow.index, p[0], p[1], p[2])
                                }
                            }
                        }
                    }
                    ToolBtn { label: "✕"; onClicked: AnimGeneratorManager.removePathPoint(genSection.selectedId, ptRow.index) }
                }
            }
        }
    }

    Text {
        width: parent.width - 8
        visible: AnimGeneratorManager.status !== ""
        wrapMode: Text.Wrap
        font.pixelSize: 10
        color: AnimGeneratorManager.lastOk ? AnimationControlController.textColor : "#e06060"
        text: AnimGeneratorManager.status
    }
}
