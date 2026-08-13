import QtQuick
import QtQuick.Controls
import AnimationControl 1.0
import PropertiesPanel 1.0

// Curve editor — per-channel animation curves with Bezier handles.
//
// Edits skeletal BONE tracks by default; when the selected animation is a
// SceneManager-level NODE-transform clip (#520) it sources its rows and
// routes every value read/write through NodeAnimationManager instead, so a
// node clip's TRS curves edit IDENTICALLY (quaternion-correct rotation,
// undoable, live-drag preview). The bone path is byte-for-byte unchanged
// when `isNodeClip` is false.
Rectangle {
    id: root
    color: AnimationControlController.panelColor
    focus: true

    // True when the selected animation is a node-transform clip rather than
    // a skeletal one. When true, `rows` come from NodeAnimationManager and
    // the "selected row" is the animated node (keyed by node name). (#520)
    readonly property bool isNodeClip: AnimationControlController.selectedIsNodeClip
    // The node a node-clip's curves belong to. A node clip animates the
    // SceneNode whose name == the selected entity name (see
    // AnimationControlController::setAnimation node-clip branch). (#520)
    readonly property string nodeClipName: AnimationControlController.selectedAnimation
    readonly property string nodeName: AnimationControlController.selectedEntityName

    property real pxPerSec: 200
    property real viewStart: 0.0
    property real yScale: 60   // px per unit value
    property real yCenter: 0   // value at the vertical center of the canvas

    property int leftStripWidth: 130
    property var hiddenChannels: ({})

    // Row model: bone rows from the controller, OR node-clip rows from
    // NodeAnimationManager (normalised so each carries a `bone` field ==
    // the node name — every downstream reference reads row.bone, so the
    // draw / pick / drag code needs no node-specific branch). (#520)
    property var rows: root.fetchRows()

    function fetchRows() {
        if (isNodeClip) {
            if (nodeClipName.length === 0) return []
            var nrows = NodeAnimationManager.nodeRows(nodeClipName)
            var out = []
            for (var i = 0; i < nrows.length; i++) {
                // Alias `node` → `bone` so selectedBoneRow()/onPaint/pick
                // all keep reading `.bone` unchanged.
                out.push({ bone: nrows[i].node,
                           keyTimes: nrows[i].keyTimes,
                           channels: nrows[i].channels })
            }
            return out
        }
        return AnimationControlController.allBoneRows()
    }

    // For a node clip the "selected bone" is the animated node; there is
    // typically one node per clip, so default to it. Falls back to the
    // controller's selected bone for the skeletal path. (#520)
    readonly property string selectedBone: isNodeClip
                                           ? root.nodeName
                                           : AnimationControlController.selectedBone

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

    function isChannelVisible(channelId) {
        return hiddenChannels[channelId] !== true
    }

    function visibleChannelsFor(row) {
        var active = activeChannelsFor(row)
        var result = []
        for (var i = 0; i < active.length; i++) {
            if (isChannelVisible(active[i].id)) result.push(active[i])
        }
        return result
    }

    function visibleChannelsForSelected() {
        return visibleChannelsFor(selectedBoneRow())
    }

    function toggleChannelVisibility(channelId) {
        var nextHidden = {}
        for (var key in hiddenChannels) nextHidden[key] = hiddenChannels[key]
        if (nextHidden[channelId] === true) delete nextHidden[channelId]
        else nextHidden[channelId] = true
        hiddenChannels = nextHidden
        if (panArea.dragChannel === channelId) panArea.dragMode = ""
        curveCanvas.requestPaint()
    }

    // anchorPx is the pixel that should stay fixed while zooming.
    function zoomHorizontal(factor, anchorPx) {
        var newPx = Math.max(20, Math.min(2000, root.pxPerSec * factor))
        if (newPx === root.pxPerSec) return
        var tCursor = root.viewStart + anchorPx / root.pxPerSec
        root.pxPerSec = newPx
        root.viewStart = Math.max(0, tCursor - anchorPx / newPx)
        clampViewStart()
        curveCanvas.requestPaint()
    }

    function zoomVertical(factor, anchorPy) {
        var newScale = Math.max(8, Math.min(800, root.yScale * factor))
        if (newScale === root.yScale) return
        var midY = (curveCanvas.height - 16) / 2
        var vCursor = root.yCenter - (anchorPy - midY) / root.yScale
        root.yScale = newScale
        root.yCenter = vCursor + (anchorPy - midY) / newScale
        curveCanvas.requestPaint()
    }

    function fitToView() {
        var maxT = AnimationControlController.animationLength
        if (maxT <= 0 || curveCanvas.width <= 0) return
        var pad = 0.05
        root.pxPerSec = Math.max(20, curveCanvas.width / (maxT * (1.0 + pad)))
        root.viewStart = 0
        curveCanvas.requestPaint()
    }

    function clampViewStart() {
        var maxT = AnimationControlController.animationLength
        if (maxT <= 0) { root.viewStart = 0; return }
        // Match ScrollBar's scrollable range so panning can't drift past
        // the thumb's end position.
        var visibleSecs = curveCanvas.width / root.pxPerSec
        var maxStart = Math.max(0, maxT - visibleSecs)
        if (root.viewStart > maxStart) root.viewStart = maxStart
        if (root.viewStart < 0) root.viewStart = 0
    }

    // Re-pull rows + values from whichever source is current (bone or
    // node) and repaint. Centralised so every refresh trigger stays in
    // sync with the bone-vs-node branch. (#520)
    function reloadAndRepaint() {
        root.rows = root.fetchRows()
        curveCanvas.refreshChannelValues(root.selectedBoneRow())
        curveCanvas.requestPaint()
    }

    // A change of clip type (skeletal ↔ node) must re-source the rows.
    onIsNodeClipChanged: reloadAndRepaint()
    onNodeClipNameChanged: if (isNodeClip) reloadAndRepaint()

    Connections {
        target: AnimationControlController
        function onBoneRowsChanged()      { root.reloadAndRepaint() }
        function onSelectionChanged()     { root.reloadAndRepaint() }
        function onKeyframeTicksChanged() { curveCanvas.requestPaint() }
        function onBoneListChanged()      {
            curveCanvas.refreshChannelValues(root.selectedBoneRow())
            curveCanvas.requestPaint()
        }
    }

    // #520: node-clip keyframe/clip changes (add/move/delete, undo/redo)
    // fire on NodeAnimationManager, not the controller — refresh on them so
    // node curves stay live the same way bone curves do.
    Connections {
        target: NodeAnimationManager
        function onKeyframesChanged(clipName) {
            if (root.isNodeClip) root.reloadAndRepaint()
        }
        function onActiveClipChanged() {
            if (root.isNodeClip) root.reloadAndRepaint()
        }
    }

    Connections {
        target: CurveEditModel
        function onModelChanged() { curveCanvas.requestPaint() }
    }

    Text {
        anchors.centerIn: parent
        visible: !AnimationControlController.hasAnimation || !root.selectedBone
        text: !AnimationControlController.hasAnimation
              ? "Select a rigged mesh and an animation."
              : "Select a bone in the Animation Control or Dope Sheet."
        color: AnimationControlController.disabledTextColor
        font.pixelSize: 12
    }

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

        Row {
            anchors.right: parent.right; anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            spacing: 10

            Row {
                spacing: 2
                anchors.verticalCenter: parent.verticalCenter
                Text {
                    text: "H:"
                    color: AnimationControlController.textColor
                    font.pixelSize: 10
                    anchors.verticalCenter: parent.verticalCenter
                }
                Button {
                    text: "−"; width: 22; height: 18
                    font.pixelSize: 12
                    onClicked: root.zoomHorizontal(1.0 / 1.25, curveCanvas.width / 2)
                    ToolTip.visible: hovered
                    ToolTip.text: "Zoom out time axis (wheel)"
                }
                Button {
                    text: "+"; width: 22; height: 18
                    font.pixelSize: 12
                    onClicked: root.zoomHorizontal(1.25, curveCanvas.width / 2)
                    ToolTip.visible: hovered
                    ToolTip.text: "Zoom in time axis (wheel)"
                }
                Button {
                    text: "⤢"; width: 22; height: 18
                    font.pixelSize: 11
                    onClicked: root.fitToView()
                    ToolTip.visible: hovered
                    ToolTip.text: "Fit animation to view"
                }
            }
            Row {
                spacing: 2
                anchors.verticalCenter: parent.verticalCenter
                Text {
                    text: "V:"
                    color: AnimationControlController.textColor
                    font.pixelSize: 10
                    anchors.verticalCenter: parent.verticalCenter
                }
                Button {
                    text: "−"; width: 22; height: 18
                    font.pixelSize: 12
                    onClicked: root.zoomVertical(1.0 / 1.25, curveCanvas.height / 2)
                    ToolTip.visible: hovered
                    ToolTip.text: "Zoom out value axis (Shift+wheel)"
                }
                Button {
                    text: "+"; width: 22; height: 18
                    font.pixelSize: 12
                    onClicked: root.zoomVertical(1.25, curveCanvas.height / 2)
                    ToolTip.visible: hovered
                    ToolTip.text: "Zoom in value axis (Shift+wheel)"
                }
            }

            // Bake = explicit resample of every segment on the active
            // bone/channel into dense TransformKeyFrames. ThemedComboBox
            // matches the inspector's other dropdowns. The "Bake…"
            // header stays visible (we never select an entry — selecting
            // any item triggers the action, then we reset the index).
            ThemedComboBox {
                id: bakeCombo
                width: 100; height: 22
                anchors.verticalCenter: parent.verticalCenter
                enabled: root.selectedBone !== ""
                font.pixelSize: 10
                model: [
                    "Bake…",
                    "Sparse",
                    "Medium",
                    "Dense",
                    "Set to 10 FPS",
                    "Set to 15 FPS",
                    "Set to 30 FPS",
                    "Set to 60 FPS"
                ]
                ToolTip.visible: hovered
                ToolTip.text: "Resample curves into keyframes"

                function bake(density) {
                    var row = root.selectedBoneRow()
                    if (!row || !row.channels) return
                    for (var i = 0; i < root.channelOrder.length; i++) {
                        var ch = root.channelOrder[i]
                        if (!row.channels[ch.id]) continue
                        // Node clips resample through NodeAnimationManager
                        // (SceneManager-owned track); bones through the
                        // controller (skeleton track). Same density levels,
                        // same undoable ResampleCurveCommand under the hood. (#520)
                        if (root.isNodeClip) {
                            NodeAnimationManager.resampleAllNodeSegments(
                                root.nodeClipName, root.nodeName, ch.id, density)
                        } else {
                            AnimationControlController.resampleAllSegmentsForBone(
                                root.selectedBone, ch.id, density)
                        }
                    }
                }

                onActivated: function(index) {
                    // Density int passes through to the controller:
                    // 0 Sparse / 1 Medium / 2 Dense / 3-6 = 10/15/30/60 FPS exact.
                    // Use `< model.length` instead of a hand-counted
                    // upper bound so adding/removing entries can't
                    // silently drop the last action again.
                    if (index >= 1 && index < model.length) bake(index - 1)
                    // Snap back to the header label so the combo always
                    // shows "Bake…" — these entries are actions, not
                    // a persistent selection.
                    currentIndex = 0
                }
            }

            Repeater {
                model: root.activeChannelsForSelected()
                Rectangle {
                    width: tagRow.implicitWidth + 8
                    height: 18
                    radius: 4
                    color: "transparent"
                    border.width: 1
                    border.color: root.isChannelVisible(modelData.id)
                                  ? modelData.color
                                  : AnimationControlController.borderColor
                    opacity: root.isChannelVisible(modelData.id) ? 1.0 : 0.45
                    anchors.verticalCenter: parent.verticalCenter

                    Row {
                        id: tagRow
                        anchors.centerIn: parent
                        spacing: 4

                        Rectangle {
                            width: 10; height: 10; radius: 2
                            color: root.isChannelVisible(modelData.id)
                                   ? modelData.color
                                   : AnimationControlController.borderColor
                            anchors.verticalCenter: parent.verticalCenter
                        }

                        Text {
                            text: modelData.label
                            color: AnimationControlController.textColor
                            font.pixelSize: 10
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton
                        cursorShape: Qt.PointingHandCursor
                        hoverEnabled: true
                        onClicked: root.toggleChannelVisibility(modelData.id)
                        ToolTip.visible: containsMouse
                        ToolTip.text: root.isChannelVisible(modelData.id)
                                      ? "Click to hide " + modelData.label
                                      : "Click to show " + modelData.label
                    }
                }
            }
        }
    }

    Canvas {
        id: curveCanvas
        anchors.left: parent.left
        anchors.top: header.visible ? header.bottom : parent.top
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 14   // matches hScrollBar.height
        visible: header.visible

        property var channelValues: ({})

        function refreshChannelValues(boneRow) {
            var cache = {}
            if (!boneRow) { curveCanvas.channelValues = cache; return }
            var chans = root.activeChannelsFor(boneRow)
            for (var i = 0; i < chans.length; i++) {
                // #520: node clips read per-channel values from
                // NodeAnimationManager; bones from the controller. Same
                // (time-ordered) shape, so the rest of the canvas is agnostic.
                cache[chans[i].id] = root.isNodeClip
                    ? NodeAnimationManager.nodeChannelValuesAt(
                          root.nodeClipName, boneRow.bone, chans[i].id)
                    : AnimationControlController.channelValuesAt(
                          boneRow.bone, chans[i].id)
            }
            curveCanvas.channelValues = cache
        }

        function valueAtTimeForChannel(boneRow, channelId, time) {
            var values = channelValues[channelId] || []
            // CurveEditModel is track-agnostic (#520): it only does curve math
            // on the times/values passed in, keyed by (skeleton,anim,bone,
            // channel) for its tangent side-table. Passing the node clip name
            // as the anim key + the node name as the bone key gives node
            // curves their own independent tangent state.
            return CurveEditModel.evaluate(
                AnimationControlController.selectedEntityName,
                AnimationControlController.selectedAnimation,
                boneRow.bone, channelId, time,
                boneRow.keyTimes,
                values
            )
        }

        Component.onCompleted: refreshChannelValues(root.selectedBoneRow())

        onPaint: {
            var ctx = getContext("2d"); ctx.clearRect(0, 0, width, height)
            var row = root.selectedBoneRow()
            if (!row) return
            var maxT = AnimationControlController.animationLength
            if (maxT <= 0) return

            ctx.strokeStyle = AnimationControlController.borderColor
            ctx.fillStyle   = AnimationControlController.textColor
            ctx.font        = "10px sans-serif"; ctx.lineWidth = 1
            var step = root.pxPerSec >= 100 ? 0.25 : (root.pxPerSec >= 40 ? 1.0 : 5.0)
            for (var t = 0; t <= maxT; t += step) {
                var x = (t - root.viewStart) * root.pxPerSec
                ctx.beginPath(); ctx.moveTo(x, height - 12); ctx.lineTo(x, height); ctx.stroke()
                ctx.fillText(t.toFixed(2) + "s", x + 2, height - 14)
            }

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

            var chans = root.visibleChannelsForSelected()
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

    Menu {
        id: modeMenu
        property string boneName: ""
        property string channelId: ""
        property real keyTime: 0

        function applyMode(mode) {
            // Updates the CurveEditModel side-table mode + tangents.
            // Doesn't touch Ogre's per-animation interp (that's a
            // global setting and would distort other bones); the
            // canvas reflects the new shape but viewport playback
            // still uses the existing TransformKeyFrames until the
            // user clicks Bake to commit a resample explicitly.
            var prev = CurveEditModel.tangentsAt(
                AnimationControlController.selectedEntityName,
                AnimationControlController.selectedAnimation,
                boneName, channelId, keyTime)
            var inT  = prev.length >= 1 ? prev[0] : 0
            var outT = prev.length >= 2 ? prev[1] : 0
            AnimationControlController.setCurveHandle(
                boneName, channelId, keyTime, inT, outT, mode)
        }

        MenuItem { text: "Bezier";   onTriggered: modeMenu.applyMode(CurveEditModel.ModeBezier) }
        MenuItem { text: "Linear";   onTriggered: modeMenu.applyMode(CurveEditModel.ModeLinear) }
        MenuItem { text: "Stepped";  onTriggered: modeMenu.applyMode(CurveEditModel.ModeStepped) }
        MenuItem { text: "Auto";     onTriggered: modeMenu.applyMode(CurveEditModel.ModeAuto) }
    }

    function pickKeyframeAt(px, py) {
        var row = selectedBoneRow()
        if (!row) return null
        var midY = (curveCanvas.height - 16) / 2
        var chans = visibleChannelsFor(row)
        for (var c = 0; c < chans.length; c++) {
            var ch = chans[c]
            for (var k = 0; k < row.keyTimes.length; k++) {
                var kt = row.keyTimes[k]
                var kv = curveCanvas.valueAtTimeForChannel(row, ch.id, kt)
                var kx = (kt - viewStart) * pxPerSec
                var ky = midY - (kv - yCenter) * yScale
                if (Math.abs(px - kx) < 8 && Math.abs(py - ky) < 8) {
                    return { bone: row.bone, channel: ch.id,
                             time: kt, value: kv, x: kx, y: ky }
                }
            }
        }
        return null
    }

    function pickTangentHandleAt(px, py) {
        var row = selectedBoneRow()
        if (!row) return null
        var midY = (curveCanvas.height - 16) / 2
        var chans = visibleChannelsFor(row)
        var handlePx = 30
        for (var c = 0; c < chans.length; c++) {
            var ch = chans[c]
            for (var k = 0; k < row.keyTimes.length; k++) {
                var kt = row.keyTimes[k]
                var kv = curveCanvas.valueAtTimeForChannel(row, ch.id, kt)
                var kx = (kt - viewStart) * pxPerSec
                var ky = midY - (kv - yCenter) * yScale
                var tdata = CurveEditModel.tangentsAt(
                    AnimationControlController.selectedEntityName,
                    AnimationControlController.selectedAnimation,
                    row.bone, ch.id, kt)
                if (!tdata || tdata.length < 2) continue
                var inT  = tdata[0]
                var outT = tdata[1]
                var inHx  = kx - handlePx, inHy  = ky + inT * handlePx * 0.5
                var outHx = kx + handlePx, outHy = ky - outT * handlePx * 0.5
                if (Math.abs(px - inHx) < 6 && Math.abs(py - inHy) < 6) {
                    return { bone: row.bone, channel: ch.id, time: kt,
                             side: "in", inT: inT, outT: outT, kx: kx, ky: ky }
                }
                if (Math.abs(px - outHx) < 6 && Math.abs(py - outHy) < 6) {
                    return { bone: row.bone, channel: ch.id, time: kt,
                             side: "out", inT: inT, outT: outT, kx: kx, ky: ky }
                }
            }
        }
        return null
    }

    MouseArea {
        id: panArea
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
        property real panStartX: 0
        property real panStartView: 0

        // dragMode: "" / "keyframe" / "tangent"
        property string dragMode: ""
        property string dragBone: ""
        property string dragChannel: ""
        property real   dragKeyTime: 0
        property real   dragOriginalKeyTime: 0
        property real   dragOriginalValue: 0
        property real   dragLastValue: 0
        property real   dragPressX: 0
        property real   dragPressY: 0
        property string dragTangentSide: ""
        property real   dragTangentKx: 0
        property real   dragTangentKy: 0
        property real   dragInT: 0
        property real   dragOutT: 0
        // CurveEditModel state at tangent-drag press, used by the
        // CurveEditModelChangeCommand pushed on release.
        property real   dragTangentOrigInT: 0
        property real   dragTangentOrigOutT: 0

        onPressed: function(mouse) {
            if (mouse.button === Qt.MiddleButton) {
                panStartX = mouse.x; panStartView = root.viewStart
                mouse.accepted = true
            } else if (mouse.button === Qt.RightButton) {
                var rhit = root.pickKeyframeAt(
                    mouse.x - curveCanvas.x, mouse.y - curveCanvas.y)
                if (rhit) {
                    modeMenu.boneName    = rhit.bone
                    modeMenu.channelId   = rhit.channel
                    modeMenu.keyTime     = rhit.time
                    modeMenu.popup()
                    mouse.accepted = true
                } else {
                    mouse.accepted = false
                }
            } else if (mouse.button === Qt.LeftButton) {
                // Try tangent handle (smaller target) first, then keyframe square.
                var canvasX = mouse.x - curveCanvas.x
                var canvasY = mouse.y - curveCanvas.y
                var thit = root.pickTangentHandleAt(canvasX, canvasY)
                if (thit) {
                    panArea.dragMode = "tangent"
                    panArea.dragBone = thit.bone
                    panArea.dragChannel = thit.channel
                    panArea.dragKeyTime = thit.time
                    panArea.dragTangentSide = thit.side
                    panArea.dragTangentKx = thit.kx
                    panArea.dragTangentKy = thit.ky
                    panArea.dragInT = thit.inT
                    panArea.dragOutT = thit.outT
                    // Capture pre-drag CurveEditModel state for the
                    // release-time undo entry. Mode comes through as
                    // a third element of tangentsAt.
                    panArea.dragTangentOrigInT  = thit.inT
                    panArea.dragTangentOrigOutT = thit.outT
                    mouse.accepted = true
                    return
                }
                var khit = root.pickKeyframeAt(canvasX, canvasY)
                if (khit) {
                    panArea.dragMode = "keyframe"
                    panArea.dragBone = khit.bone
                    panArea.dragChannel = khit.channel
                    panArea.dragKeyTime = khit.time
                    panArea.dragOriginalKeyTime = khit.time
                    panArea.dragOriginalValue = khit.value
                    // Seed lastValue too — onReleased compares it to
                    // originalValue to decide whether to commit. Without
                    // this, a click without drag (or a Shift-X-locked
                    // drag) would commit a stale `0` back to the curve.
                    panArea.dragLastValue = khit.value
                    panArea.dragPressX = canvasX
                    panArea.dragPressY = canvasY
                    mouse.accepted = true
                    return
                }
                mouse.accepted = false
            } else mouse.accepted = false
        }
        onPositionChanged: function(mouse) {
            if (!pressed) return

            if (mouse.buttons & Qt.MiddleButton) {
                var dx = mouse.x - panStartX
                root.viewStart = panStartView - dx / root.pxPerSec
                root.clampViewStart()
                curveCanvas.requestPaint()
                return
            }

            if (!(mouse.buttons & Qt.LeftButton) || panArea.dragMode === "") return
            var canvasX = mouse.x - curveCanvas.x
            var canvasY = mouse.y - curveCanvas.y

            if (panArea.dragMode === "keyframe") {
                // Shift locks to the dominant axis since press.
                var midY = (curveCanvas.height - 16) / 2
                var newValue = root.yCenter - (canvasY - midY) / root.yScale
                var newTime  = root.viewStart + canvasX / root.pxPerSec
                if (newTime < 0) newTime = 0

                var dxAxis = Math.abs(canvasX - panArea.dragPressX)
                var dyAxis = Math.abs(canvasY - panArea.dragPressY)
                var shift = (mouse.modifiers & Qt.ShiftModifier) !== 0
                // Shift constrains to the dominant axis. writeValue/writeTime
                // are gated so axis-locked drags don't smear into the orthogonal
                // channel.
                var writeValue = !shift || dyAxis >= dxAxis
                var writeTime  = !shift || dxAxis >  dyAxis

                // Preview API skips the undo stack — pushing a command
                // per move fires MainWindow's indexChanged handler,
                // which calls Skeleton::reset(true) and snaps the bone
                // to T-pose between events. Commit on release.
                if (writeValue) {
                    panArea.dragLastValue = newValue
                    // #520: node clips preview through NodeAnimationManager
                    // (no undo push — committed on release); bones through
                    // the controller. Both leave the undo stack untouched
                    // during the drag.
                    if (root.isNodeClip) {
                        NodeAnimationManager.setNodeKeyframeValuePreview(
                            root.nodeClipName, panArea.dragBone,
                            panArea.dragChannel, panArea.dragKeyTime, newValue)
                    } else {
                        AnimationControlController.setKeyframeValuePreview(
                            panArea.dragBone, panArea.dragChannel,
                            panArea.dragKeyTime, newValue)
                    }
                }
                if (writeTime) {
                    if (root.isNodeClip) {
                        // NodeAnimationManager has no "preview" (non-undoable)
                        // move — moveNodeKeyframe pushes a command. Pushing one
                        // per mouse move would spam the undo stack, so during
                        // the drag we only TRACK the intended time and commit a
                        // single moveNodeKeyframe on release (see onReleased).
                        panArea.dragKeyTime = newTime
                    } else {
                        var ok = AnimationControlController.moveKeyframePreview(
                            panArea.dragBone, panArea.dragKeyTime, newTime)
                        if (ok) panArea.dragKeyTime = newTime
                    }
                }
                // boneRowsChanged is suppressed by the preview API; refresh
                // inline. fetchRows() picks the bone-vs-node source. (#520)
                root.rows = root.fetchRows()
                curveCanvas.refreshChannelValues(root.selectedBoneRow())
                curveCanvas.requestPaint()
            } else if (panArea.dragMode === "tangent") {
                // Mirrors the encoding in onPaint.
                var handlePx = 30
                if (panArea.dragTangentSide === "in") {
                    var newInT = (canvasY - panArea.dragTangentKy) / (handlePx * 0.5)
                    panArea.dragInT = newInT
                    CurveEditModel.setTangents(
                        AnimationControlController.selectedEntityName,
                        AnimationControlController.selectedAnimation,
                        panArea.dragBone, panArea.dragChannel,
                        panArea.dragKeyTime, newInT, panArea.dragOutT)
                } else {
                    var newOutT = -(canvasY - panArea.dragTangentKy) / (handlePx * 0.5)
                    panArea.dragOutT = newOutT
                    CurveEditModel.setTangents(
                        AnimationControlController.selectedEntityName,
                        AnimationControlController.selectedAnimation,
                        panArea.dragBone, panArea.dragChannel,
                        panArea.dragKeyTime, panArea.dragInT, newOutT)
                }
                curveCanvas.requestPaint()
            }
        }
        onReleased: function(mouse) {
            if (panArea.dragMode === "keyframe") {
                var valueChanged = panArea.dragLastValue !== panArea.dragOriginalValue
                var timeChanged  = panArea.dragKeyTime    !== panArea.dragOriginalKeyTime

                if (root.isNodeClip) {
                    // #520: node path. Value was previewed (non-undoable);
                    // time was only TRACKED during the drag (dragKeyTime holds
                    // the desired new time — no live move happened). Restore the
                    // previewed value to the ORIGINAL first so the command's
                    // redo snapshots the right prior TRS, then commit.
                    //
                    // Order matters: re-time first (moveNodeKeyframe reads
                    // dragOriginalKeyTime), then set the value at the new time.
                    if (timeChanged) {
                        NodeAnimationManager.setNodeKeyframeValuePreview(
                            root.nodeClipName, panArea.dragBone,
                            panArea.dragChannel, panArea.dragOriginalKeyTime,
                            panArea.dragOriginalValue)
                        NodeAnimationManager.moveNodeKeyframe(
                            root.nodeClipName, panArea.dragBone,
                            panArea.dragOriginalKeyTime, panArea.dragKeyTime)
                    }
                    if (valueChanged) {
                        // Revert the live preview to the original value before
                        // committing so the undoable set snapshots correctly.
                        NodeAnimationManager.setNodeKeyframeValuePreview(
                            root.nodeClipName, panArea.dragBone,
                            panArea.dragChannel, panArea.dragKeyTime,
                            panArea.dragOriginalValue)
                        NodeAnimationManager.setNodeKeyframeValue(
                            root.nodeClipName, panArea.dragBone,
                            panArea.dragChannel, panArea.dragKeyTime,
                            panArea.dragLastValue)
                    }
                    // Refresh the local row/value caches (no boneRowsChanged
                    // fires for node edits).
                    root.rows = root.fetchRows()
                    curveCanvas.refreshChannelValues(root.selectedBoneRow())
                    curveCanvas.requestPaint()
                } else {
                // Restore originals before pushing the real commands so
                // their redo() captures the correct old-state snapshot.
                if (timeChanged) {
                    AnimationControlController.moveKeyframePreview(
                        panArea.dragBone,
                        panArea.dragKeyTime, panArea.dragOriginalKeyTime)
                    AnimationControlController.moveKeyframe(
                        panArea.dragBone,
                        panArea.dragOriginalKeyTime, panArea.dragKeyTime)
                }
                if (valueChanged) {
                    AnimationControlController.setKeyframeValuePreview(
                        panArea.dragBone, panArea.dragChannel,
                        panArea.dragKeyTime, panArea.dragOriginalValue)
                    AnimationControlController.setKeyframeValue(
                        panArea.dragBone, panArea.dragChannel,
                        panArea.dragKeyTime, panArea.dragLastValue)
                }
                // Value/time edits already pushed proper commands via
                // setKeyframeValue/moveKeyframe — Ogre's existing
                // interp draws the segment, no implicit resample.
                }
            } else if (panArea.dragMode === "tangent") {
                // Tangent drag wrote CurveEditModel directly during
                // move (no undo per move). On release, restore the
                // pre-drag tangents and push one undoable
                // CurveEditModelChangeCommand. Ogre's interp mode
                // (linear vs spline) is updated to match the new
                // shape, but no dense keyframes are inserted — the
                // user opts in via the Bake button when they want
                // exact curve fidelity baked into the track.
                CurveEditModel.setTangents(
                    AnimationControlController.selectedEntityName,
                    AnimationControlController.selectedAnimation,
                    panArea.dragBone, panArea.dragChannel,
                    panArea.dragKeyTime,
                    panArea.dragTangentOrigInT,
                    panArea.dragTangentOrigOutT)
                AnimationControlController.setCurveHandle(
                    panArea.dragBone, panArea.dragChannel,
                    panArea.dragKeyTime,
                    panArea.dragInT, panArea.dragOutT,
                    -1)
            }
            panArea.dragMode = ""
        }

        // Pointer handlers must be children of the MouseArea — at root
        // level the MouseArea blocks them.

        // Plain wheel / two-finger scroll = pan; horizontal swipe pans
        // time, vertical pans value. pixelDelta is preferred when set
        // so trackpad pan tracks finger motion 1:1.
        WheelHandler {
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            acceptedModifiers: Qt.NoModifier
            onWheel: function(event) {
                var pxX = event.pixelDelta.x !== 0 ? event.pixelDelta.x
                                                   : event.angleDelta.x / 8
                var pxY = event.pixelDelta.y !== 0 ? event.pixelDelta.y
                                                   : event.angleDelta.y / 8

                if (Math.abs(pxX) > Math.abs(pxY)) {
                    root.viewStart -= pxX / root.pxPerSec
                    root.clampViewStart()
                } else {
                    root.yCenter += pxY / root.yScale
                }
                curveCanvas.requestPaint()
            }
        }
        WheelHandler {
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            acceptedModifiers: Qt.ControlModifier | Qt.MetaModifier
            onWheel: function(event) {
                var dy = event.pixelDelta.y !== 0 ? event.pixelDelta.y
                                                  : event.angleDelta.y
                var factor = dy > 0 ? 1.15 : (1.0 / 1.15)
                root.zoomHorizontal(factor, event.point.position.x)
            }
        }
        WheelHandler {
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            acceptedModifiers: Qt.ShiftModifier
            onWheel: function(event) {
                var dy = event.pixelDelta.y !== 0 ? event.pixelDelta.y
                                                  : event.angleDelta.y
                var factor = dy > 0 ? 1.15 : (1.0 / 1.15)
                root.zoomVertical(factor, event.point.position.y)
            }
        }
    }

    // Drives root.viewStart while the user drags the thumb. Visible
    // only when the timeline overflows the canvas width.
    ScrollBar {
        id: hScrollBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 14
        active: true
        orientation: Qt.Horizontal
        policy: ScrollBar.AlwaysOn
        visible: header.visible
                 && AnimationControlController.animationLength > 0
                 && (AnimationControlController.animationLength * root.pxPerSec)
                    > curveCanvas.width

        readonly property real scrollableSecs: {
            var maxT = AnimationControlController.animationLength
            if (maxT <= 0 || curveCanvas.width <= 0) return 0
            var visibleSecs = curveCanvas.width / root.pxPerSec
            return Math.max(0, maxT - visibleSecs)
        }

        // Both directions of the position↔viewStart sync are imperative;
        // the `syncing` guard prevents a bounce.
        size: {
            var maxT = AnimationControlController.animationLength
            if (maxT <= 0 || curveCanvas.width <= 0) return 1
            var visibleSecs = curveCanvas.width / root.pxPerSec
            return Math.min(1, visibleSecs / maxT)
        }

        property bool syncing: false
        function syncFromViewStart() {
            var maxT = AnimationControlController.animationLength
            if (maxT <= 0) return
            syncing = true
            position = Math.max(0, Math.min(1 - size, root.viewStart / maxT))
            syncing = false
        }

        Component.onCompleted: syncFromViewStart()
        Connections {
            target: root
            function onViewStartChanged() {
                if (hScrollBar.pressed) return
                hScrollBar.syncFromViewStart()
            }
        }

        Connections {
            target: AnimationControlController
            function onAnimationLengthChanged() { hScrollBar.syncFromViewStart() }
        }

        onPositionChanged: {
            if (syncing) return
            var maxT = AnimationControlController.animationLength
            if (maxT <= 0) return
            root.viewStart = position * maxT
            curveCanvas.requestPaint()
        }
    }
}
