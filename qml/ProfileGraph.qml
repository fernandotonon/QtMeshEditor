import QtQuick 2.15

// 2D graph control for the bevel profile. Displays N-1 vertically-
// draggable control points for N segments; endpoints are fixed on the
// midline. Click-and-drag picks the nearest point; double-click resets.
Item {
    id: root

    property var values: [0.5]
    signal pointChanged(int index, real v)
    signal resetRequested()

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
        anchors.fill: parent
        color: root.backgroundColor
        border.color: root.borderColor
        radius: 3
    }

    readonly property real padX: 10
    readonly property real padY: 10
    readonly property real plotLeft:   padX
    readonly property real plotRight:  width - padX
    readonly property real plotTop:    padY
    readonly property real plotBottom: height - padY
    readonly property real plotWidth:  plotRight - plotLeft
    readonly property real plotHeight: plotBottom - plotTop
    readonly property real midY:       plotTop + plotHeight * 0.5

    readonly property int pointCount: (values !== undefined && values !== null
                                       && values.length !== undefined)
                                      ? values.length : 0
    readonly property int segments:   pointCount + 1

    function tToX(t) { return plotLeft + t * plotWidth }
    function indexToT(i) { return i / segments }
    function valueToY(v) { return plotBottom - v * plotHeight }
    function yToValue(y) {
        var v = (plotBottom - y) / plotHeight
        return Math.max(0.0, Math.min(1.0, v))
    }

    onValuesChanged:  curveCanvas.requestPaint()
    onWidthChanged:   curveCanvas.requestPaint()
    onHeightChanged:  curveCanvas.requestPaint()

    Canvas {
        id: curveCanvas
        anchors.fill: parent
        antialiasing: true

        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()

            ctx.strokeStyle = root.midlineColor
            ctx.lineWidth = 1
            ctx.setLineDash([3, 3])
            ctx.beginPath()
            ctx.moveTo(root.plotLeft,  root.midY)
            ctx.lineTo(root.plotRight, root.midY)
            ctx.stroke()
            ctx.setLineDash([])

            ctx.strokeStyle = root.curveColor
            ctx.lineWidth = 2
            ctx.beginPath()
            ctx.moveTo(root.plotLeft, root.midY)
            var n = root.pointCount
            for (var i = 0; i < n; ++i) {
                var t = root.indexToT(i + 1)
                ctx.lineTo(root.tToX(t), root.valueToY(root.values[i]))
            }
            ctx.lineTo(root.plotRight, root.midY)
            ctx.stroke()
        }
    }

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

    Repeater {
        model: root.pointCount
        delegate: Rectangle {
            required property int index
            width: 12; height: 12; radius: 6
            color: root.handleColor
            border.color: root.handleBorderColor
            border.width: 2
            x: root.tToX(root.indexToT(index + 1)) - width / 2
            y: {
                var v = 0.5
                if (root.values && index < root.values.length)
                    v = root.values[index]
                return root.valueToY(v) - height / 2
            }
            z: 10
        }
    }

    function nearestIndex(x) {
        var n = root.pointCount
        if (n <= 0) return -1
        var best = 0
        var bestDist = Math.abs(root.tToX(root.indexToT(1)) - x)
        for (var i = 1; i < n; ++i) {
            var d = Math.abs(root.tToX(root.indexToT(i + 1)) - x)
            if (d < bestDist) { bestDist = d; best = i }
        }
        return best
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.SizeVerCursor
        preventStealing: true

        property int activeIndex: -1

        function applyY(my) {
            if (activeIndex < 0) return
            var clamped = Math.max(root.plotTop, Math.min(root.plotBottom, my))
            var v = root.yToValue(clamped)
            var curr = (root.values !== undefined && activeIndex < root.values.length)
                     ? root.values[activeIndex]
                     : 0.5
            if (Math.abs(v - curr) > 1e-4) {
                root.pointChanged(activeIndex, v)
            }
        }

        onPressed: function(mouse) {
            activeIndex = root.nearestIndex(mouse.x)
            applyY(mouse.y)
        }
        onPositionChanged: function(mouse) {
            if (!pressed) return
            applyY(mouse.y)
        }
        onReleased: { activeIndex = -1 }
        onDoubleClicked: root.resetRequested()
    }
}
