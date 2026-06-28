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

    // Inspector embed uses a fixed-height preview; the detached window is full-size.
    property bool embedded: false

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
    property var seamEdgeCache: []
    property var pinVertCache: []

    property bool draggingSelect: false
    property bool draggingTransform: false
    property real dragStartX: 0
    property real dragStartY: 0
    property real dragEndX: 0
    property real dragEndY: 0
    property string numericBuffer: ""

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
        seamEdgeCache = UVEditorController.seamEdges()
        pinVertCache = UVEditorController.pinnedVertices()
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

    /// Fit once the canvas has a real layout size (detached window opens maximized).
    function fitToViewWhenReady(retry) {
        if (viewCanvas.width > 10 && viewCanvas.height > 10) {
            if (UVEditorController.hasMesh)
                fitToView()
            else
                resetView()
            return
        }
        const n = retry === undefined ? 0 : retry
        if (n < 30)
            Qt.callLater(function() { fitToViewWhenReady(n + 1) })
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

    function transformModeLabel() {
        switch (UVEditorController.transformMode) {
        case -1: return "Select (Q)"
        case 0: return "Move (W)"
        case 1: return "Rotate (E)"
        case 2: return "Scale (R)"
        default: return ""
        }
    }

    // Match the main viewport transform shortcuts (Unity convention).
    function setTransformModeFromKey(key) {
        if (key === Qt.Key_Q)
            UVEditorController.transformMode = -1
        else if (key === Qt.Key_W || key === Qt.Key_G)
            UVEditorController.transformMode = 0
        else if (key === Qt.Key_E)
            UVEditorController.transformMode = 1
        else if (key === Qt.Key_R)
            UVEditorController.transformMode = 2
    }

    function appendNumericChar(ch) {
        if (UVEditorController.transformMode < 0)
            return
        if (ch === "." && numericBuffer.indexOf(".") >= 0)
            return
        if (ch === "-" && numericBuffer.length > 0)
            return
        numericBuffer += ch
    }

    function applyNumericBuffer() {
        if (numericBuffer.length === 0)
            return false
        const val = parseFloat(numericBuffer)
        if (isNaN(val))
            return false
        UVEditorController.applyNumericTransform(val)
        numericBuffer = ""
        return true
    }

    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_F) {
            fitToView()
            event.accepted = true
        } else if (event.key === Qt.Key_Home) {
            resetView()
            event.accepted = true
        } else if (UVEditorController.transformMode >= 0
                   && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) {
            event.accepted = applyNumericBuffer()
        } else if (UVEditorController.transformMode >= 0
                   && event.key >= Qt.Key_0 && event.key <= Qt.Key_9) {
            appendNumericChar(String.fromCharCode(event.key))
            event.accepted = true
        } else if (UVEditorController.transformMode >= 0 && event.key === Qt.Key_Minus) {
            appendNumericChar("-")
            event.accepted = true
        } else if (UVEditorController.transformMode >= 0 && event.key === Qt.Key_Period) {
            appendNumericChar(".")
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
        } else if (event.key === Qt.Key_Q || event.key === Qt.Key_W || event.key === Qt.Key_E
                   || event.key === Qt.Key_R || event.key === Qt.Key_G) {
            setTransformModeFromKey(event.key)
            event.accepted = true
        } else if (event.key === Qt.Key_Escape) {
            if (root.draggingTransform)
                UVEditorController.cancelTransformDrag()
            else
                UVEditorController.transformMode = -1
            numericBuffer = ""
            root.draggingTransform = false
            event.accepted = true
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 4
        spacing: 3

        RowLayout {
            Layout.fillWidth: true
            spacing: 4
            visible: root.embedded

            Text {
                text: UVEditorController.hasMesh
                      ? (UVEditorController.islandCount + " islands")
                      : "No mesh"
                color: ThemeManager.disabledTextColor
                font.pixelSize: 10
                Layout.fillWidth: true
            }

            Row {
                spacing: 2
                Repeater {
                    model: [
                        { label: "V", mode: 0 },
                        { label: "E", mode: 1 },
                        { label: "F", mode: 2 }
                    ]
                    delegate: Rectangle {
                        width: 20; height: 18; radius: 3
                        color: UVEditorController.selectionMode === modelData.mode
                            ? ThemeManager.highlightColor
                            : ThemeManager.inputColor
                        border.color: ThemeManager.borderColor
                        border.width: 1
                        Text {
                            anchors.centerIn: parent
                            text: modelData.label
                            color: ThemeManager.textColor
                            font.pixelSize: 10
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: UVEditorController.selectionMode = modelData.mode
                        }
                    }
                }
            }

            Text {
                text: "Fit"
                color: ThemeManager.textColor
                font.pixelSize: 10
                font.underline: embFitMa.containsMouse
                MouseArea {
                    id: embFitMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.fitToView()
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            visible: !root.embedded

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
                text: UVEditorController.hasMesh ? transformModeLabel() : ""
                color: ThemeManager.accentColor
                font.pixelSize: 10
            }

            Text {
                visible: numericBuffer.length > 0
                text: numericBuffer
                color: ThemeManager.highlightColor
                font.pixelSize: 11
                font.bold: true
            }

            Row {
                spacing: 2
                Repeater {
                    model: [
                        { label: "Q", mode: -1, tip: "Select (Q)" },
                        { label: "W", mode: 0, tip: "Move (W)" },
                        { label: "E", mode: 1, tip: "Rotate (E)" },
                        { label: "R", mode: 2, tip: "Scale (R)" }
                    ]
                    delegate: Rectangle {
                        width: 20; height: 18; radius: 3
                        color: UVEditorController.transformMode === modelData.mode
                            ? ThemeManager.accentColor
                            : ThemeManager.inputColor
                        border.color: ThemeManager.borderColor
                        border.width: 1
                        ToolTip.visible: xformMa.containsMouse
                        ToolTip.text: modelData.tip
                        Text {
                            anchors.centerIn: parent
                            text: modelData.label
                            color: ThemeManager.textColor
                            font.pixelSize: 10
                            font.bold: UVEditorController.transformMode === modelData.mode
                        }
                        MouseArea {
                            id: xformMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: UVEditorController.transformMode = modelData.mode
                        }
                    }
                }
            }

            ThemedComboBox {
                id: pivotBox
                Layout.preferredWidth: 72
                model: ["Median", "Individual", "Cursor"]
                currentIndex: UVEditorController.pivotMode
                onActivated: UVEditorController.pivotMode = currentIndex
            }

            Rectangle {
                width: 18; height: 18; radius: 3
                color: UVEditorController.snapEnabled ? ThemeManager.highlightColor : ThemeManager.inputColor
                border.color: ThemeManager.borderColor
                ToolTip.visible: snapMa.containsMouse
                ToolTip.text: "Snap (Ctrl inverts while dragging)"
                Text {
                    anchors.centerIn: parent
                    text: "Snap"
                    color: ThemeManager.textColor
                    font.pixelSize: 8
                }
                MouseArea {
                    id: snapMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: UVEditorController.snapEnabled = !UVEditorController.snapEnabled
                }
            }

            ThemedComboBox {
                id: snapBox
                Layout.preferredWidth: 64
                model: ["Grid", "Vertex", "Pixel"]
                currentIndex: UVEditorController.snapMode
                onActivated: UVEditorController.snapMode = currentIndex
            }

            Text {
                text: "Mx"
                color: ThemeManager.textColor
                font.pixelSize: 10
                font.underline: mxMa.containsMouse
                MouseArea {
                    id: mxMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: UVEditorController.mirrorSelectionX()
                }
            }
            Text {
                text: "My"
                color: ThemeManager.textColor
                font.pixelSize: 10
                font.underline: myMa.containsMouse
                MouseArea {
                    id: myMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: UVEditorController.mirrorSelectionY()
                }
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

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: UVEditorController.hasMesh && !root.embedded

            Text {
                text: "Seams"
                color: ThemeManager.disabledTextColor
                font.pixelSize: 10
            }

            Row {
                spacing: 2
                Repeater {
                    model: [
                        { label: "Pin", tip: "Pin selected UV vertices", fn: function() { UVEditorController.pinSelection() } },
                        { label: "Unpin", tip: "Unpin selected UV vertices", fn: function() { UVEditorController.unpinSelection() } },
                        { label: "Sew", tip: "Sew UVs along selected edges", fn: function() { UVEditorController.sewSelectedEdges() } },
                        { label: "Split", tip: "Split UVs along selected edges", fn: function() { UVEditorController.splitSelectedEdges() } },
                        { label: "Unwrap", tip: "Re-unwrap selected faces (respects seams + pins)", fn: function() { UVEditorController.unwrapSelectedFaces() } }
                    ]
                    delegate: Rectangle {
                        width: modelData.label === "Unpin" ? 38 : (modelData.label === "Unwrap" ? 48 : 32)
                        height: 18
                        radius: 3
                        color: seamBtnMa.pressed ? Qt.darker(ThemeManager.inputColor, 1.15)
                             : seamBtnMa.containsMouse ? Qt.lighter(ThemeManager.inputColor, 1.08)
                             : ThemeManager.inputColor
                        border.color: ThemeManager.borderColor
                        border.width: 1
                        ToolTip.visible: seamBtnMa.containsMouse
                        ToolTip.text: modelData.tip
                        Text {
                            anchors.centerIn: parent
                            text: modelData.label
                            color: ThemeManager.textColor
                            font.pixelSize: 9
                        }
                        MouseArea {
                            id: seamBtnMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                modelData.fn()
                                root.rebuildTriangleCache()
                            }
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: !root.embedded
            Layout.preferredHeight: root.embedded ? 280 : 0
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
                    drawSeamEdges(ctx)
                    drawSelection(ctx)
                    drawPinnedVertices(ctx)
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

                function drawSeamEdges(ctx) {
                    if (seamEdgeCache.length === 0)
                        return
                    ctx.save()
                    ctx.lineWidth = 2
                    ctx.strokeStyle = "#E03030"
                    for (let i = 0; i < seamEdgeCache.length; ++i) {
                        const e = seamEdgeCache[i]
                        const a = root.uvToScreen(e.u0, e.v0)
                        const b = root.uvToScreen(e.u1, e.v1)
                        ctx.beginPath()
                        ctx.moveTo(a.x, a.y)
                        ctx.lineTo(b.x, b.y)
                        ctx.stroke()
                    }
                    ctx.restore()
                }

                function drawPinnedVertices(ctx) {
                    if (pinVertCache.length === 0)
                        return
                    ctx.save()
                    const r = 5
                    ctx.fillStyle = "#FFD54A"
                    ctx.strokeStyle = "#8A6A00"
                    ctx.lineWidth = 1
                    for (let i = 0; i < pinVertCache.length; ++i) {
                        const v = pinVertCache[i]
                        const p = root.uvToScreen(v.u, v.v)
                        ctx.beginPath()
                        ctx.arc(p.x, p.y, r, 0, Math.PI * 2)
                        ctx.fill()
                        ctx.stroke()
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
                acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
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
                    if (mouse.button === Qt.RightButton) {
                        const uv = root.screenToUv(mouse.x - viewCanvas.x, mouse.y - viewCanvas.y)
                        UVEditorController.setCursorFromUv(uv.x, uv.y)
                        return
                    }
                    if (UVEditorController.transformMode >= 0
                            && UVEditorController.selectedVertexCount
                               + UVEditorController.selectedEdgeCount
                               + UVEditorController.selectedFaceCount > 0) {
                        const uv = root.screenToUv(mouse.x - viewCanvas.x, mouse.y - viewCanvas.y)
                        if (UVEditorController.beginTransformDrag(uv.x, uv.y, activeModifiers)) {
                            root.draggingTransform = true
                            return
                        }
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
                    if (root.draggingTransform && (mouse.buttons & Qt.LeftButton)) {
                        const uv = root.screenToUv(mouse.x - viewCanvas.x, mouse.y - viewCanvas.y)
                        UVEditorController.updateTransformDrag(uv.x, uv.y, activeModifiers)
                        root.rebuildTriangleCache()
                        return
                    }
                    if (root.draggingSelect) {
                        root.dragEndX = mouse.x
                        root.dragEndY = mouse.y
                        marqueeCanvas.requestPaint()
                    }
                }
                onReleased: function(mouse) {
                    if (mouse.button === Qt.MiddleButton || mouse.button === Qt.RightButton)
                        return
                    if (root.draggingTransform) {
                        UVEditorController.commitTransformDrag()
                        root.draggingTransform = false
                        root.rebuildTriangleCache()
                        return
                    }
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
