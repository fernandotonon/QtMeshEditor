import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AIChatPanel 1.0
import PropertiesPanel 1.0

Rectangle {
    id: root
    color: PropertiesPanelController.panelColor

    // Forward focus to the input field whenever the panel gains active focus
    // (e.g. when the user clicks anywhere in the dock after returning from
    // another app window).
    onActiveFocusChanged: {
        if (activeFocus)
            Qt.callLater(() => inputField.forceActiveFocus())
    }

    // ---- Header bar ----
    Rectangle {
        id: header
        anchors { top: parent.top; left: parent.left; right: parent.right }
        height: 36
        color: PropertiesPanelController.headerColor

        RowLayout {
            anchors { fill: parent; leftMargin: 8; rightMargin: 8 }
            spacing: 6

            Text {
                text: "AI Chat"
                color: PropertiesPanelController.textColor
                font.pixelSize: 13; font.bold: true
                Layout.fillWidth: true
            }

            // Model status dot
            Rectangle {
                width: 8; height: 8; radius: 4
                color: AIChatManager.modelAvailable ? "#44dd44" : "#dd4444"
            }

            Text {
                text: AIChatManager.modelAvailable
                      ? AIChatManager.currentModelName
                      : "No model"
                color: PropertiesPanelController.textColor
                font.pixelSize: 10
                elide: Text.ElideMiddle
                Layout.maximumWidth: 160
            }

            // Clear button
            Rectangle {
                width: 22; height: 22; radius: 3
                color: clearArea.containsMouse
                       ? Qt.lighter(PropertiesPanelController.panelColor, 1.5)
                       : "transparent"

                Text { anchors.centerIn: parent; text: "✕"; color: PropertiesPanelController.textColor; font.pixelSize: 11 }
                MouseArea {
                    id: clearArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: AIChatManager.clearHistory()
                }
            }
        }
    }

    // ---- Single selectable conversation area ----
    Flickable {
        id: msgFlick
        anchors {
            top: header.bottom; left: parent.left; right: parent.right
            bottom: thinkingRow.top; bottomMargin: 0
        }
        clip: true
        contentWidth: width
        contentHeight: msgEdit.implicitHeight + 16

        function scrollToBottom() {
            contentY = Math.max(0, contentHeight - height)
        }

        TextEdit {
            id: msgEdit
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 8 }
            readOnly: true
            selectByMouse: true
            textFormat: Text.RichText
            wrapMode: Text.WrapAtWordBoundaryOrAnywhere
            color: PropertiesPanelController.textColor
            font.pixelSize: 12
            selectionColor: Qt.rgba(0.3, 0.5, 0.8, 0.5)
            selectedTextColor: PropertiesPanelController.textColor
            text: root.buildHtml()

            // Intercept Ctrl/Cmd+C to copy plain text instead of RichText HTML.
            Keys.onPressed: (event) => {
                if ((event.key === Qt.Key_C) &&
                    (event.modifiers & Qt.ControlModifier)) {
                    if (selectedText.length > 0) {
                        Qt.application.clipboard.text = selectedText
                        event.accepted = true
                    }
                }
            }

            // When the user finishes selecting (mouse released with no selection),
            // return focus to the input field. Use onActiveFocusChanged instead of
            // onSelectedTextChanged to avoid stealing focus mid-drag.
            onActiveFocusChanged: {
                if (!activeFocus && selectedText.length === 0)
                    Qt.callLater(() => inputField.forceActiveFocus())
            }
        }

        onContentHeightChanged: Qt.callLater(scrollToBottom)

        Connections {
            target: AIChatManager
            function onMessagesChanged()     { Qt.callLater(msgFlick.scrollToBottom) }
            function onStreamingTextChanged(){ Qt.callLater(msgFlick.scrollToBottom) }
        }
    }

    // ---- Thinking dots (no tokens yet) ----
    Row {
        id: thinkingRow
        anchors { bottom: inputRow.top; left: parent.left; leftMargin: 12; bottomMargin: 6 }
        height: visible ? 14 : 0
        spacing: 4
        visible: AIChatManager.isGenerating

        Repeater {
            model: 3
            Rectangle {
                width: 6; height: 6; radius: 3
                color: PropertiesPanelController.accentColor
                opacity: 0.3
                SequentialAnimation on opacity {
                    loops: Animation.Infinite
                    NumberAnimation { to: 1.0; duration: 400 }
                    NumberAnimation { to: 0.3; duration: 400 }
                    PauseAnimation  { duration: index * 150 }
                }
            }
        }
    }

    // ---- Input row ----
    Rectangle {
        id: inputRow
        anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
        height: Math.max(40, Math.min(inputField.implicitHeight, 80) + 16)
        color: PropertiesPanelController.headerColor
        border.color: PropertiesPanelController.borderColor
        border.width: 1

        TextArea {
            id: inputField
            anchors { left: parent.left; right: sendBtn.left;
                      verticalCenter: parent.verticalCenter;
                      leftMargin: 8; rightMargin: 6 }
            height: Math.min(implicitHeight, 80)
            placeholderText: AIChatManager.modelAvailable
                             ? "Ask AI to do something…"
                             : "Load an AI model first (AI → AI Model Settings)"
            color: PropertiesPanelController.textColor
            background: null
            font.pixelSize: 12
            wrapMode: Text.WrapAtWordBoundaryOrAnywhere
            enabled: AIChatManager.modelAvailable && !AIChatManager.isGenerating
            focus: true

            Keys.onReturnPressed: (event) => {
                if (event.modifiers & Qt.ShiftModifier) {
                    event.accepted = false   // shift+enter → newline
                } else {
                    event.accepted = true
                    doSend()
                }
            }
        }

        Rectangle {
            id: sendBtn
            anchors { right: parent.right; verticalCenter: parent.verticalCenter; rightMargin: 8 }
            width: 32; height: 32; radius: 4
            color: sendBtnArea.containsMouse
                   ? PropertiesPanelController.accentColor
                   : PropertiesPanelController.buttonColor
            enabled: AIChatManager.modelAvailable

            Text {
                anchors.centerIn: parent
                text: AIChatManager.isGenerating ? "■" : "▶"
                color: PropertiesPanelController.textColor
                font.pixelSize: 13
            }

            MouseArea {
                id: sendBtnArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (AIChatManager.isGenerating)
                        AIChatManager.stopGeneration()
                    else
                        doSend()
                }
            }
        }
    }

    // ---- Empty state hint ----
    Column {
        anchors.centerIn: parent
        anchors.verticalCenterOffset: -30
        spacing: 8
        visible: AIChatManager.messages.length === 0 && !AIChatManager.isGenerating

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "✦"
            color: PropertiesPanelController.accentColor
            font.pixelSize: 28
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "Ask me to control the editor"
            color: PropertiesPanelController.textColor
            font.pixelSize: 13; opacity: 0.7
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "\"make the selected mesh twice as large\""
            color: PropertiesPanelController.textColor
            font.pixelSize: 11; opacity: 0.45; font.italic: true
        }
    }

    // ---- Helpers ----

    function doSend() {
        var txt = inputField.text.trim()
        if (txt.length > 0) {
            AIChatManager.sendMessage(txt)
            inputField.text = ""
        }
    }

    function escHtml(t) {
        return t.replace(/&/g, "&amp;")
                .replace(/</g, "&lt;")
                .replace(/>/g, "&gt;")
                .replace(/\n/g, "<br>")
    }

    function buildHtml() {
        var html = ""
        var msgs = AIChatManager.messages
        for (var i = 0; i < msgs.length; ++i) {
            var msg = msgs[i]
            var isTool = msg.isTool
            var role   = msg.role
            var roleColor, roleLabel
            if (isTool)             { roleColor = "#88cc88"; roleLabel = "⚙ tool" }
            else if (role === "user"){ roleColor = "#88aacc"; roleLabel = "you" }
            else                    { roleColor = "#aaaaaa"; roleLabel = "assistant" }

            // Assistant messages may be structured JSON — render appropriately.
            var displayText = msg.text
            if (role === "assistant" && !isTool) {
                var trimmed = msg.text.trim()
                if (trimmed.startsWith("{")) {
                    try {
                        var obj = JSON.parse(trimmed)
                        if (obj && obj.command)
                            displayText = "[calling " + obj.command + "]"
                        else if (obj && obj.response)
                            displayText = obj.response          // final done message
                        else if (obj && obj.name)
                            displayText = "[calling " + obj.name + "]"  // legacy fallback
                    } catch(e) {}
                }
            }

            if (i > 0) html += "<br>"
            if (role === "user") {
                html += '<p align="right"><font color="#88aacc"><small><b>you</b></small></font><br>'
                html += escHtml(displayText) + '</p>'
            } else {
                html += '<font color="' + roleColor + '"><small><b>' + roleLabel + '</b></small></font><br>'
                html += escHtml(displayText) + "<br>"
            }
        }

        // In-progress streaming text
        if (AIChatManager.streamingText.length > 0) {
            if (msgs.length > 0) html += "<br>"
            html += '<font color="#aaaaaa"><small><b>assistant</b></small></font><br>'
            html += escHtml(AIChatManager.streamingText)
        }

        return html
    }
}
