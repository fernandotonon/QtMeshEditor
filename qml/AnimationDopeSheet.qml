import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AnimationControl 1.0
import PropertiesPanel 1.0

// Multi-bone dope sheet with multi-select, bulk move, and copy/paste.
// One row per animated bone with diamond markers at each keyframe time.
// Wheel zooms around the cursor; middle-drag pans.
Rectangle {
    id: root
    color: AnimationControlController.panelColor
    focus: true

    // Pixels-per-second view scale and horizontal scroll offset (in seconds).
    property real pxPerSec: 200
    property real viewStart: 0.0

    property int leftStripWidth: 130
    property int rowHeight: 22

    // Cached row data refreshed from the controller.
    property var rows: AnimationControlController.allBoneRows()

    // Slice A5b: morph-target rows for the selected entity, sourced from
    // AnimationControlController.allMorphRows() (a Q_INVOKABLE on the
    // same singleton that supplies `allBoneRows()` — kept on
    // AnimationControlController on purpose so this file doesn't
    // need a second `import` and doesn't trigger a different QML
    // singleton chain during MainWindow construction). Each row is
    // `{ name, keyTimes }`. Renders below the bone rows as a fixed
    // read-only band — full selection / move / copy interaction for
    // morph tracks is a future slice.
    property var morphRows: AnimationControlController.allMorphRows()

    // Slice C (#517): scene-node transform-animation rows. Independent of
    // the selected entity — node clips are owned by the SceneManager, not
    // a mesh — so they track NodeAnimationManager.activeClip (the clip the
    // Inspector's "Node Transform Animation" section is editing). Each row
    // is `{ node, keyTimes }`. Renders as an interactive band below the
    // morph band with the same timeline math. Empty when no node clip is
    // active or the active clip has no tracks yet.
    property string nodeClip: NodeAnimationManager.activeClip
    property var nodeRows: NodeAnimationManager.activeClip.length > 0
                           ? NodeAnimationManager.nodeRows(NodeAnimationManager.activeClip)
                           : []

    // Per-bone expansion state for per-channel rows. Keys are bone names,
    // values are bool. Reset when a new clip is selected (different bones).
    property var expandedBones: ({})

    // Per-channel render order + colors. Empty rows are filtered by the
    // controller's `channels` flags so we never paint a sub-row for a
    // channel that doesn't deviate from bind pose.
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

    function activeChannelsFor(boneRow) {
        var result = []
        if (!boneRow || !boneRow.channels) return result
        for (var i = 0; i < channelOrder.length; i++) {
            var c = channelOrder[i]
            if (boneRow.channels[c.id]) result.push(c)
        }
        return result
    }

    function isExpanded(boneName) {
        return expandedBones[boneName] === true
    }

    function toggleExpanded(boneName) {
        var copy = {}
        for (var k in expandedBones) copy[k] = expandedBones[k]
        copy[boneName] = !copy[boneName]
        expandedBones = copy
    }

    // Selection state. Each entry is { bone: string, time: number }.
    // Stored as a plain array so QML bindings update on assignment.
    property var selection: []

    function isSelected(bone, time) {
        for (var i = 0; i < selection.length; i++) {
            if (selection[i].bone === bone &&
                Math.abs(selection[i].time - time) < 0.001) return true
        }
        return false
    }

    function clearSelection() {
        selection = []
    }

    function setSingleSelection(bone, time) {
        selection = [{ bone: bone, time: time }]
    }

    function toggleInSelection(bone, time) {
        var copy = selection.slice()
        for (var i = 0; i < copy.length; i++) {
            if (copy[i].bone === bone && Math.abs(copy[i].time - time) < 0.001) {
                copy.splice(i, 1)
                selection = copy
                return
            }
        }
        copy.push({ bone: bone, time: time })
        selection = copy
    }

    // Used by the marquee to commit a rectangle selection. When `additive`
    // is true (Ctrl/Cmd held during marquee), the rect-hit set is unioned
    // with the existing selection instead of replacing it.
    //
    // Walks the rows in render order and accumulates Y top-down, factoring
    // in any per-bone expansion (each expanded bone adds N sub-rows worth
    // of vertical space). Without this accounting, marquee hit-testing
    // misses keyframes whenever a bone above is expanded.
    function selectInRect(x1, y1, x2, y2, additive) {
        var lo = Math.min(x1, x2), hi = Math.max(x1, x2)
        var top = Math.min(y1, y2), bot = Math.max(y1, y2)
        var t1 = (lo - leftStripWidth) / pxPerSec + viewStart
        var t2 = (hi - leftStripWidth) / pxPerSec + viewStart
        var newSel = additive ? selection.slice() : []
        function alreadyHas(bone, time) {
            for (var i = 0; i < newSel.length; i++) {
                if (newSel[i].bone === bone &&
                    Math.abs(newSel[i].time - time) < 0.001) return true
            }
            return false
        }
        // Cursor walks down in viewport coordinates as we visit each row.
        var cursorY = (header.visible ? header.height : 0)
        for (var r = 0; r < rows.length; r++) {
            var activeChans = activeChannelsFor(rows[r])
            var expanded = isExpanded(rows[r].bone) && activeChans.length > 0
            var rowH = rowHeight + (expanded ? activeChans.length * rowHeight : 0)
            var rowTop = cursorY
            var rowBot = rowTop + rowH
            cursorY = rowBot + 1 // ListView spacing = 1
            if (rowBot < top || rowTop > bot) continue
            var keyTimes = rows[r].keyTimes
            for (var k = 0; k < keyTimes.length; k++) {
                var t = keyTimes[k]
                if (t >= t1 && t <= t2 && !alreadyHas(rows[r].bone, t)) {
                    newSel.push({ bone: rows[r].bone, time: t })
                }
            }
        }
        selection = newSel
    }

    Connections {
        target: AnimationControlController
        // Clip change → rebuild rows AND drop selection (different keyframes
        // entirely). Track edits (boneRowsChanged) refresh rows but preserve
        // the user's selection — bulk-drag and bone-click both fire that
        // signal, and dropping selection there breaks bulk drag mid-gesture.
        function onSelectionChanged() {
            root.rows = AnimationControlController.allBoneRows()
            root.morphRows = AnimationControlController.allMorphRows()
            root.refreshNodeRows()
            root.clearSelection()
            root.expandedBones = {}
        }
        function onBoneRowsChanged()       { root.rows = AnimationControlController.allBoneRows() }
        function onKeyframeTicksChanged()  { root.rows = AnimationControlController.allBoneRows() }
    }

    // Morph weight keyframes changed (add/move/delete/key-at-playhead) → rebuild
    // the morph band so its diamonds reflect the new times immediately.
    Connections {
        target: MorphAnimationManager
        function onMorphTargetsChanged() {
            root.morphRows = AnimationControlController.allMorphRows()
        }
    }

    // Node transform clips changed (create/delete/key/move/undo) → rebuild
    // the node band. Rebuild on any of the three signals: activeClip change
    // (Inspector picked a different clip), clipsChanged (create/delete), and
    // keyframesChanged (key added/moved/deleted, incl. via undo/redo).
    // Show the active node clip's rows only when it animates the CURRENTLY
    // SELECTED entity (or is being edited) — so the node band appears alongside
    // this mesh's skeletal/morph bands and doesn't linger when you select a
    // different, skeletal-only mesh. All three band types are shown together.
    function refreshNodeRows() {
        var sel = AnimationControlController.selectedEntityName
        var editing = NodeAnimationManager.editingClip
        var active = NodeAnimationManager.activeClip

        // Pick which node clip to show in the band, in priority order:
        //   1. the clip being EDITED (node editor open on it), or
        //   2. the ACTIVE clip if it animates the selected entity, or
        //   3. ANY node clip that animates the selected entity.
        // Case 3 is what makes a clip RECONSTRUCTED on import (or simply not the
        // node editor's active pick) show its band on load — previously the band
        // only appeared once activeClip was set, i.e. after the user opened the
        // node editor and selected the clip. (#517)
        var clip = ""
        if (editing.length > 0) {
            clip = editing
        } else if (active.length > 0 && sel.length > 0
                   && NodeAnimationManager.animatedNodes(active).indexOf(sel) >= 0) {
            clip = active
        } else if (sel.length > 0) {
            var all = NodeAnimationManager.listClips()
            for (var i = 0; i < all.length; ++i) {
                if (NodeAnimationManager.animatedNodes(all[i]).indexOf(sel) >= 0) {
                    clip = all[i]; break
                }
            }
        }
        root.nodeClip = clip
        root.nodeRows = clip.length > 0 ? NodeAnimationManager.nodeRows(clip) : []
    }
    // Rebuild ALL bands when node clips change/select — the dope sheet shows
    // skeletal + morph + node together, and selecting a node clip must not drop
    // the mesh's bone/morph rows. allBoneRows/allMorphRows resolve the skeleton
    // + morph clip from the selected entity, so refreshing them here keeps the
    // full picture in sync with node-clip activity.
    function refreshAllBands() {
        root.rows = AnimationControlController.allBoneRows()
        root.morphRows = AnimationControlController.allMorphRows()
        root.refreshNodeRows()
    }
    Connections {
        target: NodeAnimationManager
        function onActiveClipChanged()   { root.refreshAllBands() }
        function onClipsChanged()        { root.refreshAllBands() }
        function onEditingClipChanged()  { root.refreshAllBands() }
        function onKeyframesChanged(clip) {
            // Refresh when the DISPLAYED clip changes — root.nodeClip is what
            // refreshNodeRows() resolved (edited / active / any-for-entity), which
            // during an edit session can differ from activeClip. Comparing against
            // activeClip missed key add/move/delete on the shown clip. (#517)
            if (clip === root.nodeClip) root.refreshNodeRows()
        }
    }

    // Cross-platform "primary" modifier — Ctrl on Win/Linux, Cmd (Meta) on macOS.
    function isPrimaryModifier(modifiers) {
        return (modifiers & Qt.ControlModifier) || (modifiers & Qt.MetaModifier)
    }

    // ── Keyboard shortcuts (Esc / Ctrl+C / Ctrl+V) ───────────────────────────
    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_Escape) {
            root.clearSelection()
            event.accepted = true
        } else if (event.key === Qt.Key_C && root.isPrimaryModifier(event.modifiers)) {
            if (root.selection.length > 0) {
                var json = AnimationControlController.serializeKeyframes(root.selection)
                if (json.length > 0) {
                    clipboardHelper.text = json
                    clipboardHelper.selectAll()
                    clipboardHelper.copy()
                }
            }
            event.accepted = true
        } else if (event.key === Qt.Key_V && root.isPrimaryModifier(event.modifiers)) {
            clipboardHelper.clear()
            clipboardHelper.paste()
            var payload = clipboardHelper.text
            if (payload.length > 0) {
                var atTime = AnimationControlController.sliderValue / 1000.0
                AnimationControlController.pasteKeyframesAt(payload, atTime)
            }
            event.accepted = true
        }
    }

    // Hidden TextEdit acts as a clipboard bridge — Qt 6 QML has no first-class
    // clipboard accessor, but TextEdit's copy()/paste() use the system clipboard.
    TextEdit {
        id: clipboardHelper
        visible: false
        width: 0; height: 0
    }

    // ── Empty-state placeholder ──────────────────────────────────────────────
    // Only "empty" when there are neither bone rows NOR morph rows — a mesh with
    // morph targets (and no skeleton) still has a populated dope sheet, so the
    // "select a rigged mesh" message must not show for it.
    Text {
        anchors.centerIn: parent
        visible: root.rows.length === 0 && root.morphRows.length === 0
                 && root.nodeRows.length === 0
        text: AnimationControlController.hasAnimation
              ? "No animated bones in this clip."
              : "Select a rigged mesh, or add morph targets, to view keyframes."
        color: AnimationControlController.disabledTextColor
        font.pixelSize: 12
    }

    // ── Header strip with timeline ruler ─────────────────────────────────────
    Rectangle {
        id: header
        width: parent.width; height: 24
        color: AnimationControlController.headerColor
        border.color: AnimationControlController.borderColor
        visible: root.rows.length > 0

        Text {
            anchors.left: parent.left; anchors.leftMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            text: root.selection.length > 0
                  ? ("Bone (" + root.selection.length + " selected)")
                  : "Bone"
            font.bold: true; font.pixelSize: 11
            color: AnimationControlController.textColor
        }

        Canvas {
            id: rulerCanvas
            anchors.left: parent.left; anchors.leftMargin: root.leftStripWidth
            anchors.top: parent.top; anchors.bottom: parent.bottom
            anchors.right: parent.right
            onPaint: {
                var ctx = getContext("2d"); ctx.clearRect(0, 0, width, height)
                ctx.strokeStyle = AnimationControlController.borderColor
                ctx.fillStyle = AnimationControlController.textColor
                ctx.font = "10px sans-serif"; ctx.lineWidth = 1
                var t0 = root.viewStart, t1 = root.viewStart + width / root.pxPerSec
                var step = root.pxPerSec >= 100 ? 0.25 : (root.pxPerSec >= 40 ? 1.0 : 5.0)
                for (var t = Math.ceil(t0 / step) * step; t < t1; t += step) {
                    var x = (t - root.viewStart) * root.pxPerSec
                    ctx.beginPath(); ctx.moveTo(x, height - 6); ctx.lineTo(x, height); ctx.stroke()
                    ctx.fillText(t.toFixed(2) + "s", x + 2, height - 8)
                }
            }
            Connections {
                target: root
                function onPxPerSecChanged() { rulerCanvas.requestPaint() }
                function onViewStartChanged() { rulerCanvas.requestPaint() }
            }
            Connections {
                target: AnimationControlController
                function onThemeChanged() { rulerCanvas.requestPaint() }
            }
        }
    }

    // ── AI in-between control (#409) ─────────────────────────────────────────
    // Appears when the selection spans a time window (>= 2 keys at different
    // times). Fills the [minTime, maxTime] gap with N predicted keyframes
    // (RMIB ONNX model when available, smooth spline fallback otherwise).
    property real inbetweenT0: {
        if (root.selection.length < 2) return -1
        var lo = Infinity
        for (var i = 0; i < root.selection.length; i++)
            lo = Math.min(lo, root.selection[i].time)
        return lo
    }
    property real inbetweenT1: {
        if (root.selection.length < 2) return -1
        var hi = -Infinity
        for (var i = 0; i < root.selection.length; i++)
            hi = Math.max(hi, root.selection[i].time)
        return hi
    }
    property int inbetweenFrames: 8

    Rectangle {
        id: inbetweenBar
        anchors.top: header.visible ? header.bottom : parent.top
        anchors.right: parent.right
        anchors.rightMargin: 6
        anchors.topMargin: 2
        // Grow to fit the status line when present.
        width: Math.max(ibRow.implicitWidth, ibStatus.implicitWidth) + 12
        height: ibCol.implicitHeight + 6
        radius: 3
        z: 50
        color: AnimationControlController.panelColor
        border.color: AnimationControlController.borderColor
        visible: root.inbetweenT1 > root.inbetweenT0 + 0.0001

        // Result of the last fill — drives the status line (which path ran +
        // count, or why nothing happened).
        property string ibMessage: ""
        property bool ibError: false
        onVisibleChanged: { ibMessage = "" }

        // Clear the stale result whenever the target request changes — a new
        // window (T0/T1) or a new frame count means the old "via RMIB model …"
        // / "already has keyframes" line no longer describes what would happen.
        Connections {
            target: root
            function onInbetweenT0Changed()     { inbetweenBar.ibMessage = "" }
            function onInbetweenT1Changed()     { inbetweenBar.ibMessage = "" }
            function onInbetweenFramesChanged() { inbetweenBar.ibMessage = "" }
        }

        Connections {
            target: AnimationControlController
            function onInbetweenStatus(message, isError) {
                inbetweenBar.ibMessage = message
                inbetweenBar.ibError = isError
            }
        }

        Column {
            id: ibCol
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: 3
            spacing: 2

        Row {
            id: ibRow
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 6
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "AI in-between:"
                font.pixelSize: 10
                color: AnimationControlController.textColor
            }
            Rectangle {
                width: 34; height: 16; radius: 2
                anchors.verticalCenter: parent.verticalCenter
                color: AnimationControlController.inputColor
                border.color: AnimationControlController.borderColor
                TextInput {
                    id: framesInput
                    anchors.fill: parent; anchors.margins: 2
                    text: String(root.inbetweenFrames)
                    font.pixelSize: 10
                    color: AnimationControlController.textColor
                    horizontalAlignment: TextInput.AlignHCenter
                    verticalAlignment: TextInput.AlignVCenter
                    validator: IntValidator { bottom: 1; top: 240 }
                    selectByMouse: true
                    onEditingFinished: {
                        var n = parseInt(text)
                        root.inbetweenFrames = (isNaN(n) || n < 1) ? 1 : n
                        text = String(root.inbetweenFrames)
                    }
                }
            }
            Text { anchors.verticalCenter: parent.verticalCenter
                   text: "frames"; font.pixelSize: 10
                   color: AnimationControlController.textColor; opacity: 0.7 }
            Rectangle {
                id: fillBtn
                width: fillTxt.implicitWidth + 12; height: 16; radius: 2
                anchors.verticalCenter: parent.verticalCenter
                color: fillMa.containsMouse ? AnimationControlController.highlightColor
                                            : AnimationControlController.headerColor
                border.color: AnimationControlController.borderColor
                Text { id: fillTxt; anchors.centerIn: parent
                       text: "Fill gap"; font.pixelSize: 10
                       color: AnimationControlController.textColor }
                MouseArea {
                    id: fillMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        var r = AnimationControlController.inbetweenWindow(
                            root.inbetweenT0, root.inbetweenT1,
                            root.inbetweenFrames, false)
                        // The controller emits inbetweenStatus for both success
                        // and failure (handled by the Connections above). But the
                        // common "dense clip, no empty gap" case returns an error
                        // whose wording is opaque — give it a clearer hint here.
                        if (r && !r.ok && r.error
                            && String(r.error).indexOf("bracketing") >= 0) {
                            inbetweenBar.ibMessage =
                                "Selected range already has keyframes — in-betweening "
                                + "fills empty gaps between sparse keys."
                            inbetweenBar.ibError = true
                        }
                    }
                }
            }
            // Trim the clip to the selected window: keep [T0..T1], cut the
            // rest, re-time to start at 0. Undoable (Ctrl+Z restores).
            Rectangle {
                id: trimBtn
                width: trimTxt.implicitWidth + 12; height: 16; radius: 2
                anchors.verticalCenter: parent.verticalCenter
                color: trimMa.containsMouse ? AnimationControlController.highlightColor
                                            : AnimationControlController.headerColor
                border.color: AnimationControlController.borderColor
                Text { id: trimTxt; anchors.centerIn: parent
                       text: "✂ Trim to range"; font.pixelSize: 10
                       color: AnimationControlController.textColor }
                MouseArea {
                    id: trimMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: AnimationControlController.trimWindow(
                                   root.inbetweenT0, root.inbetweenT1)
                }
            }
        }

        // Result line: which path ran (RMIB model vs spline) + count, or why
        // nothing happened. Empty until the first fill.
        Text {
            id: ibStatus
            anchors.horizontalCenter: parent.horizontalCenter
            visible: inbetweenBar.ibMessage.length > 0
            text: inbetweenBar.ibMessage
            wrapMode: Text.Wrap
            width: Math.min(implicitWidth, 320)
            horizontalAlignment: Text.AlignHCenter
            font.pixelSize: 9
            color: inbetweenBar.ibError ? "#e06c6c"
                                        : AnimationControlController.textColor
            opacity: 0.85
        }
        }
    }

    // ── Bone rows ────────────────────────────────────────────────────────────
    ListView {
        id: rowsView
        anchors.left: parent.left; anchors.right: parent.right
        anchors.top: header.visible ? header.bottom : parent.top
        // Leave room for the morph + node bands at the bottom when
        // visible — otherwise the bone list would draw over them. The
        // bands stack: node band (bottom-most), morph band above it.
        anchors.bottom: morphBand.visible ? morphBand.top
                        : (nodeBand.visible ? nodeBand.top : parent.bottom)
        clip: true
        model: root.rows
        spacing: 1
        // Show whenever the selected mesh has skeletal tracks — the dope sheet
        // displays ALL animation types the selection contains (skeletal +
        // morph + node) together, so users see every clip's keys at once.
        visible: root.rows.length > 0
        // Disabled flicking so Flickable doesn't grab presses meant for
        // diamond MouseAreas / Ctrl+click multi-select. Wheel scrolling is
        // handled at the root level via a WheelHandler that scrolls
        // rowsView.contentY directly.
        interactive: false
        boundsBehavior: Flickable.StopAtBounds

        delegate: Item {
            id: rowDelegate
            property string boneName: modelData.bone
            property var keyTimes: modelData.keyTimes
            property var activeChannels: root.activeChannelsFor(modelData)
            property bool expanded: root.isExpanded(boneName) && activeChannels.length > 0

            width: rowsView.width
            height: root.rowHeight + (expanded ? activeChannels.length * root.rowHeight : 0)

            // Bone name strip carries the selection-highlight tint. Track
            // strip is transparent so timelineArea behind us catches presses
            // on empty pixels for marquee/pan.
            Rectangle {
                width: root.leftStripWidth; height: root.rowHeight
                color: (rowDelegate.boneName === AnimationControlController.selectedBone)
                       ? Qt.lighter(AnimationControlController.panelColor, 1.15)
                       : AnimationControlController.panelColor
                border.color: AnimationControlController.borderColor; border.width: 1

                Row {
                    anchors.fill: parent
                    anchors.leftMargin: 4
                    spacing: 4

                    // Per-bone expansion chevron. Only shown when the bone
                    // has at least one active channel (otherwise expanding
                    // would just show empty rows).
                    Rectangle {
                        width: 16; height: parent.height
                        color: "transparent"
                        anchors.verticalCenter: parent.verticalCenter
                        Text {
                            anchors.centerIn: parent
                            visible: rowDelegate.activeChannels.length > 0
                            text: rowDelegate.expanded ? "▼" : "▶"
                            color: AnimationControlController.textColor
                            font.pixelSize: 9
                        }
                        MouseArea {
                            anchors.fill: parent
                            enabled: rowDelegate.activeChannels.length > 0
                            onClicked: root.toggleExpanded(rowDelegate.boneName)
                        }
                    }

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: rowDelegate.boneName
                        color: AnimationControlController.textColor
                        elide: Text.ElideRight; font.pixelSize: 11
                        width: parent.width - 26
                        MouseArea {
                            anchors.fill: parent
                            onClicked: AnimationControlController.selectBone(rowDelegate.boneName)
                        }
                    }
                }
            }

            // Per-channel sub-row strip on the left side, when expanded.
            Column {
                visible: rowDelegate.expanded
                anchors.top: parent.top
                anchors.topMargin: root.rowHeight
                anchors.left: parent.left
                width: root.leftStripWidth
                Repeater {
                    model: rowDelegate.activeChannels
                    Rectangle {
                        width: root.leftStripWidth; height: root.rowHeight
                        color: AnimationControlController.panelColor
                        border.color: AnimationControlController.borderColor; border.width: 1
                        Row {
                            anchors.fill: parent
                            anchors.leftMargin: 24
                            spacing: 4
                            Rectangle {
                                width: 10; height: 10; radius: 2
                                color: modelData.color
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: modelData.label
                                color: AnimationControlController.textColor
                                font.pixelSize: 10
                            }
                        }
                    }
                }
            }

            // Per-channel sub-row track strips (right side). Each renders the
            // same keyframe times as the parent track but in the channel's
            // signature color. Click on a sub-row diamond delegates to the
            // parent keyframe (selection / playhead). Per-channel-only edits
            // arrive in slice D3.
            Repeater {
                model: rowDelegate.expanded ? rowDelegate.activeChannels : []
                Item {
                    width: rowDelegate.width - root.leftStripWidth
                    height: root.rowHeight
                    x: root.leftStripWidth
                    y: root.rowHeight + index * root.rowHeight
                    clip: true

                    // Underline, like the parent track strip
                    Rectangle {
                        anchors.left: parent.left; anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        height: 1
                        color: AnimationControlController.borderColor
                        opacity: 0.3
                    }

                    Repeater {
                        model: rowDelegate.keyTimes
                        Rectangle {
                            property real keyTime: modelData
                            x: (keyTime - root.viewStart) * root.pxPerSec - width / 2
                            anchors.verticalCenter: parent.verticalCenter
                            width: 8; height: 8
                            rotation: 45
                            color: parent.parent.modelData.color
                            border.color: AnimationControlController.borderColor
                            border.width: 1
                            opacity: 0.85
                            // Sub-row diamond click delegates to the parent
                            // keyframe — same Ctrl/Cmd toggle and selection
                            // semantics as the parent diamond. Per-channel
                            // drag/edit lands in slice D3.
                            MouseArea {
                                anchors.fill: parent
                                anchors.margins: -2
                                onPressed: function(mouse) {
                                    root.forceActiveFocus()
                                    AnimationControlController.selectBone(rowDelegate.boneName)
                                    AnimationControlController.sliderValue =
                                            Math.round(parent.keyTime * 1000)
                                    if (root.isPrimaryModifier(mouse.modifiers)) {
                                        root.toggleInSelection(rowDelegate.boneName, parent.keyTime)
                                    } else {
                                        root.setSingleSelection(rowDelegate.boneName, parent.keyTime)
                                    }
                                    mouse.accepted = true
                                }
                            }
                        }
                    }
                }
            }

            // Track strip with diamond markers (parent / aggregate row)
            Item {
                id: trackStrip
                anchors.left: parent.left; anchors.leftMargin: root.leftStripWidth
                anchors.top: parent.top
                height: root.rowHeight
                anchors.right: parent.right
                clip: true

                Rectangle {
                    anchors.left: parent.left; anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    height: 1
                    color: AnimationControlController.borderColor
                    opacity: 0.4
                }

                Repeater {
                    model: rowDelegate.keyTimes
                    Rectangle {
                        property real keyTime: modelData
                        property real dragPreviewTime: keyTime
                        // While the gesture is active, dragSelectionDt holds the
                        // shared delta the whole selection is being shifted by.
                        property bool isSelected: root.isSelected(rowDelegate.boneName, keyTime)
                        // While a bulk drag is in progress, every selected
                        // diamond renders at keyTime + dragSelectionDt — so the
                        // visual offset follows the shared delta even though
                        // only the originating MouseArea has dragArea.dragging.
                        property real displayTime: {
                            if (root.bulkDragInProgress && isSelected) {
                                return keyTime + root.dragSelectionDt
                            }
                            if (dragArea.dragging) {
                                return dragArea.bulkDragging
                                    ? keyTime + root.dragSelectionDt
                                    : dragPreviewTime
                            }
                            return keyTime
                        }
                        x: (displayTime - root.viewStart) * root.pxPerSec - width / 2
                        anchors.verticalCenter: parent.verticalCenter
                        width: isSelected ? 14 : 10
                        height: width
                        rotation: 45
                        color: dragArea.dragging
                               ? "#ff8855"
                               : (isSelected
                                  ? "#ff4444"
                                  : (Math.abs(keyTime * 1000 - AnimationControlController.selectedTick) < 1
                                     ? "#ff4444" : "#ffcc00"))
                        border.color: isSelected ? "white" : AnimationControlController.borderColor
                        border.width: isSelected ? 2 : 1

                        MouseArea {
                            id: dragArea
                            anchors.fill: parent
                            anchors.margins: -3
                            cursorShape: Qt.SizeHorCursor
                            preventStealing: true
                            property bool dragging: false
                            property bool bulkDragging: false
                            property real pressX: 0
                            property real originalTime: 0
                            onPressed: function(mouse) {
                                root.forceActiveFocus()
                                originalTime = parent.keyTime
                                pressX = mouse.x
                                mouse.accepted = true
                                if (root.isPrimaryModifier(mouse.modifiers)) {
                                    // Ctrl/Cmd+click: toggle in/out of selection,
                                    // don't start a drag.
                                    root.toggleInSelection(rowDelegate.boneName, parent.keyTime)
                                    AnimationControlController.selectBone(rowDelegate.boneName)
                                    return
                                }
                                if (!root.isSelected(rowDelegate.boneName, parent.keyTime)) {
                                    root.setSingleSelection(rowDelegate.boneName, parent.keyTime)
                                }
                                dragging = true
                                bulkDragging = root.selection.length > 1
                                root.dragSelectionDt = 0
                                root.bulkDragInProgress = bulkDragging
                                AnimationControlController.selectBone(rowDelegate.boneName)
                                AnimationControlController.sliderValue =
                                        Math.round(parent.keyTime * 1000)
                            }
                            onPositionChanged: function(mouse) {
                                if (!dragging) return
                                var dx = mouse.x - pressX
                                var dt = dx / root.pxPerSec
                                if (bulkDragging) {
                                    root.dragSelectionDt = dt
                                } else {
                                    var target = originalTime + dt
                                    if (target < 0) target = 0
                                    var len = AnimationControlController.animationLength
                                    if (target > len) target = len
                                    parent.dragPreviewTime = target
                                }
                            }
                            onReleased: function(mouse) {
                                if (!dragging) return
                                dragging = false
                                root.bulkDragInProgress = false
                                if (bulkDragging) {
                                    var dt = root.dragSelectionDt
                                    root.dragSelectionDt = 0
                                    bulkDragging = false
                                    if (Math.abs(dt) > 0.001) {
                                        AnimationControlController.moveKeyframes(root.selection, dt)
                                    }
                                } else {
                                    var target = parent.dragPreviewTime
                                    if (Math.abs(target - originalTime) > 0.001) {
                                        AnimationControlController.moveKeyframe(
                                            rowDelegate.boneName, originalTime, target)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // Shared bulk-drag delta — every selected diamond binds against this so
    // they all animate together while one is being dragged. Reset on release.
    property real dragSelectionDt: 0
    property bool bulkDragInProgress: false

    // ── Marquee select + middle-drag pan + wheel zoom over timeline area ────
    // z: -1 puts this MouseArea behind the ListView. The row delegate roots
    // are Items (not Rectangles), so the trackStrip is non-opaque and
    // presses on empty pixels fall through to here. Diamonds + bone-name
    // MouseAreas inside each row capture their own presses naturally.
    MouseArea {
        id: timelineArea
        z: -1
        anchors.left: parent.left; anchors.leftMargin: root.leftStripWidth
        anchors.top: header.visible ? header.bottom : parent.top
        anchors.right: parent.right; anchors.bottom: parent.bottom
        acceptedButtons: Qt.MiddleButton | Qt.LeftButton
        property real panStartX: 0
        property real panStartView: 0
        property bool marquee: false
        property bool marqueeAdditive: false
        property real mqStartX: 0
        property real mqStartY: 0
        property real mqEndX: 0
        property real mqEndY: 0

        onPressed: function(mouse) {
            root.forceActiveFocus()
            if (mouse.button === Qt.MiddleButton) {
                panStartX = mouse.x
                panStartView = root.viewStart
                mouse.accepted = true
            } else if (mouse.button === Qt.LeftButton) {
                // Left-click on empty timeline area → start marquee selection.
                // Diamonds capture their own presses via preventStealing.
                marquee = true
                marqueeAdditive = root.isPrimaryModifier(mouse.modifiers)
                mqStartX = mouse.x; mqStartY = mouse.y
                mqEndX = mouse.x;   mqEndY = mouse.y
                if (!marqueeAdditive) root.clearSelection()
                mouse.accepted = true
            } else {
                mouse.accepted = false
            }
        }
        onPositionChanged: function(mouse) {
            if (!pressed) return
            if (marquee) {
                mqEndX = mouse.x; mqEndY = mouse.y
                marqueeRect.requestPaint()
            } else {
                var dx = mouse.x - panStartX
                root.viewStart = panStartView - dx / root.pxPerSec
                if (root.viewStart < 0) root.viewStart = 0
            }
        }
        onReleased: function(mouse) {
            if (marquee) {
                marquee = false
                // Translate marquee from timeline-area coords to root coords.
                var leftPad = root.leftStripWidth
                var topPad  = header.visible ? header.height : 0
                root.selectInRect(mqStartX + leftPad, mqStartY + topPad,
                                  mqEndX + leftPad,   mqEndY + topPad,
                                  marqueeAdditive)
                marqueeRect.requestPaint()
            }
        }

        // Ctrl/Cmd+wheel zooms around the cursor; plain wheel falls through
        // to the ListView for vertical scrolling.
        WheelHandler {
            acceptedModifiers: Qt.ControlModifier | Qt.MetaModifier
            onWheel: function(event) {
                var factor = event.angleDelta.y > 0 ? 1.15 : (1.0 / 1.15)
                var newPxPerSec = root.pxPerSec * factor
                newPxPerSec = Math.max(20, Math.min(2000, newPxPerSec))
                if (newPxPerSec === root.pxPerSec) return
                var tCursor = root.viewStart + event.point.position.x / root.pxPerSec
                root.pxPerSec = newPxPerSec
                root.viewStart = tCursor - event.point.position.x / newPxPerSec
                if (root.viewStart < 0) root.viewStart = 0
            }
        }
    }

    // Marquee rectangle overlay — drawn at root level (z: 100) so it sits on
    // top of rows + diamonds. timelineArea is at z: -1, so its child Canvas
    // would be invisible behind the rows.
    Canvas {
        id: marqueeRect
        z: 100
        x: timelineArea.x
        y: timelineArea.y
        width: timelineArea.width
        height: timelineArea.height
        visible: timelineArea.marquee
        onPaint: {
            var ctx = getContext("2d"); ctx.clearRect(0, 0, width, height)
            if (!timelineArea.marquee) return
            var x1 = Math.min(timelineArea.mqStartX, timelineArea.mqEndX)
            var y1 = Math.min(timelineArea.mqStartY, timelineArea.mqEndY)
            var w  = Math.abs(timelineArea.mqEndX - timelineArea.mqStartX)
            var h  = Math.abs(timelineArea.mqEndY - timelineArea.mqStartY)
            ctx.fillStyle = "rgba(64, 192, 255, 0.18)"
            ctx.fillRect(x1, y1, w, h)
            ctx.strokeStyle = "#40c0ff"; ctx.lineWidth = 1
            ctx.strokeRect(x1 + 0.5, y1 + 0.5, w, h)
        }
        Connections {
            target: timelineArea
            function onMqStartXChanged() { marqueeRect.requestPaint() }
            function onMqStartYChanged() { marqueeRect.requestPaint() }
            function onMqEndXChanged()   { marqueeRect.requestPaint() }
            function onMqEndYChanged()   { marqueeRect.requestPaint() }
            function onMarqueeChanged()  { marqueeRect.requestPaint() }
        }
    }

    // Plain wheel anywhere in the dope sheet scrolls the bone rows. Lives at
    // root level so it sees wheel events regardless of which child happens
    // to be under the cursor (ListView is interactive:false and the inner
    // diamonds/timelineArea otherwise eat the wheel). Ctrl/Cmd+wheel is
    // claimed by timelineArea's zoom handler instead.
    // Public method called by C++ wheel filter on the host QQuickWidget.
    // QQuickWidget inside QDockWidget can swallow wheel events on macOS;
    // routing them through here guarantees the rows scroll regardless.
    function scrollByPixels(dy) {
        if (dy === 0) return
        // Scroll the bone list as before; ListView handles its own
        // clamping in contentY assignments below.
        if (rowsView.visible) {
            var maxY = Math.max(0, rowsView.contentHeight - rowsView.height)
            rowsView.contentY = Math.max(0, Math.min(maxY, rowsView.contentY - dy))
        }
        // Also scroll the morph band when it has overflowing content.
        // We disable Flickable.interactive (so drag-marquee passes
        // through to timelineArea) which removes Flickable's own wheel
        // handling — proxy it here. Same dy as the bone list so a single
        // wheel notch advances both views consistently.
        if (morphBand.visible && morphList.contentHeight > morphList.height) {
            var maxMY = morphList.contentHeight - morphList.height
            morphList.contentY = Math.max(0, Math.min(maxMY, morphList.contentY - dy))
        }
        // Same for the node band (interactive:false, so it needs the proxy too):
        // a clip with more nodes than the 40% cap allows would otherwise hide the
        // extra rows permanently. (#517)
        if (nodeBand.visible && nodeList.contentHeight > nodeList.height) {
            var maxNY = nodeList.contentHeight - nodeList.height
            nodeList.contentY = Math.max(0, Math.min(maxNY, nodeList.contentY - dy))
        }
    }

    WheelHandler {
        target: null // accept events anywhere on the root, not on a specific item
        acceptedModifiers: Qt.NoModifier
        onWheel: function(event) {
            var dy = (event.pixelDelta && event.pixelDelta.y !== 0)
                ? event.pixelDelta.y
                : event.angleDelta.y / 120 * 40
            if (dy === 0) return
            root.scrollByPixels(dy)
            event.accepted = true
        }
    }

    // ── Morph-target rows (slice A5b) ────────────────────────────────────────
    // Fixed-height read-only band anchored to the bottom. One row per
    // morph target with diamond markers at each keyframe time, sharing
    // the bone-track timeline (same `pxPerSec`, `viewStart`). When the
    // entity has no morphs the band collapses to height=0 so bone-only
    // assets look exactly the same as before.
    //
    // Keep this strictly read-only: no MouseAreas on the diamonds, no
    // selection toggling, no drag — full interaction lives in a future
    // slice. The point here is to make morph-weight animation visible
    // alongside skeletal animation, which is the load-bearing piece of
    // #518's "dope sheet integration" acceptance criterion.
    Rectangle {
        id: morphBand
        anchors.left: parent.left
        anchors.right: parent.right
        // Shown whenever the selection has morph targets — the sheet shows all
        // animation types together (skeletal + morph + node).
        visible: morphRowsRep.count > 0

        // When there ARE bone tracks, the band docks to the BOTTOM under them
        // (capped at 40% so it can't shove the bone rows off-screen). When there
        // are NO bone tracks (morph-only mesh), it fills the whole sheet from the
        // header down — otherwise it left an empty gap where the old skeleton
        // message used to sit.
        readonly property bool boneRowsPresent: root.rows.length > 0
        // Dock above the node band when it's showing, else to the bottom. ALWAYS
        // bottom-anchored, never top-anchored — toggling anchors.top to undefined
        // doesn't reliably clear in QML (same negative-height bug the node band
        // hit). Size purely via `height`.
        anchors.bottom: nodeBand.visible ? nodeBand.top : parent.bottom

        readonly property int naturalContentHeight:
            morphHeader.height + morphRowsRep.count * (root.rowHeight + 1) + 4
        readonly property int maxBandHeight:
            Math.max(morphHeader.height + root.rowHeight + 6,
                     Math.floor(root.height * 0.4))
        // Fill height for the morph-only case (no bone rows): from below the
        // header to whatever the band is anchored above (node band or bottom).
        readonly property int fillHeight:
            (nodeBand.visible ? nodeBand.y : root.height)
            - (header.visible ? (header.y + header.height) : 0) - 2
        height: !visible ? 0
                : boneRowsPresent ? Math.min(naturalContentHeight, maxBandHeight)
                : Math.max(naturalContentHeight, fillHeight)
        color: AnimationControlController.panelColor
        border.color: AnimationControlController.borderColor
        border.width: 1

        Rectangle {
            id: morphHeader
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 16
            color: Qt.darker(AnimationControlController.panelColor, 1.15)
            Text {
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left; anchors.leftMargin: 6
                text: "Morph Targets (" + morphRowsRep.count + ")"
                color: AnimationControlController.textColor
                font.pixelSize: 10
                font.bold: true
            }
        }

        // Scrollable list when content exceeds the capped height. Plain
        // Flickable + Column (instead of ListView) so the existing
        // Repeater-rendered rows continue to work unchanged.
        Flickable {
            id: morphList
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: morphHeader.bottom
            anchors.bottom: parent.bottom
            anchors.topMargin: 2
            clip: true
            contentHeight: morphCol.height
            boundsBehavior: Flickable.StopAtBounds
            // Match `rowsView` and disable left-drag flicking — the
            // root timelineArea owns marquee selection over empty
            // pixels and Flickable would otherwise grab those drags.
            // Wheel scrolling still works because the root WheelHandler
            // is routed through `scrollByPixels` rather than relying on
            // Flickable's own wheel handling.
            interactive: false

            Column {
                id: morphCol
                width: parent.width
                spacing: 1

                Repeater {
                    id: morphRowsRep
                    model: root.morphRows

                    Item {
                        width: parent.width
                        height: root.rowHeight

                        Rectangle {
                            width: root.leftStripWidth; height: root.rowHeight
                            color: AnimationControlController.panelColor
                            border.color: AnimationControlController.borderColor
                            border.width: 1
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.left: parent.left; anchors.leftMargin: 8
                                text: modelData.name
                                color: AnimationControlController.textColor
                                font.pixelSize: 10
                                elide: Text.ElideRight
                                width: root.leftStripWidth - 12
                            }
                        }

                        // Right side: interactive diamonds. Drag to move the
                        // keyframe time, right-click to delete, double-click an
                        // empty spot to add a key at that time (weight = the
                        // target's current weight). Shares the bone-row timeline
                        // math so morph + bone diamonds line up vertically.
                        Item {
                            id: morphTrackArea
                            property string morphName: modelData.name
                            anchors.left: parent.left; anchors.leftMargin: root.leftStripWidth
                            anchors.right: parent.right
                            height: root.rowHeight
                            clip: true

                            Rectangle {
                                anchors.left: parent.left; anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                height: 1
                                color: AnimationControlController.borderColor
                                opacity: 0.4
                            }

                            // Empty-area handler: double-click adds a key at the
                            // clicked time. Sits UNDER the diamonds so their own
                            // MouseAreas win for drag/delete.
                            MouseArea {
                                anchors.fill: parent
                                acceptedButtons: Qt.LeftButton
                                onDoubleClicked: function(mouse) {
                                    var t = mouse.x / root.pxPerSec + root.viewStart
                                    if (t < 0) t = 0
                                    var w = MorphAnimationManager.weightForSelection(morphTrackArea.morphName)
                                    MorphAnimationManager.setMorphWeightKeyframe(
                                        morphTrackArea.morphName, t, w)
                                    MorphAnimationManager.activateWeightClip()
                                    AnimationControlController.sliderValue = Math.round(t * 1000)
                                }
                            }

                            Repeater {
                                model: modelData.keyTimes
                                Rectangle {
                                    id: morphDiamond
                                    property real keyTime: modelData
                                    property real dragPreviewTime: keyTime
                                    property real displayTime: dragMorph.dragging ? dragPreviewTime : keyTime
                                    x: (displayTime - root.viewStart) * root.pxPerSec - width / 2
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: dragMorph.dragging ? 14 : 10
                                    height: width
                                    rotation: 45
                                    color: dragMorph.dragging ? "#ffaa55" : "#88ccff"
                                    border.color: AnimationControlController.borderColor
                                    border.width: 1

                                    MouseArea {
                                        id: dragMorph
                                        anchors.fill: parent
                                        anchors.margins: -3
                                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                                        cursorShape: Qt.SizeHorCursor
                                        preventStealing: true
                                        property bool dragging: false
                                        property real pressX: 0
                                        property real originalTime: 0
                                        onPressed: function(mouse) {
                                            root.forceActiveFocus()
                                            if (mouse.button === Qt.RightButton) {
                                                MorphAnimationManager.clearMorphWeightKeyframe(
                                                    morphTrackArea.morphName, morphDiamond.keyTime)
                                                mouse.accepted = true
                                                return
                                            }
                                            originalTime = morphDiamond.keyTime
                                            pressX = mouse.x
                                            dragging = true
                                            AnimationControlController.sliderValue =
                                                Math.round(morphDiamond.keyTime * 1000)
                                            mouse.accepted = true
                                        }
                                        onPositionChanged: function(mouse) {
                                            if (!dragging) return
                                            var dt = (mouse.x - pressX) / root.pxPerSec
                                            var target = originalTime + dt
                                            if (target < 0) target = 0
                                            morphDiamond.dragPreviewTime = target
                                        }
                                        onReleased: function(mouse) {
                                            if (!dragging) return
                                            dragging = false
                                            var target = morphDiamond.dragPreviewTime
                                            if (Math.abs(target - originalTime) > 0.001) {
                                                MorphAnimationManager.moveMorphWeightKeyframe(
                                                    morphTrackArea.morphName, originalTime, target)
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // ── Node transform rows (#517 slice C) ───────────────────────────────────
    // Interactive band, docked at the very bottom (below the morph band), for
    // the SceneManager-owned node-transform clips the Inspector's "Node
    // Transform Animation" section authors. One row per animated SceneNode,
    // diamonds at each keyframe time. Drag a diamond to re-time it, right-click
    // to delete, double-click empty space to key the node's CURRENT transform
    // at that time (undoable). Shares the bone/morph timeline math (pxPerSec,
    // viewStart) so all three bands line up vertically. Collapses to height 0
    // when no node clip is active, so bone/morph-only sheets are unchanged.
    Rectangle {
        id: nodeBand
        anchors.left: parent.left
        anchors.right: parent.right
        // ALWAYS bottom-anchored, NEVER top-anchored. Toggling anchors.top to
        // `undefined` (the previous approach) does not reliably clear the anchor
        // in QML — the band stayed pinned to the header and grew to fill the
        // whole sheet, giving rowsView a NEGATIVE height (bug: bone rows
        // invisible when a node clip existed). Instead we control the band's
        // size purely with `height`: a fixed content-sized band when other bands
        // are present, or a taller fill (capped) when the sheet is node-only.
        anchors.bottom: parent.bottom
        visible: nodeRowsRep.count > 0

        readonly property bool otherRowsPresent: root.rows.length > 0 || root.morphRows.length > 0

        // Content-sized height, capped at 40% so a many-node clip can't shove
        // the bone/morph rows off-screen; scrolls internally past the cap.
        readonly property int naturalContentHeight:
            nodeHeader.height + nodeRowsRep.count * (root.rowHeight + 1) + 4
        readonly property int maxBandHeight:
            Math.max(nodeHeader.height + root.rowHeight + 6,
                     Math.floor(root.height * 0.4))
        // node-only sheet: fill from just below the header to the bottom.
        readonly property int fillHeight:
            root.height - (header.visible ? (header.y + header.height) : 0) - 2
        height: !visible ? 0
                : otherRowsPresent ? Math.min(naturalContentHeight, maxBandHeight)
                : Math.max(naturalContentHeight, fillHeight)
        color: AnimationControlController.panelColor
        border.color: AnimationControlController.borderColor
        border.width: 1

        Rectangle {
            id: nodeHeader
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 16
            color: Qt.darker(AnimationControlController.panelColor, 1.15)
            Text {
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left; anchors.leftMargin: 6
                text: "Node Transforms — " + root.nodeClip + " (" + nodeRowsRep.count + ")"
                color: AnimationControlController.textColor
                font.pixelSize: 10
                font.bold: true
                elide: Text.ElideRight
                width: parent.width - 12
            }
        }

        Flickable {
            id: nodeList
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: nodeHeader.bottom
            anchors.bottom: parent.bottom
            anchors.topMargin: 2
            clip: true
            contentHeight: nodeCol.height
            boundsBehavior: Flickable.StopAtBounds
            interactive: false

            Column {
                id: nodeCol
                width: parent.width
                spacing: 1

                Repeater {
                    id: nodeRowsRep
                    model: root.nodeRows

                    Item {
                        width: parent.width
                        height: root.rowHeight

                        Rectangle {
                            width: root.leftStripWidth; height: root.rowHeight
                            color: AnimationControlController.panelColor
                            border.color: AnimationControlController.borderColor
                            border.width: 1
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.left: parent.left; anchors.leftMargin: 8
                                text: modelData.node
                                color: AnimationControlController.textColor
                                font.pixelSize: 10
                                elide: Text.ElideRight
                                width: root.leftStripWidth - 12
                            }
                        }

                        Item {
                            id: nodeTrackArea
                            property string nodeName: modelData.node
                            anchors.left: parent.left; anchors.leftMargin: root.leftStripWidth
                            anchors.right: parent.right
                            height: root.rowHeight
                            clip: true

                            Rectangle {
                                anchors.left: parent.left; anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                height: 1
                                color: AnimationControlController.borderColor
                                opacity: 0.4
                            }

                            // Empty-area double-click: key the node's CURRENT
                            // transform at the clicked time. Sits under the
                            // diamonds so their MouseAreas win for drag/delete.
                            MouseArea {
                                anchors.fill: parent
                                acceptedButtons: Qt.LeftButton
                                onDoubleClicked: function(mouse) {
                                    var t = mouse.x / root.pxPerSec + root.viewStart
                                    if (t < 0) t = 0
                                    NodeAnimationManager.keyNodeCurrentTransform(
                                        root.nodeClip, nodeTrackArea.nodeName, t)
                                    AnimationControlController.sliderValue = Math.round(t * 1000)
                                }
                            }

                            Repeater {
                                model: modelData.keyTimes
                                Rectangle {
                                    id: nodeDiamond
                                    property real keyTime: modelData
                                    property real dragPreviewTime: keyTime
                                    property real displayTime: dragNode.dragging ? dragPreviewTime : keyTime
                                    x: (displayTime - root.viewStart) * root.pxPerSec - width / 2
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: dragNode.dragging ? 14 : 10
                                    height: width
                                    rotation: 45
                                    color: dragNode.dragging ? "#88ffaa" : "#66dd88"
                                    border.color: AnimationControlController.borderColor
                                    border.width: 1

                                    MouseArea {
                                        id: dragNode
                                        anchors.fill: parent
                                        anchors.margins: -3
                                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                                        cursorShape: Qt.SizeHorCursor
                                        preventStealing: true
                                        property bool dragging: false
                                        property real pressX: 0
                                        property real originalTime: 0
                                        onPressed: function(mouse) {
                                            root.forceActiveFocus()
                                            if (mouse.button === Qt.RightButton) {
                                                NodeAnimationManager.deleteNodeKeyframe(
                                                    root.nodeClip, nodeTrackArea.nodeName,
                                                    nodeDiamond.keyTime)
                                                mouse.accepted = true
                                                return
                                            }
                                            originalTime = nodeDiamond.keyTime
                                            pressX = mouse.x
                                            dragging = true
                                            // Scrub so the viewport shows this pose.
                                            AnimationControlController.sliderValue =
                                                Math.round(nodeDiamond.keyTime * 1000)
                                            NodeAnimationManager.scrubClip(
                                                root.nodeClip, nodeDiamond.keyTime)
                                            mouse.accepted = true
                                        }
                                        onPositionChanged: function(mouse) {
                                            if (!dragging) return
                                            var dt = (mouse.x - pressX) / root.pxPerSec
                                            var target = originalTime + dt
                                            if (target < 0) target = 0
                                            nodeDiamond.dragPreviewTime = target
                                        }
                                        onReleased: function(mouse) {
                                            if (!dragging) return
                                            dragging = false
                                            var target = nodeDiamond.dragPreviewTime
                                            if (Math.abs(target - originalTime) > 0.001) {
                                                NodeAnimationManager.moveNodeKeyframe(
                                                    root.nodeClip, nodeTrackArea.nodeName,
                                                    originalTime, target)
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
