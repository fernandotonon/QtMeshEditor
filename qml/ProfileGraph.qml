import QtQuick 2.15

// 2D graph control for the bevel profile.
//
// Exposes a single value in [0, 1] that maps to the vertical position of a
// draggable midpoint handle:
//   1.0 → top    (convex / fillet)
//   0.5 → centre (flat chamfer)
//   0.0 → bottom (concave / groove)
//
// Endpoints are fixed at (0, midline) and (1, midline). The midpoint handle
// is horizontally fixed at t = 0.5 for the MVP. Dragging it vertically
// emits `valueChanged` with the new profile value.
//
// The curve preview is drawn as a symmetric sine-like bulge whose amplitude
// is (value - 0.5) * 2, matching the sampling math in HalfEdgeMesh's bevel
// chord intermediates.
Item {
    id: root

    // Public API
    property real value: 0.5
    signal profileChanged(real v)

    // Styling
    property color backgroundColor: "#1a1a1a"
    property color borderColor: "#444"
    property color midlineColor: "#555"
    property color curveColor: "#4a9eff"
    property color endpointColor: "#888"
    property color handleColor: "#4a9eff"
    property color handleBorderColor: "#ffffff"

    implicitWidth: 180
    implicitHeight: 110

    Rectangle {
        id: background
        anchors.fill: parent
        color: root.backgroundColor
        border.color: root.borderColor
        radius: 3
    }

    // Drawing-area insets so endpoint dots and the handle don't clip against
    // the border.
    readonly property real padX: 10
    readonly property real padY: 10
    readonly property real plotLeft:   padX
    readonly property real plotRight:  width - padX
    readonly property real plotTop:    padY
    readonly property real plotBottom: height - padY
    readonly property real plotWidth:  plotRight - plotLeft
    readonly property real plotHeight: plotBottom - plotTop
    readonly property real midY:       plotTop + plotHeight * 0.5

    // Map value ∈ [0, 1] → y (flipped: 1 = top, 0 = bottom)
    function valueToY(v) {
        return plotBottom - v * plotHeight
    }
    function yToValue(y) {
        var v = (plotBottom - y) / plotHeight
        return Math.max(0.0, Math.min(1.0, v))
    }

    // Redraw curve whenever the value changes
    onValueChanged: curveCanvas.requestPaint()
    onWidthChanged:  curveCanvas.requestPaint()
    onHeightChanged: curveCanvas.requestPaint()

    Canvas {
        id: curveCanvas
        anchors.fill: parent
        antialiasing: true

        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()

            // Midline (flat baseline)
            ctx.strokeStyle = root.midlineColor
            ctx.lineWidth = 1
            ctx.setLineDash([3, 3])
            ctx.beginPath()
            ctx.moveTo(root.plotLeft,  root.midY)
            ctx.lineTo(root.plotRight, root.midY)
            ctx.stroke()
            ctx.setLineDash([])

            // Profile curve: symmetric sine bulge
            //   amplitude = (value - 0.5) * 2  ∈ [-1, 1]
            //   y(t) = midY - amplitude * (plotHeight/2) * sin(π t)
            var amp = (root.value - 0.5) * 2.0
            var steps = 48
            ctx.strokeStyle = root.curveColor
            ctx.lineWidth = 2
            ctx.beginPath()
            for (var i = 0; i <= steps; ++i) {
                var t = i / steps
                var x = root.plotLeft + t * root.plotWidth
                var y = root.midY - amp * (root.plotHeight * 0.5) * Math.sin(Math.PI * t)
                if (i === 0) ctx.moveTo(x, y)
                else         ctx.lineTo(x, y)
            }
            ctx.stroke()
        }
    }

    // Fixed endpoint dots at t = 0 and t = 1 (always on the midline)
    Rectangle {
        width: 6; height: 6; radius: 3
        color: root.endpointColor
        x: root.plotLeft - width / 2
        y: root.midY - height / 2
    }
    Rectangle {
        width: 6; height: 6; radius: 3
        color: root.endpointColor
        x: root.plotRight - width / 2
        y: root.midY - height / 2
    }

    // Draggable midpoint handle at t = 0.5 (visual only — input handled
    // by the MouseArea below, which sits on the root so its coordinate
    // frame doesn't shift when the handle moves).
    Rectangle {
        id: handle
        width: 14; height: 14; radius: 7
        color: root.handleColor
        border.color: root.handleBorderColor
        border.width: 2

        readonly property real centreX: root.plotLeft + root.plotWidth * 0.5
        x: centreX - width / 2
        y: root.valueToY(root.value) - height / 2
    }

    // Single MouseArea covering the whole widget. Clicking or dragging
    // anywhere moves the handle to that Y position. Double-click resets
    // to flat (0.5).
    MouseArea {
        id: inputArea
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.SizeVerCursor
        hoverEnabled: false
        preventStealing: true

        function applyFromMouseY(my) {
            var clamped = Math.max(root.plotTop, Math.min(root.plotBottom, my))
            var v = root.yToValue(clamped)
            if (Math.abs(v - root.value) > 1e-4) {
                root.value = v
                root.profileChanged(v)
            }
        }

        onPressed: function(mouse) {
            applyFromMouseY(mouse.y)
        }
        onPositionChanged: function(mouse) {
            if (!pressed) return
            applyFromMouseY(mouse.y)
        }
        onDoubleClicked: {
            if (Math.abs(root.value - 0.5) > 1e-4) {
                root.value = 0.5
                root.profileChanged(0.5)
            }
        }
    }
}
