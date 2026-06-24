import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import PropertiesPanel 1.0
import ThemeManager 1.0

// Read-only UV layout viewer (issue #459). Software Canvas2D — no GL.
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
        if (UVEditorController.meshRevision === cachedRevision)
            return
        cachedRevision = UVEditorController.meshRevision
        triCache = UVEditorController.triangles()
        viewCanvas.requestPaint()
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
            root.rebuildTriangleCache()
            if (UVEditorController.hasMesh)
                Qt.callLater(root.fitToView)
        }
        function onFitToViewRequested() { root.fitToView() }
        function onShowTextureBackgroundChanged() { viewCanvas.requestPaint() }
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
                x: Math.min(root.uvToScreen(0, 1).x, root.uvToScreen(1, 0).x)
                y: Math.min(root.uvToScreen(0, 1).y, root.uvToScreen(1, 0).y)
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
                    drawTriangles(ctx)
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

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.MiddleButton | Qt.NoButton
                hoverEnabled: true
                preventStealing: false

                property real lastX: 0
                property real lastY: 0

                onPressed: function(mouse) {
                    if (mouse.button === Qt.MiddleButton) {
                        lastX = mouse.x
                        lastY = mouse.y
                    }
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
                    }
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
