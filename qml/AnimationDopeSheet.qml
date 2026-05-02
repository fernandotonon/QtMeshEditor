import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AnimationControl 1.0

// Multi-bone dope sheet — one row per animated bone with diamond markers
// at each keyframe time. Drag a marker horizontally to move the keyframe
// (calls AnimationControlController.moveKeyframe). Wheel to zoom around the
// cursor; middle-drag to pan.
Rectangle {
    id: root
    color: AnimationControlController.panelColor

    // Pixels-per-second view scale and horizontal scroll offset (in seconds).
    property real pxPerSec: 200
    property real viewStart: 0.0

    property int leftStripWidth: 130
    property int rowHeight: 22

    // Cached row data refreshed from the controller.
    property var rows: AnimationControlController.allBoneRows()

    Connections {
        target: AnimationControlController
        function onBoneRowsChanged()       { root.rows = AnimationControlController.allBoneRows() }
        function onSelectionChanged()      { root.rows = AnimationControlController.allBoneRows() }
        function onKeyframeTicksChanged()  { root.rows = AnimationControlController.allBoneRows() }
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

    // ── Header strip with timeline ruler + zoom hint ─────────────────────────
    Rectangle {
        id: header
        width: parent.width; height: 24
        color: AnimationControlController.headerColor
        border.color: AnimationControlController.borderColor
        visible: root.rows.length > 0

        Text {
            anchors.left: parent.left; anchors.leftMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            text: "Bone"
            font.bold: true; font.pixelSize: 11
            color: AnimationControlController.textColor
        }

        // Time ruler — major ticks every 0.5s
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

        delegate: Rectangle {
            id: rowDelegate
            // Capture row data once at the delegate scope. The inner Repeater
            // shadows `modelData` with the keyTime number, so we can't reach
            // back to the row map from inside it.
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

                // Underline
                Rectangle {
                    anchors.left: parent.left; anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    height: 1
                    color: AnimationControlController.borderColor
                    opacity: 0.4
                }

                // Diamonds
                Repeater {
                    model: rowDelegate.keyTimes
                    Rectangle {
                        // The bound keyTime; while dragging we render at
                        // `dragPreviewTime` instead of pushing a command per
                        // pixel. The single MoveKeyframeCommand is pushed on
                        // release, giving exactly one undoable step per gesture.
                        property real keyTime: modelData
                        property real dragPreviewTime: keyTime
                        property real displayTime: dragArea.dragging ? dragPreviewTime : keyTime
                        x: (displayTime - root.viewStart) * root.pxPerSec - width / 2
                        anchors.verticalCenter: parent.verticalCenter
                        width: 10; height: 10
                        rotation: 45
                        color: dragArea.dragging
                               ? "#ff8855"
                               : (Math.abs(keyTime * 1000 - AnimationControlController.selectedTick) < 1
                                  ? "#ff4444" : "#ffcc00")
                        border.color: AnimationControlController.borderColor

                        MouseArea {
                            id: dragArea
                            anchors.fill: parent
                            anchors.margins: -3 // larger hit area
                            cursorShape: Qt.SizeHorCursor
                            preventStealing: true
                            property bool dragging: false
                            property real pressX: 0
                            property real originalTime: 0
                            // Single press snapshot; release commits one move.
                            onPressed: function(mouse) {
                                dragging = true
                                pressX = mouse.x
                                originalTime = parent.keyTime
                                parent.dragPreviewTime = parent.keyTime
                                // Selecting the diamond's bone + jumping the
                                // playhead is the natural "click" outcome —
                                // do it on press so it works even if the user
                                // drags slightly afterwards.
                                AnimationControlController.selectBone(rowDelegate.boneName)
                                AnimationControlController.sliderValue =
                                        Math.round(parent.keyTime * 1000)
                            }
                            onPositionChanged: function(mouse) {
                                if (!dragging) return
                                var dx = mouse.x - pressX
                                var dt = dx / root.pxPerSec
                                var target = originalTime + dt
                                if (target < 0) target = 0
                                var len = AnimationControlController.animationLength
                                if (target > len) target = len
                                parent.dragPreviewTime = target
                            }
                            onReleased: function(mouse) {
                                if (!dragging) return
                                dragging = false
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

    // ── Wheel zoom + middle-drag pan over the timeline area ──────────────────
    MouseArea {
        anchors.left: parent.left; anchors.leftMargin: root.leftStripWidth
        anchors.top: header.visible ? header.bottom : parent.top
        anchors.right: parent.right; anchors.bottom: parent.bottom
        acceptedButtons: Qt.MiddleButton
        propagateComposedEvents: true
        property real panStartX: 0
        property real panStartView: 0
        onPressed: function(mouse) {
            if (mouse.button === Qt.MiddleButton) {
                panStartX = mouse.x
                panStartView = root.viewStart
                mouse.accepted = true
            } else { mouse.accepted = false }
        }
        onPositionChanged: function(mouse) {
            if (!pressed) return
            var dx = mouse.x - panStartX
            root.viewStart = panStartView - dx / root.pxPerSec
            if (root.viewStart < 0) root.viewStart = 0
        }

        WheelHandler {
            // Zoom around the cursor: keep the time under the cursor stationary.
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
