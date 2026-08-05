import QtQuick
import QtQuick.Window
import PropertiesPanel 1.0

/**
 * Paint v2 Slice A (#544) — custom gradient ramp editor.
 */
Window {
    id: rampWin
    title: "Gradient Ramp Editor"
    width: 520
    height: 320
    minimumWidth: 420
    minimumHeight: 260
    flags: Qt.Dialog | Qt.WindowCloseButtonHint
    modality: Qt.ApplicationModal
    color: PropertiesPanelController.panelColor

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
            rampCanvas.requestPaint()
        }
    }

    onStopsChanged: rampCanvas.requestPaint()
    onSteppedChanged: rampCanvas.requestPaint()

    function pushStops() {
        TexturePaintController.setActiveRampStops(stops, stepped)
    }

    function stopColorFromObj(s) {
        if (!s) return "#888888"
        const r = Math.round((s.r || 0) * 255)
        const g = Math.round((s.g || 0) * 255)
        const b = Math.round((s.b || 0) * 255)
        return "#" + r.toString(16).padStart(2, "0")
                   + g.toString(16).padStart(2, "0")
                   + b.toString(16).padStart(2, "0")
    }

    function stopColor(i) {
        if (i < 0 || i >= stops.length) return "#888888"
        return stopColorFromObj(stops[i])
    }

    function sampleRampAt(stopsList, t, isStepped) {
        if (!stopsList || stopsList.length === 0) return "#808080"
        const sorted = stopsList.slice().sort(function(a, b) { return a.t - b.t })
        if (sorted.length === 1) return stopColorFromObj(sorted[0])
        t = Math.max(0, Math.min(1, t))
        if (t <= sorted[0].t) return stopColorFromObj(sorted[0])
        if (t >= sorted[sorted.length - 1].t) return stopColorFromObj(sorted[sorted.length - 1])

        if (isStepped) {
            for (let i = sorted.length - 1; i >= 0; --i) {
                if (t >= sorted[i].t) return stopColorFromObj(sorted[i])
            }
            return stopColorFromObj(sorted[0])
        }

        for (let i = 0; i < sorted.length - 1; ++i) {
            const a = sorted[i]
            const b = sorted[i + 1]
            if (t >= a.t && t <= b.t) {
                const span = b.t - a.t
                const u = span < 1e-6 ? 0.0 : (t - a.t) / span
                const r = Math.round(((a.r || 0) + ((b.r || 0) - (a.r || 0)) * u) * 255)
                const g = Math.round(((a.g || 0) + ((b.g || 0) - (a.g || 0)) * u) * 255)
                const bb = Math.round(((a.b || 0) + ((b.b || 0) - (a.b || 0)) * u) * 255)
                return "#" + r.toString(16).padStart(2, "0")
                           + g.toString(16).padStart(2, "0")
                           + bb.toString(16).padStart(2, "0")
            }
        }
        return stopColorFromObj(sorted[sorted.length - 1])
    }

    component InspectorLabel: Text {
        color: PropertiesPanelController.textColor
        font.pixelSize: 11
    }

    component InspectorButton: Rectangle {
        id: btn
        property string label: ""
        property bool buttonEnabled: true
        signal clicked()
        implicitWidth: labelText.implicitWidth + 20
        implicitHeight: 24
        radius: 3
        color: btnMa.containsMouse && buttonEnabled
            ? PropertiesPanelController.highlightColor
            : PropertiesPanelController.headerColor
        border.color: PropertiesPanelController.borderColor
        border.width: 1
        opacity: buttonEnabled ? 1.0 : 0.45
        Text {
            id: labelText
            anchors.centerIn: parent
            text: btn.label
            color: PropertiesPanelController.textColor
            font.pixelSize: 11
        }
        MouseArea {
            id: btnMa
            anchors.fill: parent
            hoverEnabled: true
            enabled: btn.buttonEnabled
            cursorShape: btn.buttonEnabled ? Qt.PointingHandCursor : Qt.ForbiddenCursor
            onClicked: btn.clicked()
        }
    }

    component InspectorTextField: Rectangle {
        id: tfRoot
        property alias text: input.text
        signal editedText(string newText)
        implicitHeight: 24
        implicitWidth: 160
        color: PropertiesPanelController.inputColor
        border.color: input.activeFocus
            ? PropertiesPanelController.highlightColor
            : PropertiesPanelController.borderColor
        border.width: input.activeFocus ? 2 : 1
        radius: 3
        TextInput {
            id: input
            anchors.fill: parent
            anchors.leftMargin: 6
            anchors.rightMargin: 6
            color: PropertiesPanelController.textColor
            font.pixelSize: 11
            verticalAlignment: TextInput.AlignVCenter
            selectByMouse: true
            clip: true
            onTextEdited: tfRoot.editedText(text)
        }
    }

    component InspectorCheckBox: Item {
        id: cbRoot
        property string label: ""
        property bool checked: false
        signal toggled(bool on)
        implicitWidth: labelRow.implicitWidth
        implicitHeight: 22
        Row {
            id: labelRow
            spacing: 6
            anchors.verticalCenter: parent.verticalCenter
            Rectangle {
                width: 16; height: 16; radius: 2
                anchors.verticalCenter: parent.verticalCenter
                color: cbRoot.checked
                    ? PropertiesPanelController.highlightColor
                    : PropertiesPanelController.inputColor
                border.color: PropertiesPanelController.borderColor
                border.width: 1
                Text {
                    anchors.centerIn: parent
                    visible: cbRoot.checked
                    text: "✓"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 12
                    font.bold: true
                }
            }
            InspectorLabel {
                visible: cbRoot.label.length > 0
                anchors.verticalCenter: parent.verticalCenter
                text: cbRoot.label
            }
        }
        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: cbRoot.toggled(!cbRoot.checked)
        }
    }

    Column {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 10

        InspectorLabel {
            text: "Drag stops along the ramp. Double-click the strip to add a stop."
            width: parent.width
            wrapMode: Text.Wrap
            opacity: 0.75
        }

        Item {
            id: stripArea
            width: parent.width
            height: 48

            Rectangle {
                id: stripFrame
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: 28
                color: PropertiesPanelController.inputColor
                border.color: PropertiesPanelController.borderColor
                border.width: 1
                radius: 3
                clip: true

                Canvas {
                    id: rampCanvas
                    anchors.fill: parent
                    anchors.margins: 1
                    renderTarget: Canvas.FramebufferObject
                    renderStrategy: Canvas.Cooperative
                    onPaint: {
                        const ctx = getContext("2d")
                        ctx.clearRect(0, 0, width, height)
                        const w = Math.max(1, Math.floor(width))
                        for (let x = 0; x < w; ++x) {
                            const t = w === 1 ? 0.0 : x / (w - 1)
                            ctx.fillStyle = sampleRampAt(stops, t, stepped)
                            ctx.fillRect(x, 0, 1, height)
                        }
                    }
                    Component.onCompleted: requestPaint()
                }

                MouseArea {
                    anchors.fill: parent
                    onDoubleClicked: function(mouse) {
                        const t = Math.max(0, Math.min(1, mouse.x / Math.max(1, width)))
                        let r = 0.5, g = 0.5, b = 0.5, a = 1.0
                        if (stops.length >= 2) {
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
                delegate: Rectangle {
                    id: stopHandle
                    required property var modelData
                    required property int index
                    width: 12; height: 18; radius: 2
                    y: 30
                    x: modelData.t * stripArea.width - width / 2
                    color: stopColor(index)
                    border.color: index === selectedStop
                        ? PropertiesPanelController.highlightColor
                        : PropertiesPanelController.borderColor
                    border.width: index === selectedStop ? 2 : 1

                    MouseArea {
                        id: dragArea
                        anchors.fill: parent
                        drag.target: stopHandle
                        drag.axis: Drag.XAxis
                        drag.minimumX: -stopHandle.width / 2
                        drag.maximumX: stripArea.width - stopHandle.width / 2
                        cursorShape: Qt.SizeHorCursor
                        onPressed: {
                            selectedStop = index
                            stopHandle.z = 1
                        }
                        onReleased: {
                            stopHandle.z = 0
                            const t = Math.max(0, Math.min(1,
                                (stopHandle.x + stopHandle.width / 2) / Math.max(1, stripArea.width)))
                            const next = stops.slice()
                            const cur = next[index]
                            next[index] = {
                                t: t,
                                r: cur.r, g: cur.g, b: cur.b, a: cur.a
                            }
                            next.sort(function(a, b) { return a.t - b.t })
                            stops = next
                            selectedStop = next.findIndex(function(s) {
                                return Math.abs(s.t - t) < 1e-4
                            })
                            pushStops()
                        }
                    }
                }
            }
        }

        Row {
            spacing: 8
            width: parent.width
            InspectorLabel {
                text: "Name:"
                width: 40
                anchors.verticalCenter: parent.verticalCenter
            }
            InspectorTextField {
                id: nameField
                width: Math.max(120, parent.width - 40 - 8 - steppedBox.implicitWidth - 8)
                text: rampWin.rampName
                onEditedText: function(t) { rampWin.rampName = t }
            }
            InspectorCheckBox {
                id: steppedBox
                label: "Stepped"
                checked: rampWin.stepped
                onToggled: function(on) {
                    rampWin.stepped = on
                    pushStops()
                }
            }
        }

        Flow {
            width: parent.width
            spacing: 8
            InspectorButton {
                label: "Recolour Stop"
                buttonEnabled: selectedStop >= 0 && selectedStop < stops.length
                onClicked: {
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
            InspectorButton {
                label: "Delete Stop"
                buttonEnabled: stops.length > 2 && selectedStop >= 0 && selectedStop < stops.length
                onClicked: {
                    const next = stops.slice()
                    next.splice(selectedStop, 1)
                    stops = next
                    selectedStop = Math.min(selectedStop, next.length - 1)
                    pushStops()
                }
            }
            InspectorButton {
                label: "Save Ramp"
                onClicked: {
                    const name = nameField.text.trim().length > 0
                        ? nameField.text.trim() : "Custom"
                    if (TexturePaintController.saveCustomRamp(name, stops, stepped))
                        rampWin.rampName = name
                }
            }
            InspectorButton {
                label: "Close"
                onClicked: rampWin.close()
            }
        }
    }
}
