import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import PropertiesPanel 1.0
import ThemeManager 1.0

// UV layout viewer with component selection (issues #459 / #460).
Rectangle {
    id: root
    color: ThemeManager.panelColor
    focus: true

    // panU/panV = UV coordinate at the canvas centre; zoom = pixels per UV unit.
    property real panU: 0.5
    property real panV: 0.5
    property real zoom: 200.0

    property var triCache: []
    property int cachedRevision: -1
    property int cachedSelectionRevision: -1
    property var selVertCache: []
    property var selEdgeCache: []
    property var selFaceCache: []
    property var ctxIslandCache: []

    property bool draggingSelect: false
    property real dragStartX: 0
    property real dragStartY: 0
    property real dragEndX: 0
    property real dragEndY: 0

    readonly property int modNone: 0
    readonly property int modShift: 0x02000000
    readonly property int modCtrl: 0x04000000

    function uvToScreen(u, v) {
        return Qt.point(
            (u - panU) * zoom + viewCanvas.width * 0.5,
            (panV - v) * zoom + viewCanvas.height * 0.5
        )
    }

    function screenToUv(x, y) {
        return Qt.point(
            (x - viewCanvas.width * 0.5) / zoom + panU,
            panV - (y - viewCanvas.height * 0.5) / zoom
        )
    }

    function rebuildTriangleCache() {
        if (UVEditorController.meshRevision === cachedRevision
                && UVEditorController.selectionRevision === cachedSelectionRevision)
            return
        cachedRevision = UVEditorController.meshRevision
        cachedSelectionRevision = UVEditorController.selectionRevision
        triCache = UVEditorController.triangles()
        selVertCache = UVEditorController.selectionVertices()
        selEdgeCache = UVEditorController.selectionEdges()
        selFaceCache = UVEditorController.selectionFaces()
        ctxIslandCache = UVEditorController.contextIslandFaces()
        viewCanvas.requestPaint()
    }

    function pickRadiusUv() {
        return 8.0 / Math.max(1, root.zoom)
    }

    function eventModifiers(event) {
        let m = modNone
        if (event.modifiers & Qt.ShiftModifier)
            m |= modShift
        if (event.modifiers & Qt.ControlModifier)
            m |= modCtrl
        return m
    }

    function resetView() {
        const availW = Math.max(1, viewCanvas.width * 0.9)
        const availH = Math.max(1, viewCanvas.height * 0.9)
        panU = 0.5
        panV = 0.5
        zoom = Math.min(availW, availH)
        viewCanvas.requestPaint()
    }

    function fitToView() {
        if (!UVEditorController.hasMesh)
            return
        const b = UVEditorController.uvBounds
        const pad = 0.05
        const spanU = Math.max(b.width, 1e-4)
        const spanV = Math.max(b.height, 1e-4)
        const cx = b.x + spanU * 0.5
        const cy = b.y + spanV * 0.5
        const availW = Math.max(1, viewCanvas.width * 0.9)
        const availH = Math.max(1, viewCanvas.height * 0.9)
        panU = cx
        panV = cy
        zoom = Math.min(availW / (spanU + pad * 2), availH / (spanV + pad * 2))
        viewCanvas.requestPaint()
    }

    Connections {
        target: UVEditorController
        function onMeshDataChanged() {
            const prevRevision = root.cachedRevision
            root.rebuildTriangleCache()
            if (UVEditorController.meshRevision !== prevRevision
                    && UVEditorController.hasMesh)
                Qt.callLater(root.fitToView)
        }
        function onFitToViewRequested() { root.fitToView() }
        function onShowTextureBackgroundChanged() { viewCanvas.requestPaint() }
        function onUvSelectionChanged() { root.rebuildTriangleCache() }
        function onSelectionModeChanged() { viewCanvas.requestPaint() }
    }

    Component.onCompleted: {
        rebuildTriangleCache()
        Qt.callLater(fitToView)
    }

    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_F) {
            fitToView()
            event.accepted = true
        } else if (event.key === Qt.Key_Home) {
            resetView()
            event.accepted = true
        } else if (event.key === Qt.Key_1) {
            UVEditorController.selectionMode = 0
            event.accepted = true
        } else if (event.key === Qt.Key_2) {
            UVEditorController.selectionMode = 1
            event.accepted = true
        } else if (event.key === Qt.Key_3) {
            UVEditorController.selectionMode = 2
            event.accepted = true
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 4
        spacing: 3

        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            Text {
                text: UVEditorController.statusText
                color: ThemeManager.textColor
                font.pixelSize: 11
                elide: Text.ElideRight
                Layout.fillWidth: true
            }

            Text {
                text: UVEditorController.hasMesh
                      ? (UVEditorController.islandCount + " islands")
                      : ""
                color: ThemeManager.disabledTextColor
                font.pixelSize: 10
            }

            Row {
                spacing: 2
                Repeater {
                    model: [
                        { label: "V", mode: 0, tip: "UV Vertex (1)" },
                        { label: "E", mode: 1, tip: "UV Edge (2)" },
                        { label: "F", mode: 2, tip: "UV Face (3)" }
                    ]
                    delegate: Rectangle {
                        width: 20; height: 18; radius: 3
                        color: UVEditorController.selectionMode === modelData.mode
                            ? ThemeManager.highlightColor
                            : ThemeManager.inputColor
                        border.color: ThemeManager.borderColor
                        border.width: 1
                        ToolTip.visible: modeMa.containsMouse
                        ToolTip.text: modelData.tip
                        Text {
                            anchors.centerIn: parent
                            text: modelData.label
                            color: ThemeManager.textColor
                            font.pixelSize: 10
                            font.bold: UVEditorController.selectionMode === modelData.mode
                        }
                        MouseArea {
                            id: modeMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: UVEditorController.selectionMode = modelData.mode
                        }
                    }
                }
            }

            Text {
                text: "Channel"
                color: ThemeManager.disabledTextColor
                font.pixelSize: 10
            }

            ThemedComboBox {
                id: channelBox
                Layout.preferredWidth: 58
                model: ["UV0", "UV1"]
                currentIndex: UVEditorController.uvChannel
                onActivated: UVEditorController.uvChannel = currentIndex
            }

            Connections {
                target: UVEditorController
                function onUvChannelChanged() {
                    channelBox.currentIndex = UVEditorController.uvChannel
                }
            }

            Row {
                spacing: 4
                anchors.verticalCenter: parent.verticalCenter
                Rectangle {
                    width: 18; height: 18; radius: 3
                    color: UVEditorController.showTextureBackground
                        ? ThemeManager.highlightColor
                        : ThemeManager.inputColor
                    border.color: ThemeManager.borderColor
                    border.width: 1
                    anchors.verticalCenter: parent.verticalCenter
                    Text {
                        anchors.centerIn: parent
                        text: UVEditorController.showTextureBackground ? "\u2713" : ""
                        color: ThemeManager.textColor
                        font.pixelSize: 10
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: UVEditorController.showTextureBackground
                            = !UVEditorController.showTextureBackground
                    }
                }
                Text {
                    text: "Texture"
                    color: ThemeManager.textColor
                    font.pixelSize: 11
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            Text {
                text: "Fit"
                color: ThemeManager.textColor
                font.pixelSize: 11
                font.underline: fitMa.containsMouse
                MouseArea {
                    id: fitMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.fitToView()
                }
            }
            Text {
                text: "100%"
                color: ThemeManager.textColor
                font.pixelSize: 11
                font.underline: resetMa.containsMouse
                MouseArea {
                    id: resetMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.resetView()
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: ThemeManager.inputColor
            border.color: ThemeManager.borderColor
            border.width: 1
            clip: true

            // Texture mapped into 0–1 UV space (same transform as wireframe).
            Image {
                id: texBg
                visible: UVEditorController.showTextureBackground
                         && UVEditorController.textureBackgroundSource.length > 0
                         && UVEditorController.hasMesh
                x: viewCanvas.x + Math.min(root.uvToScreen(0, 1).x, root.uvToScreen(1, 0).x)
                y: viewCanvas.y + Math.min(root.uvToScreen(0, 1).y, root.uvToScreen(1, 0).y)
                width: Math.abs(root.uvToScreen(1, 0).x - root.uvToScreen(0, 1).x)
                height: Math.abs(root.uvToScreen(0, 0).y - root.uvToScreen(0, 1).y)
                source: UVEditorController.textureBackgroundSource
                fillMode: Image.Stretch
                opacity: 0.65
                smooth: true
                cache: false
            }

            Canvas {
                id: viewCanvas
                anchors.fill: parent
                anchors.margins: 1
                renderTarget: Canvas.Image

                onWidthChanged: viewCanvas.requestPaint()
                onHeightChanged: viewCanvas.requestPaint()

                onPaint: {
                    const ctx = getContext("2d")
                    ctx.clearRect(0, 0, width, height)

                    if (!UVEditorController.showTextureBackground
                        || UVEditorController.textureBackgroundSource.length === 0) {
                        ctx.fillStyle = Qt.rgba(ThemeManager.inputColor.r,
                                                ThemeManager.inputColor.g,
                                                ThemeManager.inputColor.b, 1.0)
                        ctx.fillRect(0, 0, width, height)
                    }

                    drawGrid(ctx)
                    drawContextIslands(ctx)
                    drawTriangles(ctx)
                    drawSelection(ctx)
                    drawUnitBoundary(ctx)
                }

                function drawGrid(ctx) {
                    ctx.save()
                    ctx.lineWidth = 1
                    ctx.strokeStyle = Qt.rgba(ThemeManager.borderColor.r,
                                              ThemeManager.borderColor.g,
                                              ThemeManager.borderColor.b, 0.35)
                    for (let i = 0; i <= 10; ++i) {
                        const t = i * 0.1
                        const a = root.uvToScreen(t, 0)
                        const b = root.uvToScreen(t, 1)
                        ctx.beginPath()
                        ctx.moveTo(a.x, a.y)
                        ctx.lineTo(b.x, b.y)
                        ctx.stroke()
                        const c = root.uvToScreen(0, t)
                        const d = root.uvToScreen(1, t)
                        ctx.beginPath()
                        ctx.moveTo(c.x, c.y)
                        ctx.lineTo(d.x, d.y)
                        ctx.stroke()
                    }
                    ctx.restore()
                }

                function drawUnitBoundary(ctx) {
                    const p0 = root.uvToScreen(0, 0)
                    const p1 = root.uvToScreen(1, 1)
                    const x = Math.min(p0.x, p1.x)
                    const y = Math.min(p0.y, p1.y)
                    const w = Math.abs(p1.x - p0.x)
                    const h = Math.abs(p1.y - p0.y)
                    ctx.save()
                    ctx.lineWidth = 2
                    ctx.strokeStyle = ThemeManager.accentColor
                    ctx.strokeRect(x, y, w, h)
                    ctx.restore()
                }

                function drawContextIslands(ctx) {
                    if (ctxIslandCache.length === 0)
                        return
                    ctx.save()
                    for (let i = 0; i < ctxIslandCache.length; ++i) {
                        const t = ctxIslandCache[i]
                        const p0 = root.uvToScreen(t.u0, t.v0)
                        const p1 = root.uvToScreen(t.u1, t.v1)
                        const p2 = root.uvToScreen(t.u2, t.v2)
                        ctx.beginPath()
                        ctx.moveTo(p0.x, p0.y)
                        ctx.lineTo(p1.x, p1.y)
                        ctx.lineTo(p2.x, p2.y)
                        ctx.closePath()
                        ctx.fillStyle = Qt.rgba(ThemeManager.accentColor.r,
                                                ThemeManager.accentColor.g,
                                                ThemeManager.accentColor.b, 0.18)
                        ctx.fill()
                    }
                    ctx.restore()
                }

                function drawSelection(ctx) {
                    ctx.save()
                    for (let i = 0; i < selFaceCache.length; ++i) {
                        const t = selFaceCache[i]
                        const p0 = root.uvToScreen(t.u0, t.v0)
                        const p1 = root.uvToScreen(t.u1, t.v1)
                        const p2 = root.uvToScreen(t.u2, t.v2)
                        ctx.beginPath()
                        ctx.moveTo(p0.x, p0.y)
                        ctx.lineTo(p1.x, p1.y)
                        ctx.lineTo(p2.x, p2.y)
                        ctx.closePath()
                        ctx.fillStyle = Qt.rgba(ThemeManager.highlightColor.r,
                                                ThemeManager.highlightColor.g,
                                                ThemeManager.highlightColor.b, 0.35)
                        ctx.fill()
                    }
                    ctx.lineWidth = 2
                    ctx.strokeStyle = ThemeManager.highlightColor
                    for (let i = 0; i < selEdgeCache.length; ++i) {
                        const e = selEdgeCache[i]
                        const a = root.uvToScreen(e.u0, e.v0)
                        const b = root.uvToScreen(e.u1, e.v1)
                        ctx.beginPath()
                        ctx.moveTo(a.x, a.y)
                        ctx.lineTo(b.x, b.y)
                        ctx.stroke()
                    }
                    const r = 4
                    ctx.fillStyle = ThemeManager.highlightColor
                    for (let i = 0; i < selVertCache.length; ++i) {
                        const v = selVertCache[i]
                        const p = root.uvToScreen(v.u, v.v)
                        ctx.beginPath()
                        ctx.arc(p.x, p.y, r, 0, Math.PI * 2)
                        ctx.fill()
                    }
                    ctx.restore()
                }

                function drawTriangles(ctx) {
                    if (!UVEditorController.hasMesh)
                        return
                    ctx.save()
                    for (let i = 0; i < root.triCache.length; ++i) {
                        const t = root.triCache[i]
                        const p0 = root.uvToScreen(t.u0, t.v0)
                        const p1 = root.uvToScreen(t.u1, t.v1)
                        const p2 = root.uvToScreen(t.u2, t.v2)
                        ctx.beginPath()
                        ctx.moveTo(p0.x, p0.y)
                        ctx.lineTo(p1.x, p1.y)
                        ctx.lineTo(p2.x, p2.y)
                        ctx.closePath()
                        ctx.fillStyle = t.color
                        ctx.fill()
                        ctx.strokeStyle = Qt.rgba(ThemeManager.textColor.r,
                                                  ThemeManager.textColor.g,
                                                  ThemeManager.textColor.b, 0.65)
                        ctx.lineWidth = 1
                        ctx.stroke()
                    }
                    ctx.restore()
                }
            }

            Text {
                anchors.centerIn: parent
                visible: !UVEditorController.hasMesh
                text: "Select a mesh to view its UV layout."
                color: ThemeManager.disabledTextColor
                font.pixelSize: 12
            }

            // Drag marquee for box select (issue #460).
            Canvas {
                id: marqueeCanvas
                anchors.fill: viewCanvas
                visible: root.draggingSelect
                renderTarget: Canvas.Immediate
                onPaint: {
                    const ctx = getContext("2d")
                    ctx.clearRect(0, 0, width, height)
                    if (!root.draggingSelect)
                        return
                    const x = Math.min(root.dragStartX, root.dragEndX)
                    const y = Math.min(root.dragStartY, root.dragEndY)
                    const w = Math.abs(root.dragEndX - root.dragStartX)
                    const h = Math.abs(root.dragEndY - root.dragStartY)
                    ctx.strokeStyle = ThemeManager.highlightColor
                    ctx.lineWidth = 1
                    ctx.setLineDash([4, 3])
                    ctx.strokeRect(x, y, w, h)
                    ctx.fillStyle = Qt.rgba(ThemeManager.highlightColor.r,
                                            ThemeManager.highlightColor.g,
                                            ThemeManager.highlightColor.b, 0.12)
                    ctx.fillRect(x, y, w, h)
                }
            }

            MouseArea {
                id: selectMa
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                hoverEnabled: true
                preventStealing: false

                property real lastX: 0
                property real lastY: 0
                property int activeModifiers: modNone

                onPressed: function(mouse) {
                    activeModifiers = eventModifiers(mouse)
                    if (mouse.button === Qt.MiddleButton) {
                        lastX = mouse.x
                        lastY = mouse.y
                        return
                    }
                    root.draggingSelect = true
                    root.dragStartX = mouse.x
                    root.dragStartY = mouse.y
                    root.dragEndX = mouse.x
                    root.dragEndY = mouse.y
                    marqueeCanvas.requestPaint()
                }
                onPositionChanged: function(mouse) {
                    if (mouse.buttons & Qt.MiddleButton) {
                        const du = (mouse.x - lastX) / root.zoom
                        const dv = (mouse.y - lastY) / root.zoom
                        root.panU -= du
                        root.panV += dv
                        lastX = mouse.x
                        lastY = mouse.y
                        viewCanvas.requestPaint()
                        return
                    }
                    if (root.draggingSelect) {
                        root.dragEndX = mouse.x
                        root.dragEndY = mouse.y
                        marqueeCanvas.requestPaint()
                    }
                }
                onReleased: function(mouse) {
                    if (mouse.button === Qt.MiddleButton)
                        return
                    if (!root.draggingSelect)
                        return
                    root.draggingSelect = false
                    marqueeCanvas.requestPaint()

                    const dx = mouse.x - root.dragStartX
                    const dy = mouse.y - root.dragStartY
                    const dist = Math.sqrt(dx * dx + dy * dy)
                    const mods = activeModifiers

                    if (dist < 4) {
                        const uv = root.screenToUv(mouse.x - viewCanvas.x, mouse.y - viewCanvas.y)
                        UVEditorController.pickAt(uv.x, uv.y, mods, root.pickRadiusUv())
                    } else {
                        const a = root.screenToUv(root.dragStartX - viewCanvas.x,
                                                  root.dragStartY - viewCanvas.y)
                        const b = root.screenToUv(root.dragEndX - viewCanvas.x,
                                                  root.dragEndY - viewCanvas.y)
                        UVEditorController.boxSelect(a.x, a.y, b.x, b.y, mods)
                    }
                    root.rebuildTriangleCache()
                }
            }

            WheelHandler {
                target: null
                onWheel: function(event) {
                    const uv = root.screenToUv(event.x, event.y)
                    const factor = event.angleDelta.y > 0 ? 1.12 : 1.0 / 1.12
                    root.zoom = Math.max(20, Math.min(8000, root.zoom * factor))
                    const after = root.screenToUv(event.x, event.y)
                    root.panU += uv.x - after.x
                    root.panV += uv.y - after.y
                    viewCanvas.requestPaint()
                    event.accepted = true
                }
            }
        }
    }
}
