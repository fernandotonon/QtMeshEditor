pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import AnimationControl 1.0
import PropertiesPanel 1.0

// ── Animation constraints (#525) ─────────────────────────────────────────────
// Drive a bone or scene node from another object every frame, on top of its
// animation: Look at, 2-bone IK (on the END bone — a hand or a foot), Child
// of, Copy rotation / position and Limit rotation. Each owner has a STACK;
// the top-most constraint is applied last and wins. Bake writes the result
// into keyframes. Every action is one undo step (ConstraintManager pushes it).
Column {
    id: conSection
    width: parent ? parent.width : 300
    spacing: 6

    property string selectedId: ""
    property var details: ({})
    property int refreshTick: 0
    property bool showAll: false

    // Owner currently shown, as a ref string ("bone:E/B" | "node:N").
    readonly property string ownerRef: ownerPick.ref
    readonly property var rows: {
        conSection.refreshTick
        ConstraintManager.constraints   // dependency
        return conSection.showAll ? ConstraintManager.constraints : ConstraintManager.rowsFor(conSection.ownerRef)
    }

    function refreshDetails() {
        details = selectedId !== "" ? ConstraintManager.details(selectedId) : ({})
    }
    onSelectedIdChanged: refreshDetails()
    Connections {
        target: ConstraintManager
        function onConstraintsChanged() {
            var all = ConstraintManager.constraints
            var still = false
            for (var i = 0; i < all.length; ++i) if (all[i].id === conSection.selectedId) still = true
            if (!still) conSection.selectedId = ""
            conSection.refreshDetails()
        }
    }
    Connections {
        target: PropertiesPanelController
        function onSelectionChanged() {
            conSection.refreshTick++
            ownerPick.pickFromSelection()
        }
    }
    Connections {
        target: AnimationControlController
        function onBoneListChanged() { ownerPick.pickFromSelection() }
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


    // Picks a bone ("bone:Entity/Bone") or a scene node ("node:Name").
    component RefPicker: Column {
        id: rp
        property string title: "Owner"
        property bool optional: false
        property bool allowNode: true
        property bool allowBone: true
        property string kind: kindCombo.currentText
        readonly property string ref: {
            if (kind === "(none)" || objCombo.currentText === "") return ""
            if (kind === "bone") return boneCombo.currentText === "" ? "" : "bone:" + objCombo.currentText + "/" + boneCombo.currentText
            return "node:" + objCombo.currentText
        }
        function setRef(r) {
            if (r === "") return
            var k = r.substring(0, r.indexOf(":"))
            var rest = r.substring(r.indexOf(":") + 1)
            var ki = kindCombo.model.indexOf(k)
            if (ki < 0) return
            kindCombo.currentIndex = ki
            var obj = k === "bone" ? rest.substring(0, rest.indexOf("/")) : rest
            var oi = objCombo.model.indexOf(obj)
            if (oi >= 0) objCombo.currentIndex = oi
            if (k === "bone") {
                var bi = boneCombo.model.indexOf(rest.substring(rest.indexOf("/") + 1))
                if (bi >= 0) boneCombo.currentIndex = bi
            }
        }
        function pickFromSelection() { setRef(ConstraintManager.ownerFromSelection()) }
        width: conSection.width
        spacing: 3
        Row {
            spacing: 4
            Label { width: 52; height: 22; text: rp.title }
            ThemedComboBox {
                id: kindCombo
                width: 70; height: 22; font.pixelSize: 11
                model: {
                    var m = []
                    if (rp.optional) m.push("(none)")
                    if (rp.allowBone) m.push("bone")
                    if (rp.allowNode) m.push("node")
                    return m
                }
            }
            ThemedComboBox {
                id: objCombo
                visible: rp.kind !== "(none)"
                width: conSection.width - 134; height: 22; font.pixelSize: 11
                model: {
                    conSection.refreshTick
                    return rp.kind === "bone" ? ConstraintManager.skinnedEntities() : ConstraintManager.nodeNames()
                }
            }
        }
        Row {
            visible: rp.kind === "bone"
            spacing: 4
            Label { width: 52; height: 22; text: "Bone" }
            ThemedComboBox {
                id: boneCombo
                width: conSection.width - 60; height: 22; font.pixelSize: 11
                model: { conSection.refreshTick; return ConstraintManager.bonesOf(objCombo.currentText) }
            }
        }
    }

    component ParamField: Row {
        id: pf
        property string key: ""
        property string label: key
        property var value: undefined
        spacing: 4
        Label { width: 92; height: 22; text: pf.label }
        Rectangle {
            width: conSection.width - 120; height: 22; radius: 2
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
                    ConstraintManager.setParamFromUi(conSection.selectedId, pf.key, text)
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
        text: "Drive a bone or node from another object on top of its animation. "
            + "The top-most constraint of a stack wins; Bake writes the result into keyframes. "
            + "IK goes on the END bone (hand / foot) and bends its two parents."
    }

    // ---- Owner ----
    RefPicker {
        id: ownerPick
        title: "Owner"
        Component.onCompleted: pickFromSelection()
    }

    // ---- Add ----
    Column {
        id: addCol
        width: parent.width
        spacing: 3
        property string type: ConstraintManager.typeIds[typeBox.currentIndex] || "look-at"
        property bool needsTarget: type !== "limit-rotation"

        Row {
            spacing: 4
            Label { width: 52; height: 22; text: "Type" }
            ThemedComboBox {
                id: typeBox
                width: conSection.width - 60; height: 22; font.pixelSize: 11
                model: ConstraintManager.typeIds.map(function(t) { return ConstraintManager.typeLabel(t) })
            }
        }
        RefPicker {
            id: targetPick
            visible: addCol.needsTarget
            title: "Target"
        }
        RefPicker {
            id: polePick
            visible: addCol.type === "ik"
            title: "Pole"
            optional: true
        }
        Row {
            spacing: 4
            ToolBtn {
                label: "Add constraint"
                enabled: conSection.ownerRef !== "" && (!addCol.needsTarget || targetPick.ref !== "")
                tip: "Adds the constraint at the TOP of the owner's stack (it wins). One undo step"
                onClicked: {
                    if (ConstraintManager.addFromUi(addCol.type, conSection.ownerRef,
                                                    addCol.needsTarget ? targetPick.ref : "",
                                                    addCol.type === "ik" ? polePick.ref : "")) {
                        var rows = ConstraintManager.rowsFor(conSection.ownerRef)
                        if (rows.length > 0) conSection.selectedId = rows[0].id
                    }
                }
            }
            ToolBtn {
                label: "↻"
                tip: "Refresh the object lists and pick the selection as owner"
                onClicked: { conSection.refreshTick++; ownerPick.pickFromSelection() }
            }
        }
    }

    Rectangle { width: parent.width - 8; height: 1; color: AnimationControlController.borderColor; opacity: 0.6 }

    // ---- Stack ----
    Row {
        spacing: 8
        Text {
            height: 22
            verticalAlignment: Text.AlignVCenter
            color: AnimationControlController.textColor; font.pixelSize: 11; font.bold: true
            text: conSection.showAll ? "All constraints" : "Stack (top wins)"
        }
        InspectorCheckBox {
            text: "Show all"
            checked: conSection.showAll
            onToggled: conSection.showAll = checked
        }
    }
    Text {
        visible: conSection.rows.length === 0
        color: AnimationControlController.disabledTextColor
        font.pixelSize: 10
        text: conSection.showAll ? "No constraints yet." : "No constraints on this owner."
    }
    Repeater {
        model: conSection.rows
        delegate: Rectangle {
            id: rowRect
            required property var modelData
            width: conSection.width - 8
            height: 26
            radius: 3
            color: modelData.id === conSection.selectedId
                ? Qt.rgba(AnimationControlController.highlightColor.r, AnimationControlController.highlightColor.g,
                          AnimationControlController.highlightColor.b, 0.25)
                : "transparent"
            border.color: AnimationControlController.borderColor
            border.width: modelData.id === conSection.selectedId ? 1 : 0
            Row {
                anchors.fill: parent; anchors.leftMargin: 2; spacing: 3
                InspectorCheckBox {
                    anchors.verticalCenter: parent.verticalCenter
                    checked: rowRect.modelData.enabled
                    ToolTip.visible: hovered
                    ToolTip.text: "Live (unchecked = muted)"
                    onToggled: ConstraintManager.setEnabledFromUi(rowRect.modelData.id, checked)
                }
                Text {
                    width: conSection.width - 128
                    anchors.verticalCenter: parent.verticalCenter
                    elide: Text.ElideRight
                    color: rowRect.modelData.bound ? AnimationControlController.textColor : "#e08060"
                    font.pixelSize: 11
                    text: rowRect.modelData.typeLabel
                          + (rowRect.modelData.target !== "" ? " → " + rowRect.modelData.target.substring(rowRect.modelData.target.indexOf(":") + 1) : "")
                          + (conSection.showAll ? "  (" + rowRect.modelData.owner.substring(rowRect.modelData.owner.indexOf(":") + 1) + ")" : "")
                          + (rowRect.modelData.bound ? "" : "  [missing]")
                    MouseArea {
                        id: rowMa
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: conSection.selectedId = rowRect.modelData.id
                    }
                    ToolTip.visible: rowMa.containsMouse
                    ToolTip.text: rowRect.modelData.owner + " ← " + (rowRect.modelData.target || "(no target)")
                    ToolTip.delay: 500
                }
                ToolBtn {
                    anchors.verticalCenter: parent.verticalCenter
                    label: "▲"; tip: "Move up (toward the top — applied later, wins)"
                    onClicked: ConstraintManager.moveFromUi(rowRect.modelData.id, -1)
                }
                ToolBtn {
                    anchors.verticalCenter: parent.verticalCenter
                    label: "▼"; tip: "Move down"
                    onClicked: ConstraintManager.moveFromUi(rowRect.modelData.id, 1)
                }
                ToolBtn {
                    anchors.verticalCenter: parent.verticalCenter
                    label: "✕"; tip: "Remove the constraint"
                    onClicked: ConstraintManager.removeFromUi(rowRect.modelData.id)
                }
            }
        }
    }

    // ---- Parameters of the selected constraint ----
    Column {
        visible: conSection.selectedId !== "" && conSection.details.type !== undefined
        width: parent.width
        spacing: 3
        property string t: conSection.details.type || ""

        Text {
            color: AnimationControlController.textColor; font.pixelSize: 11; font.bold: true
            text: (conSection.details.typeLabel || "") + " parameters"
        }
        ParamField { key: "influence"; label: "Influence (0–1)"; value: conSection.details.influence }
        Row {
            visible: parent.t === "look-at"
            spacing: 4
            Label { width: 92; height: 22; text: "Aim / up axis" }
            ThemedComboBox {
                width: (conSection.width - 104) / 2; height: 22; font.pixelSize: 11
                model: ["x", "y", "z", "-x", "-y", "-z"]
                currentIndex: Math.max(0, model.indexOf(conSection.details.aim || "z"))
                onActivated: function(i) { ConstraintManager.setParamFromUi(conSection.selectedId, "aim", model[i]) }
            }
            ThemedComboBox {
                width: (conSection.width - 104) / 2; height: 22; font.pixelSize: 11
                model: ["x", "y", "z", "-x", "-y", "-z"]
                currentIndex: Math.max(0, model.indexOf(conSection.details.up || "y"))
                onActivated: function(i) { ConstraintManager.setParamFromUi(conSection.selectedId, "up", model[i]) }
            }
        }
        Row {
            visible: parent.t === "copy-position"
            spacing: 8
            Repeater {
                model: ["x", "y", "z"]
                delegate: InspectorCheckBox {
                    required property string modelData
                    text: modelData.toUpperCase()
                    checked: conSection.details[modelData] !== false
                    onToggled: ConstraintManager.setParamFromUi(conSection.selectedId, modelData, checked ? "true" : "false")
                }
            }
        }
        Repeater {
            model: parent.t === "limit-rotation" ? ["x", "y", "z"] : []
            delegate: Row {
                id: limRow
                required property string modelData
                spacing: 4
                InspectorCheckBox {
                    width: 92
                    text: limRow.modelData.toUpperCase() + " (°)"
                    checked: conSection.details["limit_" + limRow.modelData] !== false
                    onToggled: ConstraintManager.setParamFromUi(conSection.selectedId, "limit_" + limRow.modelData, checked ? "true" : "false")
                }
                Repeater {
                    model: ["min", "max"]
                    delegate: Rectangle {
                        id: limBox
                        required property string modelData
                        width: (conSection.width - 112) / 2; height: 22; radius: 2
                        color: AnimationControlController.inputColor
                        border.color: limIn.activeFocus ? AnimationControlController.highlightColor : AnimationControlController.borderColor
                        TextInput {
                            id: limIn
                            anchors.fill: parent; anchors.margins: 4
                            color: AnimationControlController.textColor; font.pixelSize: 11
                            verticalAlignment: TextInput.AlignVCenter
                            selectByMouse: true; clip: true
                            readonly property string key: limBox.modelData + "_" + limRow.modelData
                            text: conSection.details[key] === undefined ? "" : String(conSection.details[key])
                            onAccepted: ConstraintManager.setParamFromUi(conSection.selectedId, key, text)
                            onActiveFocusChanged: if (!activeFocus && text !== String(conSection.details[key]))
                                ConstraintManager.setParamFromUi(conSection.selectedId, key, text)
                        }
                    }
                }
            }
        }
    }

    Rectangle { width: parent.width - 8; height: 1; color: AnimationControlController.borderColor; opacity: 0.6 }

    // ---- Bake ----
    Row {
        spacing: 4
        Label { width: 52; height: 22; text: "Bake" }
        ThemedComboBox {
            id: bakeClip
            visible: ownerPick.kind === "bone"
            width: conSection.width - 172; height: 22; font.pixelSize: 11
            model: {
                conSection.refreshTick
                var r = conSection.ownerRef
                if (!r.startsWith("bone:")) return []
                return ConstraintManager.clipsOf(r.substring(5, r.indexOf("/")))
            }
            ToolTip.visible: hovered
            ToolTip.text: "Skeletal clip the constrained bones are baked into"
        }
        Rectangle {
            width: 40; height: 22; radius: 2
            color: AnimationControlController.inputColor
            border.color: AnimationControlController.borderColor
            TextInput {
                id: fpsIn
                anchors.fill: parent; anchors.margins: 4
                color: AnimationControlController.textColor; font.pixelSize: 11
                verticalAlignment: TextInput.AlignVCenter
                text: "30"
                validator: IntValidator { bottom: 1; top: 240 }
            }
        }
        ToolBtn {
            label: conSection.showAll ? "Bake all" : "Bake stack"
            enabled: conSection.rows.length > 0
            tip: "Write the constrained motion into keyframes (fps at left) and mute the baked constraints. One undo step"
            onClicked: ConstraintManager.bakeFromUi(conSection.showAll ? "" : conSection.ownerRef,
                                                    bakeClip.visible ? bakeClip.currentText : "",
                                                    parseInt(fpsIn.text) || 30)
        }
    }

    Text {
        width: parent.width - 8
        visible: ConstraintManager.status !== ""
        wrapMode: Text.Wrap
        font.pixelSize: 10
        color: ConstraintManager.lastOk ? AnimationControlController.textColor : "#e06060"
        text: ConstraintManager.status
    }
}
