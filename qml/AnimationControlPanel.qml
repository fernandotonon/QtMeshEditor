import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AnimationControl 1.0

Column {
    id: root
    width: parent ? parent.width : 300
    spacing: 6
    padding: 0

    // ── KFTransformField — mirrors TransformField.qml but uses AnimationControl theme ──
    component KFTransformField: Row {
        id: kfRoot
        property string label: "X"
        property real   val: 0.0
        property color  labelColor: "#c04040"
        property bool   editable: false
        property real   step: 0.0001
        property int    decimals: 4
        signal committed(real v)

        spacing: 2
        opacity: editable ? 1.0 : 0.45

        Rectangle {
            width: 16; height: 22; radius: 2
            color: kfRoot.labelColor
            Text { anchors.centerIn: parent; text: kfRoot.label; color: "white"; font.pixelSize: 10; font.bold: true }
        }

        Rectangle {
            id: kfInputBg
            width: kfRoot.width - 18; height: 22
            color: AnimationControlController.inputColor
            border.color: kfIn.activeFocus ? kfRoot.labelColor : AnimationControlController.borderColor
            border.width: 1; radius: 2

            TextInput {
                id: kfIn
                anchors.left: parent.left; anchors.right: kfArrows.left
                anchors.top: parent.top; anchors.bottom: parent.bottom; anchors.margins: 2
                text: kfRoot.val.toFixed(kfRoot.decimals)
                color: AnimationControlController.textColor
                font.pixelSize: 11
                verticalAlignment: TextInput.AlignVCenter
                selectByMouse: true; clip: true
                readOnly: !kfRoot.editable
                validator: DoubleValidator { decimals: kfRoot.decimals + 1; notation: DoubleValidator.StandardNotation }

                onEditingFinished: { if (kfRoot.editable) { var v = parseFloat(text); if (!isNaN(v)) kfRoot.committed(v) } }
                Keys.onUpPressed:   { if (kfRoot.editable) { var v = parseFloat(text) + kfRoot.step; text = v.toFixed(kfRoot.decimals); kfRoot.committed(v) } }
                Keys.onDownPressed: { if (kfRoot.editable) { var v = parseFloat(text) - kfRoot.step; text = v.toFixed(kfRoot.decimals); kfRoot.committed(v) } }

                Connections {
                    target: AnimationControlController
                    function onCurrentKeyframeChanged() { if (!kfIn.activeFocus) kfIn.text = kfRoot.val.toFixed(kfRoot.decimals) }
                }
            }

            Column {
                id: kfArrows
                anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom
                width: 14

                Rectangle {
                    width: parent.width; height: parent.height / 2
                    color: upMa.pressed ? Qt.darker(AnimationControlController.panelColor, 1.2)
                         : upMa.containsMouse ? Qt.lighter(AnimationControlController.panelColor, 1.2)
                         : AnimationControlController.panelColor
                    border.color: AnimationControlController.borderColor; border.width: 1
                    Text { anchors.centerIn: parent; text: "\u25B2"; font.pixelSize: 6; color: AnimationControlController.textColor }
                    MouseArea { id: upMa; anchors.fill: parent; hoverEnabled: true; enabled: kfRoot.editable
                        onClicked: { var base = parseFloat(kfIn.text); if (!isNaN(base)) { var v = base + kfRoot.step; kfIn.text = v.toFixed(kfRoot.decimals); kfRoot.committed(v) } }
                    }
                }
                Rectangle {
                    width: parent.width; height: parent.height / 2
                    color: downMa.pressed ? Qt.darker(AnimationControlController.panelColor, 1.2)
                         : downMa.containsMouse ? Qt.lighter(AnimationControlController.panelColor, 1.2)
                         : AnimationControlController.panelColor
                    border.color: AnimationControlController.borderColor; border.width: 1
                    Text { anchors.centerIn: parent; text: "\u25BC"; font.pixelSize: 6; color: AnimationControlController.textColor }
                    MouseArea { id: downMa; anchors.fill: parent; hoverEnabled: true; enabled: kfRoot.editable
                        onClicked: { var base = parseFloat(kfIn.text); if (!isNaN(base)) { var v = base - kfRoot.step; kfIn.text = v.toFixed(kfRoot.decimals); kfRoot.committed(v) } }
                    }
                }
            }
        }
    }

    // ── Toolbar button ────────────────────────────────────────────────────────
    component ToolBtn: Rectangle {
        property string label: ""
        property bool   enabled: true
        signal clicked()
        width: Math.max(28, lblT.implicitWidth + 10); height: 22; radius: 3
        color: maT.pressed ? Qt.darker(AnimationControlController.buttonColor, 1.3)
             : maT.containsMouse ? Qt.lighter(AnimationControlController.buttonColor, 1.15)
             : AnimationControlController.buttonColor
        border.color: AnimationControlController.borderColor; border.width: 1
        opacity: enabled ? 1.0 : 0.4
        Text { id: lblT; anchors.centerIn: parent; text: parent.label; color: AnimationControlController.buttonTextColor; font.pixelSize: 11 }
        MouseArea { id: maT; anchors.fill: parent; hoverEnabled: true; enabled: parent.enabled; onClicked: parent.clicked() }
    }

    // ── Animation typeahead ───────────────────────────────────────────────────
    Text { text: "Animation:"; color: AnimationControlController.textColor; font.pixelSize: 11 }

    Item {
        id: animSelector
        width: parent.width; height: 24
        property bool dropdownOpen: false

        property var flatAnims: {
            var list = []
            var tree = AnimationControlController.animationTree
            for (var i = 0; i < tree.length; i++) {
                var g = tree[i]
                for (var j = 0; j < g.animations.length; j++)
                    list.push({ label: g.entity + " / " + g.animations[j], entity: g.entity, anim: g.animations[j] })
            }
            return list
        }

        property string currentLabel: {
            var e = AnimationControlController.selectedEntityName
            var a = AnimationControlController.selectedAnimation
            return (e && a) ? (e + " / " + a) : "(none)"
        }

        Connections {
            target: AnimationControlController
            function onSelectionChanged() { animSelector.currentLabel = Qt.binding(function() {
                var e = AnimationControlController.selectedEntityName
                var a = AnimationControlController.selectedAnimation
                return (e && a) ? (e + " / " + a) : "(none)"
            })}
            function onAnimationTreeChanged() { animSelector.dropdownOpen = false }
        }

        Rectangle {
            anchors.fill: parent; radius: 3
            color: animSelectorMouse.pressed ? Qt.darker(AnimationControlController.buttonColor, 1.2)
                 : animSelectorMouse.containsMouse ? Qt.lighter(AnimationControlController.buttonColor, 1.1)
                 : AnimationControlController.buttonColor
            border.color: animSelector.dropdownOpen ? AnimationControlController.highlightColor
                                                    : AnimationControlController.borderColor
            border.width: 1

            Row {
                anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 4; spacing: 4
                Text {
                    text: animSelector.currentLabel
                    color: AnimationControlController.buttonTextColor; font.pixelSize: 11
                    elide: Text.ElideRight; anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 18
                }
                Text {
                    text: animSelector.dropdownOpen ? "\u25B2" : "\u25BC"
                    color: AnimationControlController.buttonTextColor; font.pixelSize: 8
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            MouseArea {
                id: animSelectorMouse; anchors.fill: parent; hoverEnabled: true
                onClicked: {
                    animSelector.dropdownOpen = !animSelector.dropdownOpen
                    if (animSelector.dropdownOpen) { animFilter.text = ""; animFilter.forceActiveFocus() }
                }
            }
        }

        Popup {
            id: animDropdown
            visible: animSelector.dropdownOpen
            x: 0; y: animSelector.height + 2
            width: animSelector.width
            height: Math.min(animListView.contentHeight + 30, 200)
            padding: 0
            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
            onClosed: animSelector.dropdownOpen = false

            background: Rectangle {
                color: AnimationControlController.inputColor
                border.color: AnimationControlController.borderColor; border.width: 1; radius: 3
            }

            Column {
                anchors.fill: parent; spacing: 0

                Rectangle {
                    width: parent.width; height: 26
                    color: AnimationControlController.panelColor
                    border.color: AnimationControlController.borderColor; border.width: 1; radius: 3

                    TextInput {
                        id: animFilter
                        anchors.fill: parent; anchors.margins: 4
                        color: AnimationControlController.textColor; font.pixelSize: 11; clip: true
                        verticalAlignment: TextInput.AlignVCenter

                        property var filtered: {
                            var q = text.toLowerCase()
                            var all = animSelector.flatAnims
                            if (q.length === 0) return all
                            var r = []
                            for (var i = 0; i < all.length; i++)
                                if (all[i].label.toLowerCase().indexOf(q) >= 0) r.push(all[i])
                            return r
                        }

                        Keys.onEscapePressed: animSelector.dropdownOpen = false
                        Keys.onReturnPressed: {
                            if (filtered.length > 0) {
                                AnimationControlController.selectAnimation(filtered[0].entity, filtered[0].anim)
                                animSelector.dropdownOpen = false
                            }
                        }
                    }

                    Text {
                        anchors.fill: parent; anchors.margins: 4
                        text: "Type to filter..."; font.pixelSize: 11; font.italic: true
                        color: AnimationControlController.borderColor
                        visible: animFilter.text.length === 0 && !animFilter.activeFocus
                        verticalAlignment: Text.AlignVCenter
                    }
                }

                ListView {
                    id: animListView
                    width: parent.width; height: parent.height - 26
                    model: animFilter.filtered; clip: true

                    delegate: Rectangle {
                        width: animListView.width; height: 22
                        color: animDelegateMouse.containsMouse ? AnimationControlController.highlightColor : "transparent"

                        Text {
                            anchors.left: parent.left; anchors.leftMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.label
                            color: animDelegateMouse.containsMouse ? "white" : AnimationControlController.textColor
                            font.pixelSize: 11; elide: Text.ElideRight
                        }
                        MouseArea {
                            id: animDelegateMouse; anchors.fill: parent; hoverEnabled: true
                            onClicked: {
                                AnimationControlController.selectAnimation(modelData.entity, modelData.anim)
                                animSelector.dropdownOpen = false
                            }
                        }
                    }
                }
            }
        }
    }

    // ── Bone typeahead ────────────────────────────────────────────────────────
    Text { text: "Bone:"; color: AnimationControlController.textColor; font.pixelSize: 11 }

    Item {
        id: boneSelector
        width: parent.width; height: 24
        property bool dropdownOpen: false

        Rectangle {
            anchors.fill: parent; radius: 3
            color: boneSelectorMouse.pressed ? Qt.darker(AnimationControlController.buttonColor, 1.2)
                 : boneSelectorMouse.containsMouse ? Qt.lighter(AnimationControlController.buttonColor, 1.1)
                 : AnimationControlController.buttonColor
            border.color: boneSelector.dropdownOpen ? AnimationControlController.highlightColor
                                                    : AnimationControlController.borderColor
            border.width: 1

            Row {
                anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 4; spacing: 4
                Text {
                    text: AnimationControlController.selectedBone || "(none)"
                    color: AnimationControlController.buttonTextColor; font.pixelSize: 11
                    elide: Text.ElideRight; anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 18
                }
                Text {
                    text: boneSelector.dropdownOpen ? "\u25B2" : "\u25BC"
                    color: AnimationControlController.buttonTextColor; font.pixelSize: 8
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            MouseArea {
                id: boneSelectorMouse; anchors.fill: parent; hoverEnabled: true
                onClicked: {
                    boneSelector.dropdownOpen = !boneSelector.dropdownOpen
                    if (boneSelector.dropdownOpen) { boneFilter.text = ""; boneFilter.forceActiveFocus() }
                }
            }
        }

        Connections {
            target: AnimationControlController
            function onBoneListChanged() { boneSelector.dropdownOpen = false }
        }

        Popup {
            id: boneDropdown
            visible: boneSelector.dropdownOpen
            x: 0; y: boneSelector.height + 2
            width: boneSelector.width
            height: Math.min(boneListView.contentHeight + 30, 160)
            padding: 0
            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
            onClosed: boneSelector.dropdownOpen = false

            background: Rectangle {
                color: AnimationControlController.inputColor
                border.color: AnimationControlController.borderColor; border.width: 1; radius: 3
            }

            Column {
                anchors.fill: parent; spacing: 0

                Rectangle {
                    width: parent.width; height: 26
                    color: AnimationControlController.panelColor
                    border.color: AnimationControlController.borderColor; border.width: 1; radius: 3

                    TextInput {
                        id: boneFilter
                        anchors.fill: parent; anchors.margins: 4
                        color: AnimationControlController.textColor; font.pixelSize: 11; clip: true
                        verticalAlignment: TextInput.AlignVCenter

                        property var filtered: {
                            var q = text.toLowerCase()
                            var bones = AnimationControlController.boneNames
                            if (q.length === 0) return bones
                            var r = []
                            for (var i = 0; i < bones.length; i++)
                                if (bones[i].toLowerCase().indexOf(q) >= 0) r.push(bones[i])
                            return r
                        }

                        Keys.onEscapePressed: boneSelector.dropdownOpen = false
                        Keys.onReturnPressed: {
                            if (filtered.length > 0) {
                                AnimationControlController.selectBone(filtered[0])
                                boneSelector.dropdownOpen = false
                            }
                        }
                    }

                    Text {
                        anchors.fill: parent; anchors.margins: 4
                        text: "Type to filter..."; font.pixelSize: 11; font.italic: true
                        color: AnimationControlController.borderColor
                        visible: boneFilter.text.length === 0 && !boneFilter.activeFocus
                        verticalAlignment: Text.AlignVCenter
                    }
                }

                ListView {
                    id: boneListView
                    width: parent.width; height: parent.height - 26
                    model: boneFilter.filtered; clip: true

                    delegate: Rectangle {
                        width: boneListView.width; height: 22
                        color: boneDelegateMouse.containsMouse ? AnimationControlController.highlightColor : "transparent"

                        Text {
                            anchors.left: parent.left; anchors.leftMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData
                            color: boneDelegateMouse.containsMouse ? "white" : AnimationControlController.textColor
                            font.pixelSize: 11; elide: Text.ElideRight
                        }
                        MouseArea {
                            id: boneDelegateMouse; anchors.fill: parent; hoverEnabled: true
                            onClicked: { AnimationControlController.selectBone(modelData); boneSelector.dropdownOpen = false }
                        }
                    }
                }
            }
        }
    }

    // ── Length + keyframe nav ─────────────────────────────────────────────────
    RowLayout {
        width: parent.width; spacing: 4

        Text { text: "Length:"; color: AnimationControlController.textColor; font.pixelSize: 11; Layout.alignment: Qt.AlignVCenter }

        Rectangle {
            Layout.preferredWidth: 58; height: 22; radius: 2
            color: AnimationControlController.hasAnimation ? AnimationControlController.inputColor
                                                           : Qt.darker(AnimationControlController.panelColor, 1.12)
            border.color: AnimationControlController.borderColor; border.width: 1; Layout.alignment: Qt.AlignVCenter

            TextInput {
                id: lengthInput
                anchors.fill: parent; anchors.margins: 4
                text: AnimationControlController.animationLength.toFixed(3)
                color: AnimationControlController.hasAnimation ? AnimationControlController.textColor
                                                               : AnimationControlController.disabledTextColor
                font.pixelSize: 11; readOnly: !AnimationControlController.hasAnimation
                selectByMouse: true; verticalAlignment: Text.AlignVCenter
                validator: DoubleValidator { bottom: 0.001; decimals: 3 }
                onEditingFinished: { var v = parseFloat(text); if (!isNaN(v) && v > 0) AnimationControlController.animationLength = v }
                Connections {
                    target: AnimationControlController
                    function onAnimationLengthChanged() { if (!lengthInput.activeFocus) lengthInput.text = AnimationControlController.animationLength.toFixed(3) }
                }
            }
        }

        Text { text: "s"; color: AnimationControlController.textColor; font.pixelSize: 11; Layout.alignment: Qt.AlignVCenter }
        Item { Layout.fillWidth: true }
        ToolBtn { label: "|<"; enabled: AnimationControlController.hasPrevKeyframe; onClicked: AnimationControlController.prevKeyframe() }
        ToolBtn { label: ">|"; enabled: AnimationControlController.hasNextKeyframe; onClicked: AnimationControlController.nextKeyframe() }
        ToolBtn { label: "+KF"; enabled: AnimationControlController.hasAnimation;   onClicked: AnimationControlController.addKeyframe() }
        ToolBtn { label: "-KF"; enabled: AnimationControlController.canDeleteKeyframe; onClicked: AnimationControlController.deleteKeyframe() }
    }

    // ── Playback toolbar: loop toggle (speed lives next to Play button) ───
    RowLayout {
        width: parent.width; spacing: 6

        Rectangle {
            Layout.preferredWidth: 80; height: 22; radius: 3
            color: AnimationControlController.loopRegionActive
                ? AnimationControlController.highlightColor
                : (loopMa.containsMouse ? Qt.lighter(AnimationControlController.buttonColor, 1.15)
                                        : AnimationControlController.buttonColor)
            border.color: AnimationControlController.borderColor; border.width: 1
            Text {
                anchors.centerIn: parent
                text: AnimationControlController.loopRegionActive ? "Loop ON" : "Loop OFF"
                color: AnimationControlController.loopRegionActive ? "white" : AnimationControlController.buttonTextColor
                font.pixelSize: 11
            }
            MouseArea {
                id: loopMa; anchors.fill: parent; hoverEnabled: true
                onClicked: AnimationControlController.loopRegionActive = !AnimationControlController.loopRegionActive
            }
        }

        Item { Layout.fillWidth: true }
    }

    // ── Timeline ──────────────────────────────────────────────────────────────
    RowLayout {
        width: parent.width; height: 28; spacing: 4

        Text { text: "0"; color: AnimationControlController.textColor; font.pixelSize: 10; Layout.alignment: Qt.AlignVCenter }

        Item {
            Layout.fillWidth: true; height: 28

            Slider {
                id: timeSlider
                anchors.fill: parent; from: 0; to: AnimationControlController.sliderMaximum; stepSize: 1
                value: AnimationControlController.sliderValue
                onMoved: AnimationControlController.sliderValue = value

                background: Rectangle {
                    x: timeSlider.leftPadding; y: timeSlider.topPadding + timeSlider.availableHeight / 2 - height / 2
                    width: timeSlider.availableWidth; height: 4; radius: 2
                    color: AnimationControlController.inputColor
                    Rectangle { width: timeSlider.visualPosition * parent.width; height: parent.height; radius: 2; color: AnimationControlController.highlightColor }
                }
                handle: Rectangle {
                    x: timeSlider.leftPadding + timeSlider.visualPosition * (timeSlider.availableWidth - width)
                    y: timeSlider.topPadding + timeSlider.availableHeight / 2 - height / 2
                    width: 12; height: 12; radius: 6
                    color: AnimationControlController.highlightColor
                    border.color: Qt.lighter(AnimationControlController.highlightColor, 1.4); border.width: 1.5
                }
            }

            Canvas {
                id: tickCanvas; anchors.fill: parent; enabled: false
                onPaint: {
                    var ctx = getContext("2d"); ctx.clearRect(0, 0, width, height)
                    var maxMs = AnimationControlController.sliderMaximum; if (maxMs <= 0) return
                    // Use the slider's actual layout so loop shading + tick marks
                    // track the slider groove across Qt styles, DPI, and platforms.
                    var pad = timeSlider.leftPadding
                    var avail = timeSlider.availableWidth

                    // Loop region shading (drawn under keyframe ticks)
                    if (AnimationControlController.loopRegionActive) {
                        var ls = AnimationControlController.loopStart * 1000
                        var le = AnimationControlController.loopEnd   * 1000
                        var lx = pad + (ls / maxMs) * avail
                        var rx = pad + (le / maxMs) * avail
                        ctx.fillStyle = "rgba(64, 192, 255, 0.18)"
                        ctx.fillRect(lx, 0, Math.max(0, rx - lx), height)
                        ctx.strokeStyle = "#40c0ff"; ctx.lineWidth = 2
                        ctx.beginPath(); ctx.moveTo(lx, 0); ctx.lineTo(lx, height); ctx.stroke()
                        ctx.beginPath(); ctx.moveTo(rx, 0); ctx.lineTo(rx, height); ctx.stroke()
                        ctx.fillStyle = "#40c0ff"
                        ctx.beginPath(); ctx.moveTo(lx, 0); ctx.lineTo(lx + 6, 0); ctx.lineTo(lx, 6); ctx.closePath(); ctx.fill()
                        ctx.beginPath(); ctx.moveTo(rx, 0); ctx.lineTo(rx - 6, 0); ctx.lineTo(rx, 6); ctx.closePath(); ctx.fill()
                    }

                    var ticks = AnimationControlController.keyframeTicks; var selTk = AnimationControlController.selectedTick
                    for (var i = 0; i < ticks.length; i++) {
                        var x = pad + (ticks[i] / maxMs) * avail; var isSel = (ticks[i] === selTk)
                        if (isSel) {
                            ctx.strokeStyle = "#ff4444"; ctx.lineWidth = 3
                            ctx.beginPath(); ctx.moveTo(x, 4); ctx.lineTo(x, height); ctx.stroke()
                            ctx.fillStyle = "#ff4444"
                            ctx.beginPath(); ctx.moveTo(x - 5, 2); ctx.lineTo(x + 5, 2); ctx.lineTo(x, 8); ctx.closePath(); ctx.fill()
                        } else {
                            ctx.strokeStyle = "#ffcc00"; ctx.lineWidth = 1.5
                            ctx.beginPath(); ctx.moveTo(x, 2); ctx.lineTo(x, height - 2); ctx.stroke()
                        }
                    }
                }
                Connections {
                    target: AnimationControlController
                    function onKeyframeTicksChanged()  { tickCanvas.requestPaint() }
                    function onAnimationLengthChanged() { tickCanvas.requestPaint() }
                    function onThemeChanged()           { tickCanvas.requestPaint() }
                    function onSliderValueChanged()     { tickCanvas.requestPaint() }
                    function onLoopRegionChanged()      { tickCanvas.requestPaint() }
                }
            }

            // Drag handles for loop in/out points — only visible when loop is active.
            // Sits on top of the slider so drags on the handle areas don't move the playhead.
            // Handle x stays purely bound to the controller value; we compute the
            // new time from mouseX deltas instead of dragging the visual item
            // (drag.target on the rectangle would break the binding after release).
            Item {
                id: loopHandlesLayer
                anchors.fill: parent
                visible: AnimationControlController.loopRegionActive

                // Bind to the slider's actual layout so handles stay aligned
                // with the groove regardless of style/DPI.
                property real pad: timeSlider.leftPadding
                property real avail: timeSlider.availableWidth
                property real maxMs: Math.max(1, AnimationControlController.sliderMaximum)

                function pxToSec(px) {
                    var t = (px / avail) * (maxMs / 1000.0)
                    if (t < 0) t = 0
                    if (t > maxMs / 1000.0) t = maxMs / 1000.0
                    return t
                }

                Rectangle {
                    id: loopStartHandle
                    width: 10; height: parent.height
                    x: loopHandlesLayer.pad
                       + (AnimationControlController.loopStart * 1000 / loopHandlesLayer.maxMs) * loopHandlesLayer.avail
                       - width / 2
                    color: "transparent"
                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.SizeHorCursor
                        preventStealing: true
                        property bool dragging: false
                        onPressed: dragging = true
                        onReleased: dragging = false
                        onPositionChanged: function(mouse) {
                            if (!dragging) return
                            // Convert local mouseX to layer-space, then to seconds.
                            var layerX = loopStartHandle.x + mouse.x - loopHandlesLayer.pad
                            var t = loopHandlesLayer.pxToSec(layerX)
                            if (t > AnimationControlController.loopEnd) t = AnimationControlController.loopEnd
                            AnimationControlController.loopStart = t
                        }
                    }
                }

                Rectangle {
                    id: loopEndHandle
                    width: 10; height: parent.height
                    x: loopHandlesLayer.pad
                       + (AnimationControlController.loopEnd * 1000 / loopHandlesLayer.maxMs) * loopHandlesLayer.avail
                       - width / 2
                    color: "transparent"
                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.SizeHorCursor
                        preventStealing: true
                        property bool dragging: false
                        onPressed: dragging = true
                        onReleased: dragging = false
                        onPositionChanged: function(mouse) {
                            if (!dragging) return
                            var layerX = loopEndHandle.x + mouse.x - loopHandlesLayer.pad
                            var t = loopHandlesLayer.pxToSec(layerX)
                            if (t < AnimationControlController.loopStart) t = AnimationControlController.loopStart
                            AnimationControlController.loopEnd = t
                        }
                    }
                }
            }
        }

        Text { text: AnimationControlController.animationLength.toFixed(2) + "s"; color: AnimationControlController.textColor; font.pixelSize: 10; Layout.alignment: Qt.AlignVCenter }
    }

    // ── Keyframe T / S / R value sections ────────────────────────────────────
    Column {
        visible: AnimationControlController.hasAnimation
        width: parent.width; spacing: 6

        // ── Translate ────────────────────────────────────────────────────
        Text { text: "Translate"; color: AnimationControlController.textColor; font.pixelSize: 11; font.bold: true }
        Row {
            spacing: 4; width: parent.width
            KFTransformField { label: "X"; val: AnimationControlController.kfTransX; labelColor: "#c04040"; editable: AnimationControlController.onKeyframe; width: (parent.width - 8) / 3
                onCommitted: function(v) { AnimationControlController.setKfTransX(v) } }
            KFTransformField { label: "Y"; val: AnimationControlController.kfTransY; labelColor: "#40c040"; editable: AnimationControlController.onKeyframe; width: (parent.width - 8) / 3
                onCommitted: function(v) { AnimationControlController.setKfTransY(v) } }
            KFTransformField { label: "Z"; val: AnimationControlController.kfTransZ; labelColor: "#4040c0"; editable: AnimationControlController.onKeyframe; width: (parent.width - 8) / 3
                onCommitted: function(v) { AnimationControlController.setKfTransZ(v) } }
        }

        // ── Scale ─────────────────────────────────────────────────────────
        Text { text: "Scale"; color: AnimationControlController.textColor; font.pixelSize: 11; font.bold: true }
        Row {
            spacing: 4; width: parent.width
            KFTransformField { label: "X"; val: AnimationControlController.kfScaleX; labelColor: "#c04040"; editable: AnimationControlController.onKeyframe; width: (parent.width - 8) / 3
                onCommitted: function(v) { AnimationControlController.setKfScaleX(v) } }
            KFTransformField { label: "Y"; val: AnimationControlController.kfScaleY; labelColor: "#40c040"; editable: AnimationControlController.onKeyframe; width: (parent.width - 8) / 3
                onCommitted: function(v) { AnimationControlController.setKfScaleY(v) } }
            KFTransformField { label: "Z"; val: AnimationControlController.kfScaleZ; labelColor: "#4040c0"; editable: AnimationControlController.onKeyframe; width: (parent.width - 8) / 3
                onCommitted: function(v) { AnimationControlController.setKfScaleZ(v) } }
        }

        // ── Rotate (quaternion W X Y Z) ───────────────────────────────────
        Text { text: "Orientation"; color: AnimationControlController.textColor; font.pixelSize: 11; font.bold: true }
        Row {
            spacing: 4; width: parent.width
            KFTransformField { label: "W"; val: AnimationControlController.kfRotW; labelColor: "#a040a0"; editable: AnimationControlController.onKeyframe; width: (parent.width - 8) / 3
                onCommitted: function(v) { AnimationControlController.setKfRotW(v) } }
            KFTransformField { label: "X"; val: AnimationControlController.kfRotX; labelColor: "#c04040"; editable: AnimationControlController.onKeyframe; width: (parent.width - 8) / 3
                onCommitted: function(v) { AnimationControlController.setKfRotX(v) } }
            KFTransformField { label: "Y"; val: AnimationControlController.kfRotY; labelColor: "#40c040"; editable: AnimationControlController.onKeyframe; width: (parent.width - 8) / 3
                onCommitted: function(v) { AnimationControlController.setKfRotY(v) } }
        }
        Row {
            spacing: 4; width: parent.width
            KFTransformField { label: "Z"; val: AnimationControlController.kfRotZ; labelColor: "#4040c0"; editable: AnimationControlController.onKeyframe; width: (parent.width - 8) / 3
                onCommitted: function(v) { AnimationControlController.setKfRotZ(v) } }
        }
    }
}
