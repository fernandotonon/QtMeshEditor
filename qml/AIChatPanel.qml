import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import AIChatPanel 1.0
import PropertiesPanel 1.0

Rectangle {
    id: root
    color: PropertiesPanelController.panelColor

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

    // ---- Message list ----
    ListView {
        id: messageList
        anchors {
            top: header.bottom; left: parent.left; right: parent.right
            bottom: inputRow.top
            bottomMargin: 4
        }
        clip: true
        spacing: 6
        topMargin: 8; leftMargin: 8; rightMargin: 8
        model: AIChatManager.messages

        // Scroll to bottom on new messages
        onCountChanged: Qt.callLater(() => messageList.positionViewAtEnd())

        delegate: Item {
            width: messageList.width - 16
            height: bubble.height + 4

            property string msgRole: modelData.role
            property string msgText: modelData.text
            property bool   msgTool: modelData.isTool

            Rectangle {
                id: bubble
                width: parent.width
                height: bubbleColumn.height + 10
                radius: 6
                color: {
                    if (msgTool)    return Qt.rgba(0.2, 0.3, 0.2, 0.6)
                    if (msgRole === "user")      return Qt.rgba(0.2, 0.3, 0.5, 0.7)
                    return Qt.rgba(0.25, 0.25, 0.28, 0.9)
                }
                border.color: PropertiesPanelController.borderColor
                border.width: 1

                Column {
                    id: bubbleColumn
                    anchors { top: parent.top; left: parent.left; right: parent.right;
                              topMargin: 5; leftMargin: 8; rightMargin: 8 }
                    spacing: 2

                    // Role label (small)
                    Text {
                        text: {
                            if (msgTool)                return "⚙ tool"
                            if (msgRole === "user")     return "you"
                            return "assistant"
                        }
                        color: {
                            if (msgTool)                return "#88cc88"
                            if (msgRole === "user")     return "#88aacc"
                            return "#aaaaaa"
                        }
                        font.pixelSize: 9; font.bold: true
                    }

                    TextEdit {
                        id: msgLabel
                        width: parent.width
                        height: contentHeight
                        text: msgText
                        color: PropertiesPanelController.textColor
                        font.pixelSize: 12
                        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                        readOnly: true
                        selectByMouse: true
                        selectionColor: Qt.rgba(0.3, 0.5, 0.8, 0.5)
                        selectedTextColor: PropertiesPanelController.textColor
                        background: null
                    }
                }
            }
        }

        // Streaming bubble (shown while generating)
        footer: Item {
            width: messageList.width - 16
            height: AIChatManager.streamingText.length > 0 ? streamBubble.height + 4 : 0
            visible: AIChatManager.streamingText.length > 0

            Rectangle {
                id: streamBubble
                width: parent.width
                height: streamLabel.contentHeight + 28
                radius: 6
                color: Qt.rgba(0.25, 0.25, 0.28, 0.9)
                border.color: PropertiesPanelController.accentColor
                border.width: 1

                Text {
                    anchors { top: parent.top; left: parent.left; topMargin: 4; leftMargin: 8 }
                    text: "assistant"
                    color: "#aaaaaa"; font.pixelSize: 9; font.bold: true
                }

                TextEdit {
                    id: streamLabel
                    anchors { top: parent.top; left: parent.left; right: parent.right;
                              topMargin: 16; leftMargin: 8; rightMargin: 8 }
                    height: contentHeight
                    text: AIChatManager.streamingText
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 12
                    wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                    readOnly: true
                    selectByMouse: true
                    selectionColor: Qt.rgba(0.3, 0.5, 0.8, 0.5)
                    selectedTextColor: PropertiesPanelController.textColor
                    background: null
                }

                // Animated "thinking" dots when no tokens yet
                Row {
                    anchors { bottom: parent.bottom; bottomMargin: 4; left: parent.left; leftMargin: 8 }
                    spacing: 3
                    visible: AIChatManager.streamingText.length === 0 && AIChatManager.isGenerating

                    Repeater {
                        model: 3
                        Rectangle {
                            width: 5; height: 5; radius: 2.5
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
            }

            Component.onCompleted: {
                if (AIChatManager.streamingText.length > 0)
                    Qt.callLater(() => messageList.positionViewAtEnd())
            }
        }

        // Scroll to bottom when streaming text changes
        Connections {
            target: AIChatManager
            function onStreamingTextChanged() {
                Qt.callLater(() => messageList.positionViewAtEnd())
            }
        }
    }

    // ---- Input row ----
    Rectangle {
        id: inputRow
        anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
        height: Math.max(40, inputField.implicitHeight + 16)
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
            enabled: AIChatManager.modelAvailable && !AIChatManager.isGenerating

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

    function doSend() {
        var txt = inputField.text.trim()
        if (txt.length > 0) {
            AIChatManager.sendMessage(txt)
            inputField.text = ""
        }
    }
}
