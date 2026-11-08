pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Window
import PropertiesPanel 1.0

// ── Image source ──────────────────────────────────────────────────────────
// ONE place to get the image every "from an image" action works on: pick a file, or create / edit one from a text prompt
// (FLUX.2-klein or any Stable Diffusion checkpoint), with a preview, a full-size viewer + Save As, and the automatic caption.
// The selection is the ImageSourceController singleton's, so the Image → 3D and the Character Generator sections share it:
// choose it once, use it from either.
Column {
    id: picker
    width: parent ? parent.width : 300
    spacing: 6

    component PickerButton: Rectangle {
        id: pbRoot
        property alias text: pbLabel.text
        property bool clickEnabled: true
        signal clicked()
        width: Math.min(parent ? parent.width : 200, pbLabel.implicitWidth + 20)
        height: 26
        radius: 3
        opacity: clickEnabled ? 1.0 : 0.45
        color: (pbMa.containsMouse || pbRoot.activeFocus) && clickEnabled
            ? PropertiesPanelController.highlightColor
            : PropertiesPanelController.headerColor
        border.color: pbRoot.activeFocus ? PropertiesPanelController.highlightColor : PropertiesPanelController.borderColor
        border.width: pbRoot.activeFocus ? 2 : 1
        activeFocusOnTab: clickEnabled
        Accessible.role: Accessible.Button
        Accessible.name: pbLabel.text
        Keys.onSpacePressed: if (clickEnabled) clicked()
        Keys.onReturnPressed: if (clickEnabled) clicked()
        Keys.onEnterPressed: if (clickEnabled) clicked()
        Text { id: pbLabel; anchors.centerIn: parent; color: PropertiesPanelController.textColor; font.pixelSize: 11 }
        MouseArea {
            id: pbMa
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: pbRoot.clickEnabled ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: if (pbRoot.clickEnabled) pbRoot.clicked()
        }
    }

    // what the picker itself reports: selected / described / saved / errors
    TextEdit {
        id: sourceStatus
        width: parent.width
        visible: text.length > 0
        property bool isError: false
        color: isError ? "#e08080" : PropertiesPanelController.textColor
        opacity: isError ? 1.0 : 0.7
        font.pixelSize: 10
        wrapMode: TextEdit.WordWrap
        readOnly: true
        selectByMouse: true
        activeFocusOnPress: true
        cursorVisible: false
        persistentSelection: true
    }
    Connections {
        target: ImageSourceController
        function onStatusMessage(message) { sourceStatus.isError = false; sourceStatus.text = message }
        function onError(message) { sourceStatus.isError = true; sourceStatus.text = message }
    }

    // Step 1: select the source image (no generation yet).
    PickerButton {
        text: "Select Image…"
        clickEnabled: !ImageSourceController.locked
        onClicked: ImageSourceController.selectImage()
    }

    // — or generate the source image from a TEXT PROMPT (FLUX.2-klein
    // via stable-diffusion.cpp). Shown when the build has SD support
    // and a usable image model exists (or can be pointed at).
    Text {
        text: ImageSourceController.selectedImagePath.length > 0
            ? "— or EDIT the loaded image with a prompt (FLUX.2):"
            : "— or generate the image from text:"
        color: PropertiesPanelController.textColor
        opacity: 0.7; font.pixelSize: 10
    }
    Row {
        width: parent.width
        spacing: 6
        Rectangle {
            width: parent.width - imgGenBtn.width - 6; height: 24; radius: 3
            color: PropertiesPanelController.inputColor
            border.color: imgGenPromptIn.activeFocus
                ? PropertiesPanelController.highlightColor
                : PropertiesPanelController.borderColor
            TextInput {
                id: imgGenPromptIn
                anchors.fill: parent
                anchors.leftMargin: 6; anchors.rightMargin: 6
                verticalAlignment: TextInput.AlignVCenter
                color: PropertiesPanelController.textColor; font.pixelSize: 11
                clip: true; selectByMouse: true; activeFocusOnPress: true
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.IBeamCursor
                    onPressed: function(mouse) {
                        // WIDGET-level focus first: after the app window
                        // deactivates, the QQuickWidget still thinks this
                        // item has activeFocus, so forceActiveFocus alone
                        // is a no-op while keys go to the viewport.
                        PropertiesPanelController.focusPanel()
                        imgGenPromptIn.forceActiveFocus()
                        mouse.accepted = false
                    }
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: !imgGenPromptIn.text && !imgGenPromptIn.activeFocus
                    text: ImageSourceController.selectedImagePath.length > 0
                        ? "e.g. give him golden armor"
                        : "describe it — \"a capybara standing on grass\", not \"capybara\""
                    color: PropertiesPanelController.textColor
                    opacity: 0.4; font.pixelSize: 11
                }
                onAccepted: imgGenBtn.run()
            }
        }
        Rectangle {
            id: imgGenBtn
            width: 88; height: 24; radius: 3
            property bool genBusy: false
            opacity: genBusy ? 0.5 : 1.0
            color: imgGenMa.pressed
                ? Qt.darker(PropertiesPanelController.highlightColor, 1.2)
                : imgGenMa.containsMouse
                    ? Qt.lighter(PropertiesPanelController.highlightColor, 1.1)
                    : PropertiesPanelController.highlightColor
            function run() {
                if (genBusy || !imgGenPromptIn.text.trim()) return
                if (!ImageSourceController.imageGenAvailable()) {
                    imgGenStatusTxt.isError = true
                    imgGenStatusTxt.text =
                        "Download FLUX.2-klein-4B in AI Model Settings first."
                    return
                }
                genBusy = true
                ImageSourceController.generateSourceImage(imgGenPromptIn.text)
            }
            // With an image already selected the prompt EDITS it
            // (FLUX.2 kontext-style) instead of generating from
            // scratch — a user who typed "capybara" after a previous
            // run got their old subject nudged, not a capybara. Say
            // which one the click will do.
            Text { anchors.centerIn: parent
                   text: imgGenBtn.genBusy
                         ? "…"
                         : (ImageSourceController.selectedImagePath.length > 0
                            ? "Edit Image" : "Create Image")
                   color: "white"; font.pixelSize: 10 }
            MouseArea { id: imgGenMa; anchors.fill: parent; hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: imgGenBtn.run() }
        }
    }
    // Edit mode is easy to miss (the placeholder only shows on an EMPTY
    // field) and its failure is confusing: typing a new subject nudges
    // the old image instead of generating that subject. Sibling of the
    // Row, not a child — a Row lays out horizontally, so a full-width
    // Text inside it would overflow and displace the input and button.
    Text {
        visible: ImageSourceController.selectedImagePath.length > 0
        text: "✎ Editing the loaded image — 🗑 it to create a new one"
        color: PropertiesPanelController.textColor
        opacity: 0.65; font.pixelSize: 10
        wrapMode: Text.Wrap
        width: parent.width
    }
    TextEdit {
        id: imgGenStatusTxt
        width: parent.width
        visible: text.length > 0
        property bool isError: false
        color: isError ? "#e08080" : PropertiesPanelController.textColor
        font.pixelSize: 10
        wrapMode: TextEdit.WordWrap
        text: ""
        // Selectable so a failure message can be COPIED — see mgStatus.
        readOnly: true
        selectByMouse: true
        activeFocusOnPress: true
        cursorVisible: false
        persistentSelection: true
    }
    // Progress while the image generates: determinate once sampling
    // ticks arrive, indeterminate sweep during the (long) model load.
    Rectangle {
        id: imgGenProgress
        width: parent.width; height: 6; radius: 3
        visible: imgGenBtn.genBusy
        color: PropertiesPanelController.inputColor
        border.color: PropertiesPanelController.borderColor
        property int step: 0
        property int total: 0
        onVisibleChanged: if (visible) { step = 0; total = 0 }
        // Determinate fill
        Rectangle {
            visible: imgGenProgress.total > 0
            x: 1; y: 1; height: parent.height - 2; radius: 2
            width: imgGenProgress.total > 0
                ? Math.max(4, (parent.width - 2)
                               * imgGenProgress.step / imgGenProgress.total)
                : 0
            color: PropertiesPanelController.highlightColor
        }
        // Indeterminate sweep (model loading — no ticks yet)
        Rectangle {
            id: imgGenSweep
            visible: imgGenProgress.total === 0 && imgGenProgress.visible
            y: 1; height: parent.height - 2; width: parent.width / 4
            radius: 2
            color: PropertiesPanelController.highlightColor
            opacity: 0.7
            SequentialAnimation on x {
                running: imgGenSweep.visible
                loops: Animation.Infinite
                NumberAnimation { from: 1; to: imgGenProgress.width * 3 / 4 - 1
                                  duration: 900; easing.type: Easing.InOutQuad }
                NumberAnimation { from: imgGenProgress.width * 3 / 4 - 1; to: 1
                                  duration: 900; easing.type: Easing.InOutQuad }
            }
        }
    }
    Connections {
        target: ImageSourceController
        function onImageGenStatus(message, isError) {
            imgGenStatusTxt.isError = isError
            imgGenStatusTxt.text = message
            // Busy only while a load/generate is still pending: any
            // error, and the final "Image ready" line, end the run.
            imgGenBtn.genBusy = !isError
                && message.indexOf("ready") < 0
        }
        function onImageGenProgress(step, total) {
            imgGenProgress.step = step
            imgGenProgress.total = total
        }
    }

    // Preview of the selected image (shown once one is chosen).
    // Click to open the full-size image in a viewer window.
    Rectangle {
        width: parent.width
        height: visible ? 140 : 0
        visible: ImageSourceController.selectedImagePath.length > 0
        color: PropertiesPanelController.inputColor
        border.color: PropertiesPanelController.borderColor
        border.width: 1
        radius: 3
        Image {
            anchors.fill: parent
            anchors.margins: 4
            source: ImageSourceController.previewSource
            fillMode: Image.PreserveAspectFit
            smooth: true
            cache: false
        }
        Text {
            anchors.right: parent.right; anchors.bottom: parent.bottom
            anchors.margins: 4
            text: "🔍"
            font.pixelSize: 12
            opacity: prevMa.containsMouse ? 1.0 : 0.5
        }
        MouseArea {
            id: prevMa
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: imageViewerWindow.open()
        }
        // Remove the image (back to generate-from-scratch).
        Rectangle {
            anchors.right: parent.right; anchors.top: parent.top
            anchors.margins: 4
            width: 22; height: 22; radius: 3
            color: trashMa.containsMouse
                ? Qt.darker(PropertiesPanelController.headerColor, 1.1)
                : PropertiesPanelController.headerColor
            border.color: PropertiesPanelController.borderColor
            opacity: trashMa.containsMouse ? 1.0 : 0.7
            Text { anchors.centerIn: parent; text: "🗑"; font.pixelSize: 12 }
            MouseArea {
                id: trashMa
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: ImageSourceController.clearSelectedImage()
            }
        }
    }
    // Full-size viewer for the selected/generated source image.
    Window {
        id: imageViewerWindow
        title: "Source Image"
        flags: Qt.Dialog
        color: PropertiesPanelController.panelColor
        width: Math.min(1024, Screen.desktopAvailableWidth - 120)
        height: Math.min(1024, Screen.desktopAvailableHeight - 120)
        function open() {
            fullImage.source = ""   // force reload (same path, new pixels)
            fullImage.source = ImageSourceController.selectedImageUrl
            show(); raise(); requestActivate()
        }
        Image {
            id: fullImage
            anchors.fill: parent
            anchors.margins: 8
            fillMode: Image.PreserveAspectFit
            smooth: true
            cache: false
        }
        // Declared before the controls so they are on top of it, and
        // bounded above the button row so it cannot cover them.
        MouseArea {
            anchors { left: parent.left; right: parent.right; top: parent.top
                      bottom: saveRow.top }
            onDoubleClicked: imageViewerWindow.close()
        }
        // A generated image otherwise lives only in AppData under a
        // timestamped name, so it is effectively thrown away after the
        // mesh is built. Saving makes prompt-to-image useful on its own.
        Row {
            id: saveRow
            // Bottom-RIGHT: the path label below is centred and full
            // width, so a centred button sat directly above it and
            // read as part of that label.
            anchors.bottom: sourcePathLabel.top
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.bottomMargin: 6
            spacing: 8
            Button {
                text: "Save Image As…"
                onClicked: ImageSourceController.saveSelectedImageInteractive()
            }
        }
        Text {
            id: sourcePathLabel
            anchors.bottom: parent.bottom
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.margins: 4
            text: ImageSourceController.selectedImagePath
            color: PropertiesPanelController.textColor
            opacity: 0.6; font.pixelSize: 10
            elide: Text.ElideMiddle
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
        }
        // Esc closes. The close-on-double-click MouseArea must not
        // eat the button's clicks: `fullImage` fills the WHOLE window
        // (anchors.fill: parent), so anchoring to it still covered the
        // button row — and being declared last it sat on top, so Save
        // never received a press. Stop it above the controls, and
        // declare it BEFORE them so they win the overlap regardless.
        Shortcut { sequence: "Esc"; onActivated: imageViewerWindow.close() }
    }

    // Auto-generated caption of the selected image (SmolVLM), computed
    // in the background the moment the image is picked. Shown here so
    // the user sees what the model "read" from the image — it becomes
    // the texture prompt. "Describing…" while it's still running.
    Text {
        width: parent.width
        visible: ImageSourceController.selectedImagePath.length > 0
            && (ImageSourceController.captioning
                || ImageSourceController.caption.length > 0)
        text: ImageSourceController.captioning
            ? "🔍 Describing image…"
            : "📝 " + ImageSourceController.caption
        color: PropertiesPanelController.textColor
        font.pixelSize: 11
        font.italic: ImageSourceController.captioning
        wrapMode: Text.WordWrap
    }
}
