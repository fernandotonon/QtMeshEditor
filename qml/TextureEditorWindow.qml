import QtQuick
import QtQuick.Window
import QtQuick.Controls
import PropertiesPanel 1.0

/**
 * Detached, full-size texture editor window. Mirrors the texture-paint
 * 2D preview in the right inspector — same data URI, same paint pipeline
 * via TexturePaintController.beginStrokeUV/updateStrokeUV/endStrokeUV.
 * Live-syncs with the 3D viewport: paints on the detached canvas update
 * the in-engine texture in real time, and strokes done on the 3D mesh
 * update this canvas at the next preview refresh.
 *
 * Slice 3 of the paint-image-editing-tools epic.
 */
Window {
    id: editorWindow
    title: TexturePaintController.currentTextureName.length > 0
        ? ("Texture Editor — " + TexturePaintController.currentTextureName)
        : "Texture Editor"
    width: 720
    height: 760
    minimumWidth: 480
    minimumHeight: 520
    color: "#1e1e1e"
    flags: Qt.Window | Qt.WindowMinMaxButtonsHint | Qt.WindowCloseButtonHint

    // Mirror everything we need from TexturePaintController. The whole
    // window is invisible when no paint session is active — the
    // "Open Editor Window" button only enables once the session exists.
    property string previewUri: TexturePaintController.previewDataUri
    property string maskOverlayUri: TexturePaintController.maskOverlayDataUri
    property bool   hasSession: TexturePaintController.hasActiveSession
    property bool   hasMask: TexturePaintController.hasSelectionMask
    property int    maskCount: TexturePaintController.selectedPixelCount
    property real   hoverU: -1
    property real   hoverV: -1

    Connections {
        target: TexturePaintController
        function onPreviewChanged() {
            editorWindow.previewUri = TexturePaintController.previewDataUri
        }
        function onSmartSelectChanged() {
            editorWindow.maskOverlayUri = TexturePaintController.maskOverlayDataUri
            editorWindow.hasMask = TexturePaintController.hasSelectionMask
            editorWindow.maskCount = TexturePaintController.selectedPixelCount
        }
        function onSessionChanged() {
            editorWindow.hasSession = TexturePaintController.hasActiveSession
        }
        function onHoveredUVChanged(u, v) {
            editorWindow.hoverU = u
            editorWindow.hoverV = v
        }
    }

    // Top toolbar — tool selector + current FG/BG color readout, mirroring
    // what's in the main toolbar so the user doesn't need to look away.
    Row {
        id: topBar
        spacing: 6
        anchors {
            top: parent.top
            left: parent.left
            right: parent.right
            margins: 8
        }
        height: 30

        Repeater {
            model: [
                { tool: 0, label: "Paint",  glyph: "✏",  isWand: false },
                { tool: 1, label: "Erase",  glyph: "⌫",  isWand: false },
                { tool: 2, label: "Fill",   glyph: "⧉",  isWand: false },
                { tool: 3, label: "Pick",   glyph: "⊰",  isWand: false },
                { tool: 4, label: "Smudge", glyph: "∿",  isWand: false },
                { tool: 5, label: "Wand",   glyph: "",   isWand: true }
            ]
            Rectangle {
                width: 64; height: 28; radius: 3
                color: TexturePaintController.brushTool === modelData.tool
                    ? "#5b8def"
                    : (winToolMa.containsMouse ? "#3a3a3a" : "#2a2a2a")
                border.color: "#555"; border.width: 1
                // Custom wand icon — see comment in PropertiesPanel.qml
                // for the rationale (sparkle/star glyphs look like the
                // AI button).
                Canvas {
                    id: winWandIcon
                    visible: modelData.isWand
                    width: 14; height: 14
                    anchors.left: parent.left
                    anchors.leftMargin: 6
                    anchors.verticalCenter: parent.verticalCenter
                    onPaint: {
                        const ctx = getContext("2d")
                        ctx.reset()
                        ctx.strokeStyle = "white"
                        ctx.lineWidth = 1.6
                        ctx.lineCap = "round"
                        ctx.beginPath()
                        ctx.moveTo(2.5, 11.5)
                        ctx.lineTo(11.5, 2.5)
                        ctx.stroke()
                        ctx.fillStyle = "white"
                        ctx.beginPath()
                        ctx.arc(11.8, 2.2, 1.7, 0, Math.PI * 2)
                        ctx.fill()
                        ctx.beginPath()
                        ctx.arc(2.2, 11.8, 1.0, 0, Math.PI * 2)
                        ctx.fill()
                    }
                }
                Text {
                    anchors.centerIn: parent
                    visible: !modelData.isWand
                    text: modelData.glyph + " " + modelData.label
                    color: "white"; font.pixelSize: 11
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: winWandIcon.right
                    anchors.leftMargin: 4
                    visible: modelData.isWand
                    text: modelData.label
                    color: "white"; font.pixelSize: 11
                }
                MouseArea {
                    id: winToolMa
                    anchors.fill: parent; hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: TexturePaintController.brushTool = modelData.tool
                }
            }
        }

        Item { width: 16; height: 1 } // spacer

        Text {
            text: editorWindow.hasMask ? (editorWindow.maskCount + " px") : ""
            color: "#ddd"
            font.pixelSize: 11
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    // Main canvas. Inverted-letterbox layout: the canvas grows to fill,
    // preserving the texture's aspect ratio. MouseArea handles paint
    // strokes — same UV mapping as PropertiesPanel.qml.
    Rectangle {
        id: canvasBox
        anchors {
            top: topBar.bottom
            left: parent.left
            right: parent.right
            bottom: bottomBar.top
            margins: 8
        }
        color: "#101010"
        border.color: "#444"; border.width: 1

        Image {
            id: canvasImg
            anchors.centerIn: parent
            // Largest square that fits within the box.
            width:  Math.min(parent.width, parent.height) - 8
            height: width
            source: editorWindow.previewUri
            fillMode: Image.PreserveAspectFit
            smooth: false
            cache: false
            onSourceChanged: canvasImg.update()
        }
        // Mask overlay (selection bounds rendered as a yellow tint).
        Image {
            anchors.fill: canvasImg
            visible: editorWindow.hasMask
            opacity: 0.85
            source: editorWindow.maskOverlayUri
            fillMode: Image.PreserveAspectFit
            smooth: false
            cache: false
        }
        // Hover crosshair.
        Rectangle {
            visible: editorWindow.hoverU >= 0 && editorWindow.hoverV >= 0
            color: "#ff3030"
            width: 1; height: canvasImg.height
            x: canvasImg.x + Math.round(editorWindow.hoverU * canvasImg.width)
            y: canvasImg.y
        }
        Rectangle {
            visible: editorWindow.hoverU >= 0 && editorWindow.hoverV >= 0
            color: "#ff3030"
            width: canvasImg.width; height: 1
            x: canvasImg.x
            y: canvasImg.y + Math.round(editorWindow.hoverV * canvasImg.height)
        }

        MouseArea {
            id: canvasMa
            anchors.fill: canvasImg
            hoverEnabled: true
            cursorShape: Qt.CrossCursor
            preventStealing: true
            property bool dragging: false

            function uvAt(mx, my) {
                if (canvasImg.width <= 0 || canvasImg.height <= 0)
                    return null
                const u = mx / canvasImg.width
                const v = my / canvasImg.height
                if (u < 0 || u > 1 || v < 0 || v > 1) return null
                return { u: u, v: v }
            }

            onPressed: (m) => {
                const uv = uvAt(m.x, m.y)
                if (!uv) return
                dragging = TexturePaintController.beginStrokeUV(uv.u, uv.v)
                m.accepted = true
            }
            onPositionChanged: (m) => {
                const uv = uvAt(m.x, m.y)
                if (!uv) {
                    TexturePaintController.clearHoveredUV()
                    return
                }
                TexturePaintController.setHoveredUV(uv.u, uv.v)
                if (dragging)
                    TexturePaintController.updateStrokeUV(uv.u, uv.v)
            }
            onReleased: (m) => {
                if (dragging) {
                    TexturePaintController.endStrokeUV()
                    dragging = false
                }
            }
            onCanceled: {
                if (dragging) {
                    TexturePaintController.endStrokeUV()
                    dragging = false
                }
            }
            onExited: TexturePaintController.clearHoveredUV()
        }
    }

    // Bottom action bar: save / load / bake / mask actions / "open in
    // external viewer". Keeps the most-used non-stroke actions one click
    // away without polluting the main inspector.
    Row {
        id: bottomBar
        spacing: 6
        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
            margins: 8
        }
        height: 30

        Button {
            text: "Save…"
            enabled: editorWindow.hasSession
            onClicked: TexturePaintController.savePaintBufferInteractive()
        }
        Button {
            text: "Load…"
            enabled: editorWindow.hasSession
            onClicked: TexturePaintController.loadPaintBufferInteractive()
        }
        Button {
            // Explicit destructive write — the painted pixels only hit
            // disk when the user clicks this (or saves a new file).
            // Strokes alone are kept in memory.
            text: "Save to Original"
            enabled: editorWindow.hasSession
            ToolTip.text: "Overwrite the texture's source file on disk.\nCannot be undone outside the editor."
            ToolTip.visible: hovered
            ToolTip.delay: 400
            onClicked: TexturePaintController.bakeToOriginalFile()
        }
        Button {
            text: "Fill FG"
            enabled: editorWindow.hasMask
            onClicked: TexturePaintController.fillMaskWithFG()
        }
        Button {
            text: "Fill BG"
            enabled: editorWindow.hasMask
            onClicked: TexturePaintController.fillMaskWithBG()
        }
        Button {
            text: "Delete"
            enabled: editorWindow.hasMask
            onClicked: TexturePaintController.deleteMaskPixels()
        }
        Button {
            text: "Invert"
            enabled: editorWindow.hasSession
            onClicked: TexturePaintController.invertSelectionMask()
        }
        Button {
            text: "All"
            enabled: editorWindow.hasSession
            onClicked: TexturePaintController.selectAllMask()
        }
        Button {
            text: "None"
            enabled: editorWindow.hasMask
            onClicked: TexturePaintController.clearSelectionMask()
        }
    }
}
