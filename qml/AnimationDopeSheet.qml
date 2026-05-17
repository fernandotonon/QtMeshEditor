import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AnimationControl 1.0
import PropertiesPanel 1.0  // MorphAnimationManager (slice A5)

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

    // Slice A5: morph-target rows for the selected entity. Each
    // entry is `{ name, keyTimes }`. Renders below the bone rows
    // as a read-only band — full selection/move/copy interaction
    // for morph tracks is a follow-up slice.
    property var morphRows: AnimationControlController.allMorphRows()

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
            root.clearSelection()
            root.expandedBones = {}
        }
        function onBoneRowsChanged()       { root.rows = AnimationControlController.allBoneRows() }
        function onKeyframeTicksChanged()  { root.rows = AnimationControlController.allBoneRows() }
    }

    // Refresh the morph band whenever the morph manager's data changes
    // (selection moved to a different entity, or a weight was set
    // through any path — Inspector slider, MCP, future authoring).
    Connections {
        target: MorphAnimationManager
        function onMorphTargetsChanged() {
            root.morphRows = AnimationControlController.allMorphRows()
        }
        function onMorphWeightChanged(entity, name, weight) {
            root.morphRows = AnimationControlController.allMorphRows()
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
    Text {
        anchors.centerIn: parent
        visible: root.rows.length === 0
        text: AnimationControlController.hasAnimation
              ? "No animated bones in this clip."
              : "Select a rigged mesh and an animation to view its keyframes."
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

    // ── Bone rows ────────────────────────────────────────────────────────────
    ListView {
        id: rowsView
        anchors.left: parent.left; anchors.right: parent.right
        anchors.top: header.visible ? header.bottom : parent.top
        anchors.bottom: parent.bottom
        clip: true
        model: root.rows
        spacing: 1
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

    // ── Morph-target rows (slice A5) ─────────────────────────────────────────
    // Read-only band anchored to the bottom of the dope sheet. Shows
    // one row per Ogre::Pose on the selected entity, with diamond
    // markers at each keyframe time. Selection / move / copy
    // interaction is a follow-up — A5 ships visibility only.
    Rectangle {
        id: morphBand
        anchors.left: parent.left; anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: visible ? (morphHeader.height + morphRowsCol.implicitHeight) : 0
        color: AnimationControlController.panelColor
        border.color: AnimationControlController.borderColor
        visible: root.morphRows.length > 0

        Rectangle {
            id: morphHeader
            anchors.left: parent.left; anchors.right: parent.right
            anchors.top: parent.top
            height: 18
            color: AnimationControlController.headerColor
            border.color: AnimationControlController.borderColor
            Text {
                anchors.left: parent.left; anchors.leftMargin: 6
                anchors.verticalCenter: parent.verticalCenter
                text: "Morph Targets (" + root.morphRows.length + ")"
                color: AnimationControlController.textColor
                font.pixelSize: 10; font.bold: true
            }
        }

        Column {
            id: morphRowsCol
            anchors.left: parent.left; anchors.right: parent.right
            anchors.top: morphHeader.bottom
            spacing: 1

            Repeater {
                model: root.morphRows
                delegate: Item {
                    width: morphRowsCol.width
                    height: root.rowHeight

                    // Name strip
                    Rectangle {
                        width: root.leftStripWidth; height: parent.height
                        color: AnimationControlController.panelColor
                        border.color: AnimationControlController.borderColor; border.width: 1
                        Text {
                            anchors.fill: parent
                            anchors.leftMargin: 6
                            verticalAlignment: Text.AlignVCenter
                            text: modelData.name
                            color: AnimationControlController.textColor
                            elide: Text.ElideRight
                            font.pixelSize: 11
                        }
                    }

                    // Track strip with diamonds at each keyframe.
                    Item {
                        anchors.left: parent.left; anchors.leftMargin: root.leftStripWidth
                        anchors.right: parent.right
                        height: parent.height
                        Repeater {
                            model: modelData.keyTimes
                            delegate: Rectangle {
                                property real keyTime: modelData
                                width: 8; height: 8; radius: 1
                                rotation: 45
                                color: "#c08040"
                                border.color: AnimationControlController.borderColor
                                anchors.verticalCenter: parent.verticalCenter
                                x: (keyTime - root.viewStart) * root.pxPerSec - width / 2
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
        if (dy === 0 || !rowsView.visible) return
        var maxY = Math.max(0, rowsView.contentHeight - rowsView.height)
        rowsView.contentY = Math.max(0, Math.min(maxY, rowsView.contentY - dy))
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
}
