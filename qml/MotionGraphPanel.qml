pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import AnimationControl 1.0
import PropertiesPanel 1.0

// ── Motion graph (#526) ──────────────────────────────────────────────────────
// A small state machine to PREVIEW clip logic: states are clips, arrows are
// transitions (conditions on parameters, optional exit time, blend duration +
// curve, optional bone mask for partial "upper-body" transitions). Press Play
// and flip the parameters to see it run. Authoring/preview only — export
// writes the clips, never an engine graph. Every edit is one undo step.
Column {
    id: mg
    width: parent ? parent.width : 300
    spacing: 6

    property string selState: ""
    property string selTransition: ""
    property bool connecting: false        // "add transition" picking a target
    property var tDetails: ({})
    property int refreshTick: 0

    function refreshDetails() { tDetails = selTransition !== "" ? MotionGraphManager.transitionDetails(selTransition) : ({}) }
    onSelTransitionChanged: refreshDetails()
    Connections {
        target: MotionGraphManager
        function onGraphChanged() {
            var found = false
            for (var i = 0; i < MotionGraphManager.states.length; ++i)
                if (MotionGraphManager.states[i].name === mg.selState) found = true
            if (!found) mg.selState = ""
            var tf = false
            for (var j = 0; j < MotionGraphManager.transitions.length; ++j)
                if (MotionGraphManager.transitions[j].id === mg.selTransition) tf = true
            if (!tf) mg.selTransition = ""
            mg.refreshDetails()
            edgeCanvas.requestPaint()
        }
        function onPlaybackChanged() { edgeCanvas.requestPaint() }
    }
    Connections {
        target: PropertiesPanelController
        function onSelectionChanged() {
            mg.refreshTick++
            var e = MotionGraphManager.entityFromSelection()
            if (e !== "" && !MotionGraphManager.playing) MotionGraphManager.entity = e
        }
    }
    Component.onCompleted: {
        var e = MotionGraphManager.entityFromSelection()
        if (e !== "") MotionGraphManager.entity = e
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


    component Field: Rectangle {
        id: fld
        property string text: ""
        property string placeholder: ""
        readonly property alias input: fin
        signal committed(string value)
        height: 22; radius: 2
        color: AnimationControlController.inputColor
        border.color: fin.activeFocus ? AnimationControlController.highlightColor : AnimationControlController.borderColor
        TextInput {
            id: fin
            anchors.fill: parent; anchors.margins: 4
            color: AnimationControlController.textColor; font.pixelSize: 11
            verticalAlignment: TextInput.AlignVCenter
            selectByMouse: true; clip: true
            text: fld.text
            onAccepted: if (text !== fld.text) fld.committed(text)
            onActiveFocusChanged: if (!activeFocus && text !== fld.text) fld.committed(text)
            Text {
                visible: fin.text === "" && !fin.activeFocus
                text: fld.placeholder; font.pixelSize: 11
                color: AnimationControlController.disabledTextColor
                anchors.verticalCenter: parent.verticalCenter
            }
        }
    }

    Text {
        width: parent.width - 8
        wrapMode: Text.Wrap
        opacity: 0.8
        color: AnimationControlController.textColor
        font.pixelSize: 10
        text: "Preview clip logic: states are clips, arrows are transitions driven by parameters. "
            + "Authoring/preview only — export writes the clips, not the graph."
    }

    // ---- Entity + quick actions ----
    Row {
        spacing: 4
        Label { width: 52; height: 22; text: "Entity" }
        ThemedComboBox {
            id: entityBox
            width: mg.width - 60; height: 22; font.pixelSize: 11
            enabled: !MotionGraphManager.playing
            model: { mg.refreshTick; return MotionGraphManager.skinnedEntities() }
            currentIndex: model.indexOf(MotionGraphManager.entity)
            onActivated: function(i) { MotionGraphManager.entity = model[i] }
        }
    }
    Row {
        spacing: 4
        ThemedComboBox {
            id: clipBox
            width: mg.width - 150; height: 22; font.pixelSize: 11
            model: { mg.refreshTick; return MotionGraphManager.clipsOf(MotionGraphManager.entity) }
        }
        ToolBtn {
            label: "+ State"
            enabled: clipBox.currentText !== ""
            tip: "Add a state playing this clip"
            onClicked: {
                var n = MotionGraphManager.states.length
                var name = MotionGraphManager.addState(clipBox.currentText, 30 + (n % 3) * 190, 40 + Math.floor(n / 3) * 80)
                if (name !== "") { mg.selState = name; mg.selTransition = "" }
            }
        }
        ToolBtn {
            label: "Template"
            enabled: clipBox.count > 0
            tip: "Build an idle → walk → run graph driven by a 'speed' parameter (replaces the current graph)"
            onClicked: MotionGraphManager.buildLocomotionTemplate()
        }
    }

    // ---- Graph view ----
    Rectangle {
        id: view
        width: mg.width - 8
        height: 230
        clip: true
        radius: 3
        color: Qt.darker(AnimationControlController.inputColor, 1.08)
        border.color: AnimationControlController.borderColor

        readonly property int nodeW: 120
        readonly property int nodeH: 40
        function nodeRect(name) {
            if (name === "*") return {x: 6, y: 6, w: 44, h: 22}
            var s = MotionGraphManager.states
            for (var i = 0; i < s.length; ++i)
                if (s[i].name === name) return {x: s[i].x, y: s[i].y, w: nodeW, h: nodeH}
            return null
        }
        // Edge endpoints, shifted to the right of their direction so a pair
        // of opposite transitions draws as two separate arrows.
        function edgeGeom(t) {
            var a = nodeRect(t.from), b = nodeRect(t.to)
            if (!a || !b) return null
            var ax = a.x + a.w / 2, ay = a.y + a.h / 2, bx = b.x + b.w / 2, by = b.y + b.h / 2
            var dx = bx - ax, dy = by - ay, len = Math.sqrt(dx * dx + dy * dy)
            if (len < 1) return null
            var ux = dx / len, uy = dy / len, ox = -uy * 7, oy = ux * 7
            // clip to the node boxes (approximate: shrink by the half-extent along the direction)
            var ta = Math.min(Math.abs(a.w / 2 / (ux || 1e-6)), Math.abs(a.h / 2 / (uy || 1e-6)))
            var tb = Math.min(Math.abs(b.w / 2 / (ux || 1e-6)), Math.abs(b.h / 2 / (uy || 1e-6)))
            return {x1: ax + ux * ta + ox, y1: ay + uy * ta + oy, x2: bx - ux * tb + ox, y2: by - uy * tb + oy}
        }

        // Auto-fit: the graph content scales down to fit the view, so a
        // layout wider than the Inspector stays fully visible.
        readonly property real contentW: {
            var w = 140
            var s = MotionGraphManager.states
            for (var i = 0; i < s.length; ++i) w = Math.max(w, s[i].x + nodeW + 12)
            return w
        }
        readonly property real contentH: {
            var h = 60
            var s = MotionGraphManager.states
            for (var i = 0; i < s.length; ++i) h = Math.max(h, s[i].y + nodeH + 12)
            return h
        }
        readonly property real fit: Math.min(1.0, (width - 4) / contentW, (height - 4) / contentH)
        onFitChanged: edgeCanvas.requestPaint()

        Item {
            id: layer
            x: 2; y: 2
            width: Math.max(view.contentW, (view.width - 4) / view.fit)
            height: Math.max(view.contentH, (view.height - 4) / view.fit)
            scale: view.fit
            transformOrigin: Item.TopLeft

        Canvas {
            id: edgeCanvas
            anchors.fill: parent
            renderStrategy: Canvas.Immediate
            onPaint: {
                var ctx = getContext("2d")
                ctx.reset()
                var ts = MotionGraphManager.transitions
                for (var i = 0; i < ts.length; ++i) {
                    var g = view.edgeGeom(ts[i])
                    if (!g) continue
                    var sel = ts[i].id === mg.selTransition
                    var live = MotionGraphManager.playing && ts[i].id === MotionGraphManager.lastTransition
                    ctx.strokeStyle = sel ? AnimationControlController.highlightColor
                                    : live ? "#e0b050"
                                    : ts[i].masked ? "#60b0e0" : AnimationControlController.textColor
                    ctx.fillStyle = ctx.strokeStyle
                    ctx.lineWidth = sel ? 2.5 : 1.5
                    if (ts[i].masked) ctx.setLineDash([5, 3]); else ctx.setLineDash([])
                    ctx.beginPath(); ctx.moveTo(g.x1, g.y1); ctx.lineTo(g.x2, g.y2); ctx.stroke()
                    var ang = Math.atan2(g.y2 - g.y1, g.x2 - g.x1)
                    ctx.setLineDash([])
                    ctx.beginPath()
                    ctx.moveTo(g.x2, g.y2)
                    ctx.lineTo(g.x2 - 9 * Math.cos(ang - 0.4), g.y2 - 9 * Math.sin(ang - 0.4))
                    ctx.lineTo(g.x2 - 9 * Math.cos(ang + 0.4), g.y2 - 9 * Math.sin(ang + 0.4))
                    ctx.closePath(); ctx.fill()
                }
            }
        }
        // Click on empty canvas: select the nearest arrow (within 6 px).
        MouseArea {
            anchors.fill: parent
            onClicked: function(mouse) {
                if (mg.connecting) { mg.connecting = false; return }
                var best = "", bestD = 6
                var ts = MotionGraphManager.transitions
                for (var i = 0; i < ts.length; ++i) {
                    var g = view.edgeGeom(ts[i])
                    if (!g) continue
                    var dx = g.x2 - g.x1, dy = g.y2 - g.y1, l2 = dx * dx + dy * dy
                    var t = Math.max(0, Math.min(1, ((mouse.x - g.x1) * dx + (mouse.y - g.y1) * dy) / l2))
                    var px = g.x1 + t * dx - mouse.x, py = g.y1 + t * dy - mouse.y
                    var d = Math.sqrt(px * px + py * py)
                    if (d < bestD) { bestD = d; best = ts[i].id }
                }
                mg.selTransition = best
                if (best !== "") mg.selState = ""
                edgeCanvas.requestPaint()
            }
        }

        // "Any state" source pill (drawn when some transition uses it)
        Rectangle {
            x: 6; y: 6; width: 44; height: 22; radius: 11
            visible: { for (var i = 0; i < MotionGraphManager.transitions.length; ++i)
                           if (MotionGraphManager.transitions[i].from === "*") return true
                       return mg.connecting }
            color: mg.connecting ? AnimationControlController.highlightColor : AnimationControlController.buttonColor
            border.color: AnimationControlController.borderColor
            Text { anchors.centerIn: parent; text: "Any"; font.pixelSize: 10; color: AnimationControlController.buttonTextColor }
        }

        Repeater {
            model: MotionGraphManager.states
            delegate: Rectangle {
                id: node
                required property var modelData
                readonly property bool isCurrent: MotionGraphManager.playing && MotionGraphManager.currentState === modelData.name
                readonly property bool isPrevious: MotionGraphManager.playing && MotionGraphManager.previousState === modelData.name
                x: modelData.x; y: modelData.y
                width: view.nodeW; height: view.nodeH
                radius: 5
                color: isCurrent ? Qt.rgba(AnimationControlController.highlightColor.r, AnimationControlController.highlightColor.g,
                                           AnimationControlController.highlightColor.b, 0.35 + 0.45 * MotionGraphManager.blendProgress)
                     : isPrevious ? Qt.rgba(0.88, 0.69, 0.31, 0.45 * (1 - MotionGraphManager.blendProgress))
                     : AnimationControlController.buttonColor
                border.width: mg.selState === modelData.name ? 2 : 1
                border.color: mg.selState === modelData.name ? AnimationControlController.highlightColor : AnimationControlController.borderColor
                Column {
                    anchors.centerIn: parent
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: (node.modelData.entry ? "▶ " : "") + node.modelData.name
                        font.pixelSize: 11; font.bold: true
                        color: AnimationControlController.buttonTextColor
                        elide: Text.ElideRight; width: Math.min(implicitWidth, view.nodeW - 8)
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: node.modelData.clip + (node.modelData.loop ? "" : " (once)")
                        font.pixelSize: 9; opacity: 0.75
                        color: AnimationControlController.buttonTextColor
                        elide: Text.ElideRight; width: Math.min(implicitWidth, view.nodeW - 8)
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    drag.target: mg.connecting ? null : node
                    drag.minimumX: 0; drag.minimumY: 0
                    drag.maximumX: 4000; drag.maximumY: 4000
                    property bool moved: false
                    onPressed: moved = false
                    onPositionChanged: if (drag.active) {
                        moved = true
                        MotionGraphManager.moveState(node.modelData.name, node.x, node.y, false)
                    }
                    onReleased: if (moved) MotionGraphManager.moveState(node.modelData.name, node.x, node.y, true)
                    onClicked: {
                        if (mg.connecting) {
                            var id = MotionGraphManager.addTransition(mg.selState === "" ? "*" : mg.selState, node.modelData.name)
                            mg.connecting = false
                            if (id !== "") { mg.selTransition = id; mg.selState = "" }
                            return
                        }
                        mg.selState = node.modelData.name
                        mg.selTransition = ""
                        edgeCanvas.requestPaint()
                    }
                }
            }
        }
        }   // layer

        Text {
            anchors.centerIn: parent
            visible: MotionGraphManager.states.length === 0
            width: parent.width - 20
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            color: AnimationControlController.disabledTextColor
            font.pixelSize: 11
            text: MotionGraphManager.entity === "" ? "Select an animated entity."
                                                   : "Add states (+ State) or build the idle/walk/run Template."
        }
        Text {
            anchors.bottom: parent.bottom; anchors.right: parent.right; anchors.margins: 4
            visible: mg.connecting
            text: "Click the target state (Esc: click empty space)"
            font.pixelSize: 10
            color: AnimationControlController.highlightColor
        }
    }

    // ---- Playback ----
    Row {
        spacing: 4
        ToolBtn {
            label: MotionGraphManager.playing ? "■ Stop" : "▶ Play graph"
            accent: MotionGraphManager.playing
            enabled: MotionGraphManager.playing || MotionGraphManager.states.length > 0
            tip: "Run the graph on the entity (it owns the entity's clips while playing; Stop restores them)"
            onClicked: MotionGraphManager.playing ? MotionGraphManager.stopFromUi() : MotionGraphManager.playFromUi()
        }
        Label {
            height: 22
            text: MotionGraphManager.playing
                  ? ("State: " + MotionGraphManager.currentState
                     + (MotionGraphManager.previousState !== "" ? "  (blending from " + MotionGraphManager.previousState + " "
                        + Math.round(MotionGraphManager.blendProgress * 100) + "%)" : ""))
                  : ""
        }
    }

    // ---- Parameters ----
    Text {
        color: AnimationControlController.textColor; font.pixelSize: 11; font.bold: true
        text: "Parameters" + (MotionGraphManager.playing ? " (live)" : " (defaults)")
    }
    Repeater {
        model: MotionGraphManager.params
        delegate: Row {
            id: prow
            required property var modelData
            spacing: 4
            Label { width: 80; height: 22; text: prow.modelData.name; elide: Text.ElideRight }
            Label { width: 40; height: 22; text: prow.modelData.type; opacity: 0.6 }
            InspectorCheckBox {
                visible: prow.modelData.type === "bool"
                checked: prow.modelData.value !== 0
                onToggled: MotionGraphManager.setParamFromUi(prow.modelData.name, checked ? 1 : 0)
            }
            Slider {
                id: pslider
                visible: prow.modelData.type === "float"
                width: Math.max(60, mg.width - 210); height: 22
                from: 0; to: Math.max(5, prow.modelData.value)
                value: prow.modelData.value
                onMoved: MotionGraphManager.setParamFromUi(prow.modelData.name, Math.round(value * 100) / 100)
                background: Rectangle {
                    x: pslider.leftPadding; y: pslider.topPadding + pslider.availableHeight / 2 - height / 2
                    width: pslider.availableWidth; height: 4; radius: 2
                    color: AnimationControlController.inputColor
                    border.color: AnimationControlController.borderColor
                    Rectangle {
                        width: pslider.visualPosition * parent.width; height: parent.height; radius: 2
                        color: AnimationControlController.highlightColor
                    }
                }
                handle: Rectangle {
                    x: pslider.leftPadding + pslider.visualPosition * (pslider.availableWidth - width)
                    y: pslider.topPadding + pslider.availableHeight / 2 - height / 2
                    width: 12; height: 12; radius: 6
                    color: pslider.pressed ? AnimationControlController.highlightColor : AnimationControlController.buttonColor
                    border.color: AnimationControlController.borderColor
                }
            }
            Label {
                visible: prow.modelData.type === "float"
                width: 34; height: 22
                text: Number(prow.modelData.value).toFixed(2)
            }
            ToolBtn {
                visible: prow.modelData.type === "trigger"
                label: "Fire"
                enabled: MotionGraphManager.playing
                tip: "Fire the trigger (consumed by the transition it starts)"
                onClicked: MotionGraphManager.fireTrigger(prow.modelData.name)
            }
            ToolBtn {
                label: "✕"; enabled: !MotionGraphManager.playing
                tip: "Remove the parameter"
                onClicked: MotionGraphManager.removeParam(prow.modelData.name)
            }
        }
    }
    Row {
        spacing: 4
        Field { id: newParam; width: mg.width - 170; placeholder: "parameter name" }
        ThemedComboBox { id: newParamType; width: 70; height: 22; font.pixelSize: 11; model: ["float", "bool", "trigger"] }
        ToolBtn {
            label: "+ Param"
            enabled: !MotionGraphManager.playing
            onClicked: {
                var name = newParam.input.text.trim()
                if (name !== "" && MotionGraphManager.addParam(name, newParamType.currentText)) newParam.input.text = ""
            }
        }
    }

    Rectangle { width: parent.width - 8; height: 1; color: AnimationControlController.borderColor; opacity: 0.6 }

    // ---- Selected state ----
    Column {
        visible: mg.selState !== ""
        width: parent.width
        spacing: 3
        property var st: {
            var s = MotionGraphManager.states
            for (var i = 0; i < s.length; ++i) if (s[i].name === mg.selState) return s[i]
            return ({})
        }
        Text { color: AnimationControlController.textColor; font.pixelSize: 11; font.bold: true; text: "State" }
        Row {
            spacing: 4
            Label { width: 52; height: 22; text: "Name" }
            Field {
                width: mg.width - 60
                text: mg.selState
                onCommitted: function(v) { if (MotionGraphManager.renameState(mg.selState, v)) mg.selState = v.trim() }
            }
        }
        Row {
            spacing: 4
            Label { width: 52; height: 22; text: "Clip" }
            ThemedComboBox {
                width: mg.width - 60; height: 22; font.pixelSize: 11
                model: MotionGraphManager.clipsOf(MotionGraphManager.entity)
                currentIndex: model.indexOf(parent.parent.st.clip || "")
                onActivated: function(i) { MotionGraphManager.setStateField(mg.selState, "clip", model[i]) }
            }
        }
        Row {
            spacing: 8
            InspectorCheckBox {
                text: "Loop"; checked: parent.parent.st.loop !== false
                onToggled: MotionGraphManager.setStateField(mg.selState, "loop", checked)
            }
            Label { height: 22; text: "Speed" }
            Field {
                width: 50
                text: parent.parent.st.speed !== undefined ? String(parent.parent.st.speed) : "1"
                onCommitted: function(v) { MotionGraphManager.setStateField(mg.selState, "speed", v) }
            }
        }
        Row {
            spacing: 4
            ToolBtn {
                label: "Set entry"; enabled: parent.parent.st.entry !== true
                tip: "Play starts in this state"
                onClicked: MotionGraphManager.setEntry(mg.selState)
            }
            ToolBtn {
                label: "→ Transition…"
                accent: mg.connecting
                tip: "Then click the target state"
                onClicked: mg.connecting = !mg.connecting
            }
            ToolBtn { label: "✕ Remove"; onClicked: MotionGraphManager.removeState(mg.selState) }
        }
    }
    Row {
        visible: mg.selState === "" && mg.selTransition === "" && MotionGraphManager.states.length > 0
        spacing: 4
        ToolBtn {
            label: "Any → …"
            accent: mg.connecting
            tip: "Add a transition from ANY state, then click the target"
            onClicked: { mg.selState = ""; mg.connecting = !mg.connecting }
        }
        Label { height: 22; text: "Click a state or an arrow to edit it"; opacity: 0.7 }
    }

    // ---- Selected transition ----
    Column {
        visible: mg.selTransition !== "" && mg.tDetails.id !== undefined
        width: parent.width
        spacing: 3
        Row {
            spacing: 4
            Text {
                height: 22; verticalAlignment: Text.AlignVCenter
                color: AnimationControlController.textColor; font.pixelSize: 11; font.bold: true
                text: "Transition " + (mg.tDetails.label || "")
            }
            ToolBtn { label: "▲"; tip: "Higher priority (checked first)"; onClicked: MotionGraphManager.moveTransition(mg.selTransition, -1) }
            ToolBtn { label: "▼"; tip: "Lower priority"; onClicked: MotionGraphManager.moveTransition(mg.selTransition, 1) }
            ToolBtn { label: "✕"; tip: "Remove the transition"; onClicked: MotionGraphManager.removeTransition(mg.selTransition) }
        }
        Row {
            spacing: 4
            Label { width: 52; height: 22; text: "Blend (s)" }
            Field {
                width: 50
                text: mg.tDetails.duration !== undefined ? String(mg.tDetails.duration) : ""
                onCommitted: function(v) { MotionGraphManager.setTransitionField(mg.selTransition, "duration", v) }
            }
            ThemedComboBox {
                width: 76; height: 22; font.pixelSize: 11
                model: ["linear", "ease", "step"]
                currentIndex: Math.max(0, model.indexOf(mg.tDetails.curve || "linear"))
                onActivated: function(i) { MotionGraphManager.setTransitionField(mg.selTransition, "curve", model[i]) }
            }
            Label { height: 22; text: "Exit" }
            Field {
                width: 44
                placeholder: "—"
                text: mg.tDetails.exitTime !== undefined ? String(mg.tDetails.exitTime) : ""
                onCommitted: function(v) { MotionGraphManager.setTransitionField(mg.selTransition, "exitTime", v) }
            }
        }
        Text {
            width: mg.width - 8; wrapMode: Text.Wrap
            font.pixelSize: 10; opacity: 0.7; color: AnimationControlController.textColor
            text: "Fires when ALL conditions hold" + (mg.tDetails.exitTime !== "" && mg.tDetails.exitTime !== undefined
                                                         ? " and the source clip passed its exit time (0..1)" : "")
                  + ". No conditions = as soon as possible."
        }
        Repeater {
            model: mg.tDetails.conditions || []
            delegate: Row {
                id: crow
                required property var modelData
                required property int index
                spacing: 3
                ThemedComboBox {
                    width: 90; height: 22; font.pixelSize: 11
                    model: MotionGraphManager.params.map(function(p) { return p.name })
                    currentIndex: model.indexOf(crow.modelData.param)
                    onActivated: function(i) { MotionGraphManager.setCondition(mg.selTransition, crow.index, model[i], crow.modelData.op, crow.modelData.value) }
                }
                ThemedComboBox {
                    width: 58; height: 22; font.pixelSize: 11
                    model: MotionGraphManager.opIds
                    currentIndex: model.indexOf(crow.modelData.op)
                    onActivated: function(i) { MotionGraphManager.setCondition(mg.selTransition, crow.index, crow.modelData.param, model[i], crow.modelData.value) }
                }
                Field {
                    width: 50
                    visible: crow.modelData.op !== "true" && crow.modelData.op !== "false"
                    text: String(crow.modelData.value)
                    onCommitted: function(v) {
                        var n = Number(v)
                        if (!isNaN(n)) MotionGraphManager.setCondition(mg.selTransition, crow.index, crow.modelData.param, crow.modelData.op, n)
                    }
                }
                ToolBtn { label: "✕"; onClicked: MotionGraphManager.removeCondition(mg.selTransition, crow.index) }
            }
        }
        ToolBtn {
            label: "+ Condition"
            enabled: MotionGraphManager.params.length > 0
            tip: MotionGraphManager.params.length > 0 ? "Add a condition on a parameter" : "Add a parameter first"
            onClicked: {
                var p = MotionGraphManager.params[0]
                MotionGraphManager.addCondition(mg.selTransition, p.name, p.type === "float" ? ">" : "true", 0)
            }
        }
        Row {
            spacing: 4
            Label { width: 52; height: 22; text: "Mask" }
            ThemedComboBox {
                id: maskBone
                width: mg.width - 196; height: 22; font.pixelSize: 11
                model: { mg.refreshTick; return MotionGraphManager.bonesOf(MotionGraphManager.entity) }
            }
            ToolBtn {
                label: "Subtree"
                tip: "Partial transition: the target drives only this bone and everything below it (e.g. Spine = upper body); the clip that was playing keeps the rest"
                onClicked: MotionGraphManager.setTransitionMaskFromBone(mg.selTransition, maskBone.currentText, true)
            }
            ToolBtn {
                label: "Clear"
                enabled: (mg.tDetails.mask || []).length > 0
                onClicked: MotionGraphManager.setTransitionMaskFromBone(mg.selTransition, "", false)
            }
        }
        Label {
            height: 18
            text: (mg.tDetails.mask || []).length > 0
                  ? "Partial: " + mg.tDetails.mask.length + " bones from " + mg.tDetails.mask[0] + " (dashed arrow)"
                  : "Whole body"
            opacity: 0.75
        }
    }

    Text {
        width: parent.width - 8
        visible: MotionGraphManager.status !== ""
        wrapMode: Text.Wrap
        font.pixelSize: 10
        color: MotionGraphManager.lastOk ? AnimationControlController.textColor : "#e06060"
        text: MotionGraphManager.status
    }
}
