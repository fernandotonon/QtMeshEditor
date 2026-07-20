import QtQuick
import QtQuick.Window
import QtQuick.Controls
import PropertiesPanel 1.0

/**
 * Paint v2 Slice A (#544) — custom gradient ramp editor.
 *
 * Gradient strip with draggable colour stops. Stops can be added
 * (double-click the strip), deleted (Delete / Backspace), repositioned
 * (drag), and recoloured (click a stop → colour dialog via the
 * "Recolour" button). Save writes a named JSON ramp into
 * `<AppData>/paint/ramps/`.
 */
Window {
    id: rampWin
    title: "Gradient Ramp Editor"
    width: 480
    height: 280
    minimumWidth: 360
    minimumHeight: 220
    color: "#1e1e1e"
    flags: Qt.Window | Qt.WindowCloseButtonHint

    property var stops: TexturePaintController.activeRampStops
    property bool stepped: TexturePaintController.gradientStepped
    property int selectedStop: 0
    property string rampName: TexturePaintController.useFgBgRamp
        ? "Custom"
        : TexturePaintController.activeRampName

    Connections {
        target: TexturePaintController
        function onGradientChanged() {
            rampWin.stops = TexturePaintController.activeRampStops
            rampWin.stepped = TexturePaintController.gradientStepped
            if (!TexturePaintController.useFgBgRamp)
                rampWin.rampName = TexturePaintController.activeRampName
        }
    }

    function pushStops() {
        TexturePaintController.setActiveRampStops(stops, stepped)
    }

    function stopColor(i) {
        if (i < 0 || i >= stops.length) return "#888888"
        const s = stops[i]
        const r = Math.round((s.r || 0) * 255)
        const g = Math.round((s.g || 0) * 255)
        const b = Math.round((s.b || 0) * 255)
        return "#" + r.toString(16).padStart(2, "0")
                   + g.toString(16).padStart(2, "0")
                   + b.toString(16).padStart(2, "0")
    }

    Column {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 10

        Text {
            text: "Drag stops along the ramp. Double-click the strip to add a stop."
            color: "#aaaaaa"
            font.pixelSize: 11
            width: parent.width
            wrapMode: Text.Wrap
        }

        Item {
            id: stripArea
            width: parent.width
            height: 48

            Image {
                id: stripImg
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: 28
                source: TexturePaintController.rampPreviewDataUri
                fillMode: Image.Stretch
                asynchronous: false
                cache: false

                MouseArea {
                    anchors.fill: parent
                    onDoubleClicked: function(mouse) {
                        const t = Math.max(0, Math.min(1, mouse.x / Math.max(1, width)))
                        // Sample current ramp colour at t for the new stop.
                        let r = 0.5, g = 0.5, b = 0.5, a = 1.0
                        if (stops.length >= 2) {
                            // Nearest-neighbour seed from neighbouring stops.
                            let best = stops[0]
                            let bestD = Math.abs(best.t - t)
                            for (let i = 1; i < stops.length; ++i) {
                                const d = Math.abs(stops[i].t - t)
                                if (d < bestD) { best = stops[i]; bestD = d }
                            }
                            r = best.r; g = best.g; b = best.b; a = best.a
                        }
                        const next = stops.slice()
                        next.push({ t: t, r: r, g: g, b: b, a: a })
                        next.sort(function(a, b) { return a.t - b.t })
                        stops = next
                        selectedStop = next.findIndex(function(s) {
                            return Math.abs(s.t - t) < 1e-4
                        })
                        pushStops()
                    }
                }
            }

            Repeater {
                model: stops
                Rectangle {
                    width: 12; height: 18; radius: 2
                    y: 30
                    x: modelData.t * stripArea.width - width / 2
                    color: stopColor(index)
                    border.color: index === selectedStop ? "#ffffff" : "#666666"
                    border.width: index === selectedStop ? 2 : 1

                    MouseArea {
                        anchors.fill: parent
                        drag.target: parent
                        drag.axis: Drag.XAxis
                        drag.minimumX: -width / 2
                        drag.maximumX: stripArea.width - width / 2
                        cursorShape: Qt.SizeHorCursor
                        onPressed: selectedStop = index
                        onReleased: {
                            const next = stops.slice()
                            const t = Math.max(0, Math.min(1,
                                (parent.x + parent.width / 2) / Math.max(1, stripArea.width)))
                            next[index] = {
                                t: t, r: modelData.r, g: modelData.g,
                                b: modelData.b, a: modelData.a
                            }
                            next.sort(function(a, b) { return a.t - b.t })
                            stops = next
                            pushStops()
                        }
                    }
                }
            }
        }

        Row {
            spacing: 8
            Text {
                text: "Name:"
                color: "#dddddd"
                font.pixelSize: 11
                anchors.verticalCenter: parent.verticalCenter
            }
            TextField {
                id: nameField
                width: 160
                text: rampWin.rampName
                color: "#eeeeee"
                background: Rectangle {
                    color: "#2a2a2a"
                    border.color: "#555555"
                    radius: 3
                }
                onEditingFinished: rampWin.rampName = text
            }
            CheckBox {
                text: "Stepped"
                checked: rampWin.stepped
                onToggled: {
                    rampWin.stepped = checked
                    pushStops()
                }
            }
        }

        Row {
            spacing: 8
            Button {
                text: "Recolour Stop"
                enabled: selectedStop >= 0 && selectedStop < stops.length
                onClicked: {
                    // Use FG colour as a quick recolour source — the
                    // toolbar FG picker is the project's colour dialog.
                    const fg = TexturePaintController.texturePaintColor
                    const next = stops.slice()
                    const cur = next[selectedStop]
                    next[selectedStop] = {
                        t: cur.t,
                        r: fg.r,
                        g: fg.g,
                        b: fg.b,
                        a: fg.a
                    }
                    stops = next
                    pushStops()
                }
            }
            Button {
                text: "Delete Stop"
                enabled: stops.length > 2 && selectedStop >= 0 && selectedStop < stops.length
                onClicked: {
                    const next = stops.slice()
                    next.splice(selectedStop, 1)
                    stops = next
                    selectedStop = Math.min(selectedStop, next.length - 1)
                    pushStops()
                }
            }
            Button {
                text: "Save Ramp"
                onClicked: {
                    const name = nameField.text.trim().length > 0
                        ? nameField.text.trim() : "Custom"
                    if (TexturePaintController.saveCustomRamp(name, stops, stepped))
                        rampWin.rampName = name
                }
            }
            Button {
                text: "Close"
                onClicked: rampWin.close()
            }
        }
    }
}
