import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AnimationControl 1.0

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

    // Used by the marquee to commit a rectangle selection.
    function selectInRect(x1, y1, x2, y2) {
        var lo = Math.min(x1, x2), hi = Math.max(x1, x2)
        var top = Math.min(y1, y2), bot = Math.max(y1, y2)
        var t1 = (lo - leftStripWidth) / pxPerSec + viewStart
        var t2 = (hi - leftStripWidth) / pxPerSec + viewStart
        var newSel = []
        for (var r = 0; r < rows.length; r++) {
            // Header (24px) + r * (rowHeight + spacing 1)
            var rowTop = (header.visible ? header.height : 0) + r * (rowHeight + 1)
            var rowBot = rowTop + rowHeight
            if (rowBot < top || rowTop > bot) continue
            var keyTimes = rows[r].keyTimes
            for (var k = 0; k < keyTimes.length; k++) {
                var t = keyTimes[k]
                if (t >= t1 && t <= t2) newSel.push({ bone: rows[r].bone, time: t })
            }
        }
        selection = newSel
    }

    Connections {
        target: AnimationControlController
        function onBoneRowsChanged()       { root.rows = AnimationControlController.allBoneRows(); root.clearSelection() }
        function onSelectionChanged()      { root.rows = AnimationControlController.allBoneRows(); root.clearSelection() }
        function onKeyframeTicksChanged()  { root.rows = AnimationControlController.allBoneRows() }
    }

    // ── Keyboard shortcuts (Esc / Ctrl+C / Ctrl+V) ───────────────────────────
    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_Escape) {
            root.clearSelection()
            event.accepted = true
        } else if (event.key === Qt.Key_C && (event.modifiers & Qt.ControlModifier)) {
            if (root.selection.length > 0) {
                var json = AnimationControlController.serializeKeyframes(root.selection)
                if (json.length > 0) {
                    clipboardHelper.text = json
                    clipboardHelper.selectAll()
                    clipboardHelper.copy()
                }
            }
            event.accepted = true
        } else if (event.key === Qt.Key_V && (event.modifiers & Qt.ControlModifier)) {
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
        interactive: false // we manage scrolling via wheel/middle-drag

        delegate: Rectangle {
            id: rowDelegate
            property string boneName: modelData.bone
            property var keyTimes: modelData.keyTimes

            width: rowsView.width; height: root.rowHeight
            color: (boneName === AnimationControlController.selectedBone)
                   ? Qt.lighter(AnimationControlController.panelColor, 1.15)
                   : AnimationControlController.panelColor

            // Bone name (clickable — selects the bone)
            Rectangle {
                width: root.leftStripWidth; height: parent.height
                color: "transparent"
                border.color: AnimationControlController.borderColor; border.width: 1
                Text {
                    anchors.left: parent.left; anchors.leftMargin: 6
                    anchors.verticalCenter: parent.verticalCenter
                    text: rowDelegate.boneName
                    color: AnimationControlController.textColor
                    elide: Text.ElideRight; font.pixelSize: 11
                    width: parent.width - 12
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: AnimationControlController.selectBone(rowDelegate.boneName)
                }
            }

            // Track strip with diamond markers
            Item {
                id: trackStrip
                anchors.left: parent.left; anchors.leftMargin: root.leftStripWidth
                anchors.top: parent.top; anchors.bottom: parent.bottom
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
                        property real displayTime: dragArea.dragging
                            ? (dragArea.bulkDragging
                                ? keyTime + root.dragSelectionDt
                                : dragPreviewTime)
                            : keyTime
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
                                if (mouse.modifiers & Qt.ControlModifier) {
                                    root.toggleInSelection(rowDelegate.boneName, parent.keyTime)
                                    AnimationControlController.selectBone(rowDelegate.boneName)
                                    mouse.accepted = true
                                    return
                                }
                                if (!root.isSelected(rowDelegate.boneName, parent.keyTime)) {
                                    root.setSingleSelection(rowDelegate.boneName, parent.keyTime)
                                }
                                dragging = true
                                bulkDragging = root.selection.length > 1
                                root.dragSelectionDt = 0
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

    // Shared bulk-drag delta — bound by all diamonds in the selection so they
    // move together. Reset in onReleased.
    property real dragSelectionDt: 0

    // ── Marquee select + middle-drag pan + wheel zoom over timeline area ────
    MouseArea {
        id: timelineArea
        anchors.left: parent.left; anchors.leftMargin: root.leftStripWidth
        anchors.top: header.visible ? header.bottom : parent.top
        anchors.right: parent.right; anchors.bottom: parent.bottom
        acceptedButtons: Qt.MiddleButton | Qt.LeftButton
        propagateComposedEvents: true
        property real panStartX: 0
        property real panStartView: 0
        property bool marquee: false
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
                // Click-through on diamonds is handled by their own MouseArea
                // (preventStealing = true), so this only fires on background.
                marquee = true
                mqStartX = mouse.x; mqStartY = mouse.y
                mqEndX = mouse.x;   mqEndY = mouse.y
                if (!(mouse.modifiers & Qt.ControlModifier)) root.clearSelection()
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
                                  mqEndX + leftPad,   mqEndY + topPad)
                marqueeRect.requestPaint()
            }
        }

        // Marquee rectangle overlay
        Canvas {
            id: marqueeRect
            anchors.fill: parent
            visible: timelineArea.marquee
            onPaint: {
                var ctx = getContext("2d"); ctx.clearRect(0, 0, width, height)
                if (!timelineArea.marquee) return
                var x = Math.min(timelineArea.mqStartX, timelineArea.mqEndX)
                var y = Math.min(timelineArea.mqStartY, timelineArea.mqEndY)
                var w = Math.abs(timelineArea.mqEndX - timelineArea.mqStartX)
                var h = Math.abs(timelineArea.mqEndY - timelineArea.mqStartY)
                ctx.fillStyle = "rgba(64, 192, 255, 0.18)"
                ctx.fillRect(x, y, w, h)
                ctx.strokeStyle = "#40c0ff"; ctx.lineWidth = 1
                ctx.strokeRect(x + 0.5, y + 0.5, w, h)
            }
        }

        WheelHandler {
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
}
