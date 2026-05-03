import QtQuick
import QtQuick.Controls
import AnimationControl 1.0

// Curve editor — visualizes per-channel animation curves with Bezier handles.
// Reads keyframe times + values from AnimationControlController and tangent
// state from CurveEditModel. Read-only display in this slice (D3a); handle
// dragging + resample-into-track lands in D3b. The dope sheet remains the
// primary editing surface for keyframe selection and time shifts.
Rectangle {
    id: root
    color: AnimationControlController.panelColor
    focus: true

    property real pxPerSec: 200
    property real viewStart: 0.0
    property real yScale: 60   // px per unit value
    property real yCenter: 0   // value at the vertical center of the canvas

    property int leftStripWidth: 130

    // Pulled from AnimationControlController on signal. Each row is the same
    // shape as the dope sheet's allBoneRows() returns: { bone, keyTimes,
    // channels: { tx, ty, ..., sz: bool } }.
    property var rows: AnimationControlController.allBoneRows()

    // Selected bone — only that bone's animated channels are drawn. Reuses
    // AnimationControlController.selectedBone for cross-panel sync.
    readonly property string selectedBone: AnimationControlController.selectedBone

    readonly property var channelOrder: [
        { id: "tx", label: "T.X", color: "#c04040" },
        { id: "ty", label: "T.Y", color: "#40c040" },
        { id: "tz", label: "T.Z", color: "#4040c0" },
        { id: "rw", label: "R.W", color: "#a040a0" },
        { id: "rx", label: "R.X", color: "#c04040" },
        { id: "ry", label: "R.Y", color: "#40c040" },
        { id: "rz", label: "R.Z", color: "#4040c0" },
        { id: "sx", label: "S.X", color: "#c08040" },
        { id: "sy", label: "S.Y", color: "#80c040" },
        { id: "sz", label: "S.Z", color: "#4080c0" }
    ]

    function selectedBoneRow() {
        for (var i = 0; i < rows.length; i++) {
            if (rows[i].bone === selectedBone) return rows[i]
        }
        return null
    }

    function activeChannelsFor(row) {
        if (!row || !row.channels) return []
        var result = []
        for (var i = 0; i < channelOrder.length; i++) {
            if (row.channels[channelOrder[i].id]) result.push(channelOrder[i])
        }
        return result
    }

    function activeChannelsForSelected() {
        return activeChannelsFor(selectedBoneRow())
    }

    Connections {
        target: AnimationControlController
        function onBoneRowsChanged()      {
            root.rows = AnimationControlController.allBoneRows()
            curveCanvas.refreshChannelValues(root.selectedBoneRow())
            curveCanvas.requestPaint()
        }
        function onSelectionChanged()     {
            root.rows = AnimationControlController.allBoneRows()
            curveCanvas.refreshChannelValues(root.selectedBoneRow())
            curveCanvas.requestPaint()
        }
        function onKeyframeTicksChanged() { curveCanvas.requestPaint() }
        function onBoneListChanged()      {
            curveCanvas.refreshChannelValues(root.selectedBoneRow())
            curveCanvas.requestPaint()
        }
    }

    Connections {
        target: CurveEditModel
        function onModelChanged() { curveCanvas.requestPaint() }
    }

    // ── Empty-state placeholder ──────────────────────────────────────────────
    Text {
        anchors.centerIn: parent
        visible: !AnimationControlController.hasAnimation || !root.selectedBone
        text: !AnimationControlController.hasAnimation
              ? "Select a rigged mesh and an animation."
              : "Select a bone in the Animation Control or Dope Sheet."
        color: AnimationControlController.disabledTextColor
        font.pixelSize: 12
    }

    // ── Header ───────────────────────────────────────────────────────────────
    Rectangle {
        id: header
        width: parent.width; height: 24
        color: AnimationControlController.headerColor
        border.color: AnimationControlController.borderColor
        visible: AnimationControlController.hasAnimation && root.selectedBone

        Text {
            anchors.left: parent.left; anchors.leftMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            text: "Curves — " + root.selectedBone
            font.bold: true; font.pixelSize: 11
            color: AnimationControlController.textColor
        }

        // Channel legend
        Row {
            anchors.right: parent.right; anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            spacing: 10
            Repeater {
                model: root.activeChannelsForSelected()
                Row {
                    spacing: 4
                    Rectangle {
                        width: 10; height: 10; radius: 2
                        color: modelData.color
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        text: modelData.label
                        color: AnimationControlController.textColor
                        font.pixelSize: 10
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
            }
        }
    }

    // ── Curve canvas ─────────────────────────────────────────────────────────
    Canvas {
        id: curveCanvas
        anchors.left: parent.left
        anchors.top: header.visible ? header.bottom : parent.top
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        visible: header.visible

        // Cache per-channel values keyed by channel id. Refreshed whenever
        // the controller emits boneRowsChanged.
        property var channelValues: ({})

        function refreshChannelValues(boneRow) {
            var cache = {}
            var chans = root.activeChannelsFor(boneRow)
            for (var i = 0; i < chans.length; i++) {
                cache[chans[i].id] = AnimationControlController.channelValuesAt(
                    boneRow.bone, chans[i].id)
            }
            curveCanvas.channelValues = cache
        }

        function valueAtTimeForChannel(boneRow, channelId, time) {
            var values = channelValues[channelId] || []
            return CurveEditModel.evaluate(
                AnimationControlController.selectedEntityName,
                AnimationControlController.selectedAnimation,
                boneRow.bone, channelId, time,
                boneRow.keyTimes,
                values
            )
        }

        onPaint: {
            var ctx = getContext("2d"); ctx.clearRect(0, 0, width, height)
            var row = root.selectedBoneRow()
            if (!row) return
            var maxT = AnimationControlController.animationLength
            if (maxT <= 0) return

            // Time-axis ruler at the bottom
            ctx.strokeStyle = AnimationControlController.borderColor
            ctx.fillStyle   = AnimationControlController.textColor
            ctx.font        = "10px sans-serif"; ctx.lineWidth = 1
            var step = root.pxPerSec >= 100 ? 0.25 : (root.pxPerSec >= 40 ? 1.0 : 5.0)
            for (var t = 0; t <= maxT; t += step) {
                var x = (t - root.viewStart) * root.pxPerSec
                ctx.beginPath(); ctx.moveTo(x, height - 12); ctx.lineTo(x, height); ctx.stroke()
                ctx.fillText(t.toFixed(2) + "s", x + 2, height - 14)
            }

            // Horizontal value-axis grid lines (every 0.5 in value units)
            var midY = (height - 16) / 2
            ctx.strokeStyle = AnimationControlController.borderColor
            ctx.globalAlpha = 0.25
            for (var v = -2; v <= 2; v += 0.5) {
                var y = midY - (v - root.yCenter) * root.yScale
                if (y < 0 || y > height - 16) continue
                ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(width, y); ctx.stroke()
                ctx.fillText(v.toFixed(1), 4, y - 2)
            }
            ctx.globalAlpha = 1.0

            // Per-channel curve. evaluate() reads real per-channel values
            // pulled from the controller and cached in channelValues, so the
            // curve reflects the underlying TransformKeyFrame data.
            var chans = root.activeChannelsForSelected()
            for (var c = 0; c < chans.length; c++) {
                var ch = chans[c]
                ctx.strokeStyle = ch.color; ctx.lineWidth = 2
                ctx.beginPath()
                var first = true
                var samples = 200
                for (var s = 0; s < samples; s++) {
                    var u = s / (samples - 1)
                    var time = u * maxT
                    var val = curveCanvas.valueAtTimeForChannel(row, ch.id, time)
                    var x2 = (time - root.viewStart) * root.pxPerSec
                    var y2 = midY - (val - root.yCenter) * root.yScale
                    if (first) { ctx.moveTo(x2, y2); first = false }
                    else       { ctx.lineTo(x2, y2) }
                }
                ctx.stroke()

                // Keyframe squares + tangent handle stubs (drawn only for
                // visual reference in D3a — non-interactive).
                ctx.fillStyle = ch.color
                for (var k = 0; k < row.keyTimes.length; k++) {
                    var kt = row.keyTimes[k]
                    var kv = curveCanvas.valueAtTimeForChannel(row, ch.id, kt)
                    var kx = (kt - root.viewStart) * root.pxPerSec
                    var ky = midY - (kv - root.yCenter) * root.yScale
                    ctx.fillRect(kx - 4, ky - 4, 8, 8)

                    var tdata = CurveEditModel.tangentsAt(
                        AnimationControlController.selectedEntityName,
                        AnimationControlController.selectedAnimation,
                        row.bone, ch.id, kt)
                    if (tdata && tdata.length >= 2) {
                        var inT  = tdata[0]
                        var outT = tdata[1]
                        ctx.strokeStyle = ch.color; ctx.lineWidth = 1
                        ctx.globalAlpha = 0.6
                        // Draw a short handle in each direction
                        var handlePx = 30
                        ctx.beginPath()
                        ctx.moveTo(kx - handlePx, ky + inT * handlePx * 0.5)
                        ctx.lineTo(kx, ky)
                        ctx.lineTo(kx + handlePx, ky - outT * handlePx * 0.5)
                        ctx.stroke()
                        ctx.globalAlpha = 1.0
                    }
                }
            }
        }
    }

    // ── Interp-mode picker (right-click on keyframe square) ────────────────
    Menu {
        id: modeMenu
        property string boneName: ""
        property string channelId: ""
        property real keyTime: 0

        function applyMode(mode) {
            CurveEditModel.setMode(
                AnimationControlController.selectedEntityName,
                AnimationControlController.selectedAnimation,
                boneName, channelId, keyTime, mode)
        }

        MenuItem { text: "Bezier";   onTriggered: modeMenu.applyMode(CurveEditModel.ModeBezier) }
        MenuItem { text: "Linear";   onTriggered: modeMenu.applyMode(CurveEditModel.ModeLinear) }
        MenuItem { text: "Stepped";  onTriggered: modeMenu.applyMode(CurveEditModel.ModeStepped) }
        MenuItem { text: "Auto";     onTriggered: modeMenu.applyMode(CurveEditModel.ModeAuto) }
    }

    // Look up a keyframe near (px, py) and return { bone, channel, time, x, y }
    // or null if the click missed everything. Used by the right-click handler
    // to decide whether to open the mode picker.
    function pickKeyframeAt(px, py) {
        var row = selectedBoneRow()
        if (!row) return null
        var midY = (curveCanvas.height - 16) / 2
        var chans = activeChannelsFor(row)
        for (var c = 0; c < chans.length; c++) {
            var ch = chans[c]
            for (var k = 0; k < row.keyTimes.length; k++) {
                var kt = row.keyTimes[k]
                var kv = curveCanvas.valueAtTimeForChannel(row, ch.id, kt)
                var kx = (kt - viewStart) * pxPerSec
                var ky = midY - (kv - yCenter) * yScale
                if (Math.abs(px - kx) < 8 && Math.abs(py - ky) < 8) {
                    return { bone: row.bone, channel: ch.id,
                             time: kt, x: kx, y: ky }
                }
            }
        }
        return null
    }

    // Wheel = zoom horizontally (Ctrl/Cmd) or vertically (Shift), pan with
    // middle-drag. Right-click on a keyframe square opens the interp-mode
    // picker. Matches the dope sheet's input vocabulary as closely as
    // possible to keep mental load low when switching panels.
    MouseArea {
        id: panArea
        anchors.fill: parent
        acceptedButtons: Qt.MiddleButton | Qt.RightButton
        property real panStartX: 0
        property real panStartView: 0
        onPressed: function(mouse) {
            if (mouse.button === Qt.MiddleButton) {
                panStartX = mouse.x; panStartView = root.viewStart
                mouse.accepted = true
            } else if (mouse.button === Qt.RightButton) {
                var hit = root.pickKeyframeAt(
                    mouse.x - curveCanvas.x, mouse.y - curveCanvas.y)
                if (hit) {
                    modeMenu.boneName  = hit.bone
                    modeMenu.channelId = hit.channel
                    modeMenu.keyTime   = hit.time
                    modeMenu.popup()
                    mouse.accepted = true
                } else {
                    mouse.accepted = false
                }
            } else mouse.accepted = false
        }
        onPositionChanged: function(mouse) {
            if (!pressed || mouse.button !== Qt.MiddleButton) return
            var dx = mouse.x - panStartX
            root.viewStart = panStartView - dx / root.pxPerSec
            if (root.viewStart < 0) root.viewStart = 0
            curveCanvas.requestPaint()
        }
    }
    WheelHandler {
        target: null
        acceptedModifiers: Qt.ControlModifier | Qt.MetaModifier
        onWheel: function(event) {
            var factor = event.angleDelta.y > 0 ? 1.15 : (1.0 / 1.15)
            var newPx = Math.max(20, Math.min(2000, root.pxPerSec * factor))
            if (newPx === root.pxPerSec) return
            var tCursor = root.viewStart + event.point.position.x / root.pxPerSec
            root.pxPerSec = newPx
            root.viewStart = tCursor - event.point.position.x / newPx
            if (root.viewStart < 0) root.viewStart = 0
            curveCanvas.requestPaint()
        }
    }
}
