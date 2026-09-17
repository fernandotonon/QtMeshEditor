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

    readonly property bool agentBusy: AIAgentManager.busy
    readonly property bool awaitingConfirm: AIAgentManager.state === "awaiting_confirmation"

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
                text: AIChatManager.agentMode ? "AI Agent" : "AI Chat"
                color: PropertiesPanelController.textColor
                font.pixelSize: 13; font.bold: true
                Layout.fillWidth: true
            }

            // Agent mode toggle (#1021): plan → execute → observe, one undo group.
            Rectangle {
                width: agentToggleText.implicitWidth + 12; height: 20; radius: 3
                color: AIChatManager.agentMode
                       ? PropertiesPanelController.highlightColor
                       : PropertiesPanelController.buttonColor
                border.color: PropertiesPanelController.borderColor
                ToolTip.visible: agentToggleArea.containsMouse
                ToolTip.delay: 500
                ToolTip.text: AIChatManager.agentMode
                    ? "Agent mode: multi-step plans, structured observations, one undo group per task, confirmations for destructive steps. Click for the simple chat loop."
                    : "Simple chat loop (one tool per reply). Click for agent mode."
                Text {
                    id: agentToggleText
                    anchors.centerIn: parent
                    text: "agent"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 10
                }
                MouseArea {
                    id: agentToggleArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    enabled: !AIChatManager.isGenerating
                    onClicked: AIChatManager.agentMode = !AIChatManager.agentMode
                }
            }

            // Trusted mode (#1021d): skip confirmations for destructive steps.
            Rectangle {
                visible: AIChatManager.agentMode
                width: trustText.implicitWidth + 12; height: 20; radius: 3
                color: AIAgentManager.trustedMode ? "#aa5533" : PropertiesPanelController.buttonColor
                border.color: PropertiesPanelController.borderColor
                ToolTip.visible: trustArea.containsMouse
                ToolTip.delay: 500
                ToolTip.text: AIAgentManager.trustedMode
                    ? "Trusted mode ON: the agent deletes and overwrites without asking. Click to require confirmations again."
                    : "Destructive steps (delete, overwrite a file, rewrite geometry) ask for confirmation. Click to trust the agent for this machine."
                Text {
                    id: trustText
                    anchors.centerIn: parent
                    text: AIAgentManager.trustedMode ? "trusted" : "ask"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 10
                }
                MouseArea {
                    id: trustArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: AIAgentManager.trustedMode = !AIAgentManager.trustedMode
                }
            }

            // Model status dot + name — click to open AI Model Settings and switch models.
            Rectangle {
                id: modelChip
                Layout.maximumWidth: 150
                implicitWidth: modelChipRow.implicitWidth + 10
                height: 20; radius: 3
                color: modelChipArea.containsMouse ? Qt.lighter(PropertiesPanelController.panelColor, 1.5) : "transparent"
                ToolTip.visible: modelChipArea.containsMouse
                ToolTip.delay: 500
                ToolTip.text: (AIChatManager.modelAvailable ? "Model: " + AIChatManager.currentModelName : "No model loaded")
                              + " — click to open AI Model Settings"
                Row {
                    id: modelChipRow
                    anchors.centerIn: parent
                    spacing: 5
                    Rectangle {
                        width: 8; height: 8; radius: 4
                        anchors.verticalCenter: parent.verticalCenter
                        color: AIChatManager.modelAvailable ? "#44dd44" : "#dd4444"
                    }
                    Text {
                        text: AIChatManager.modelAvailable
                              ? AIChatManager.currentModelName
                              : "No model"
                        color: PropertiesPanelController.textColor
                        font.pixelSize: 10
                        elide: Text.ElideMiddle
                        width: Math.min(implicitWidth, 120)
                    }
                }
                MouseArea {
                    id: modelChipArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: AIChatManager.openModelSettings()
                }
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

    // ---- Model recommendation (#1021e) ----
    Rectangle {
        id: modelHint
        anchors { top: header.bottom; left: parent.left; right: parent.right }
        visible: AIChatManager.agentMode && AIChatManager.modelAvailable
                 && !AIAgentManager.modelIsRecommended(AIChatManager.currentModelName)
        height: visible ? hintText.implicitHeight + 8 : 0
        color: Qt.rgba(1, 0.8, 0.3, 0.08)
        Text {
            id: hintText
            anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; margins: 8 }
            wrapMode: Text.Wrap
            font.pixelSize: 9
            opacity: 0.75
            color: PropertiesPanelController.textColor
            text: "Tip: the loaded model (" + AIChatManager.currentModelName + ") is small for multi-step tool calls. "
                  + AIAgentManager.recommendedModelName + " is the recommended agent model (AI → AI Model Settings → Recommended)."
        }
    }

    // ---- Single selectable conversation area ----
    Flickable {
        id: msgFlick
        anchors {
            top: modelHint.bottom; left: parent.left; right: parent.right
            bottom: planCard.top; bottomMargin: 0
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

    // ---- Plan card (#1021b): live step list of the running/last agent task ----
    Rectangle {
        id: planCard
        anchors { bottom: confirmBar.top; left: parent.left; right: parent.right; margins: visible ? 6 : 0 }
        visible: AIChatManager.agentMode && AIAgentManager.plan.length > 0
                 && (root.agentBusy || planCard.planCardPinned)
        height: visible ? planCol.implicitHeight + 12 : 0
        radius: 4
        color: PropertiesPanelController.headerColor
        border.color: PropertiesPanelController.borderColor
        property bool planCardPinned: false

        Column {
            id: planCol
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 6 }
            spacing: 2
            RowLayout {
                width: parent.width
                Text {
                    text: AIAgentManager.planTitle.length > 0 ? AIAgentManager.planTitle : "Plan"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 11; font.bold: true
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
                Text {
                    text: AIAgentManager.state
                    color: PropertiesPanelController.textColor
                    opacity: 0.6
                    font.pixelSize: 9
                }
            }
            Repeater {
                model: AIAgentManager.plan
                Row {
                    spacing: 6
                    width: planCol.width
                    Text {
                        width: 14
                        text: modelData.status === "succeeded" ? "✓"
                            : modelData.status === "failed"    ? "✗"
                            : modelData.status === "running"   ? "▶"
                            : modelData.status === "skipped"   ? "–"
                            : modelData.status === "repaired"  ? "↻" : "○"
                        color: modelData.status === "succeeded" ? "#66cc66"
                             : modelData.status === "failed"    ? "#dd5555"
                             : modelData.status === "running"   ? PropertiesPanelController.accentColor
                             : PropertiesPanelController.textColor
                        font.pixelSize: 11
                    }
                    Text {
                        width: parent.width - 20
                        text: (modelData.index + 1) + ". " + modelData.tool
                              + (modelData.why.length > 0 ? " — " + modelData.why : "")
                              + (modelData.status === "failed" && modelData.error.length > 0 ? "  (" + modelData.error + ")" : "")
                        color: PropertiesPanelController.textColor
                        opacity: (modelData.status === "skipped" || modelData.status === "repaired") ? 0.5 : 0.9
                        font.pixelSize: 10
                        elide: Text.ElideRight
                        maximumLineCount: 2
                        wrapMode: Text.Wrap
                    }
                }
            }
        }
    }

    // ---- Confirmation bar (#1021d) ----
    Rectangle {
        id: confirmBar
        anchors { bottom: thinkingRow.top; left: parent.left; right: parent.right; margins: visible ? 6 : 0 }
        visible: root.awaitingConfirm
        height: visible ? confirmCol.implicitHeight + 12 : 0
        radius: 4
        color: Qt.rgba(0.9, 0.5, 0.2, 0.15)
        border.color: "#cc7733"

        Column {
            id: confirmCol
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 6 }
            spacing: 6
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                text: "⚠ " + AIAgentManager.pendingConfirmation + ". Allow?"
                color: PropertiesPanelController.textColor
                font.pixelSize: 11
            }
            Flow {
                width: parent.width
                spacing: 6
                Repeater {
                    model: [
                        { label: "Allow",        approve: true,  always: false },
                        { label: "Always allow", approve: true,  always: true  },
                        { label: "Skip step",    approve: false, always: false }
                    ]
                    Rectangle {
                        width: btnText.implicitWidth + 16; height: 22; radius: 3
                        color: btnArea.containsMouse ? PropertiesPanelController.accentColor : PropertiesPanelController.buttonColor
                        border.color: PropertiesPanelController.borderColor
                        Text { id: btnText; anchors.centerIn: parent; text: modelData.label; color: PropertiesPanelController.textColor; font.pixelSize: 10 }
                        MouseArea {
                            id: btnArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: AIAgentManager.confirmPendingStep(modelData.approve, modelData.always)
                        }
                    }
                }
            }
        }
    }

    // ---- Thinking dots (no tokens yet) ----
    Row {
        id: thinkingRow
        anchors { bottom: inputRow.top; left: parent.left; leftMargin: 12; bottomMargin: 6 }
        height: visible ? 14 : 0
        spacing: 4
        visible: AIChatManager.isGenerating && !root.awaitingConfirm

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
        Text {
            visible: AIChatManager.agentMode && root.agentBusy
            text: AIAgentManager.state
            color: PropertiesPanelController.textColor
            opacity: 0.5
            font.pixelSize: 9
            anchors.verticalCenter: parent.verticalCenter
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
                             ? (AIChatManager.agentMode ? "Describe a task — e.g. \"load the wolf, rig it as a quadruped and export a glb\""
                                                        : "Ask AI to do something…")
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
            text: AIChatManager.agentMode ? "Give me a task — I plan it, run it step by step, and it undoes as one"
                                          : "Ask me to control the editor"
            color: PropertiesPanelController.textColor
            font.pixelSize: 12; opacity: 0.7
            horizontalAlignment: Text.AlignHCenter
            width: root.width - 40
            wrapMode: Text.Wrap
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: AIChatManager.agentMode ? "\"segment the wolf, rig it as a quadruped and skin it\""
                                          : "\"make the selected mesh twice as large\""
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
            if (isTool)                  { roleColor = "#88cc88"; roleLabel = "⚙ tool" }
            else if (role === "user")    { roleColor = "#88aacc"; roleLabel = "you" }
            else if (role === "plan")    { roleColor = "#ccaa66"; roleLabel = "plan" }
            else                         { roleColor = "#aaaaaa"; roleLabel = "assistant" }

            // Assistant messages may be structured JSON (v1 loop) — render appropriately.
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

        // In-progress streaming text (v1 loop only)
        if (AIChatManager.streamingText.length > 0) {
            if (msgs.length > 0) html += "<br>"
            html += '<font color="#aaaaaa"><small><b>assistant</b></small></font><br>'
            html += escHtml(AIChatManager.streamingText)
        }

        return html
    }
}
