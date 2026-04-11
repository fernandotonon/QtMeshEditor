import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import PropertiesPanel 1.0

Rectangle {
    id: root
    color: backgroundColor

    SystemPalette {
        id: palette
        colorGroup: SystemPalette.Active
    }

    property color backgroundColor: palette.window
    property color panelColor: palette.base
    property color textColor: palette.windowText
    property color borderColor: palette.mid
    property color highlightColor: palette.highlight
    property color buttonColor: palette.button
    property color buttonTextColor: palette.buttonText
    property color dimTextColor: Qt.darker(textColor, 1.4)
    property color inputBgColor: palette.base

    property int currentTab: 0

    // Helper to read a setting with default
    function readSetting(key, defaultVal) {
        return PropertiesPanelController.getSetting(key, defaultVal);
    }

    // Helper to write a setting
    function writeSetting(key, val) {
        PropertiesPanelController.setSetting(key, val);
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Header
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 46
            color: panelColor

            Text {
                text: "Preferences"
                font.pointSize: 14
                font.bold: true
                color: textColor
                anchors.centerIn: parent
            }

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: borderColor
            }
        }

        // Tab bar
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 36
            color: Qt.lighter(backgroundColor, 1.02)

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 2

                Repeater {
                    model: ["General", "Appearance", "Viewport", "AI"]

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        color: currentTab === index ? highlightColor : (tabMouse.containsMouse ? Qt.lighter(backgroundColor, 1.1) : "transparent")
                        radius: 4

                        Text {
                            anchors.centerIn: parent
                            text: modelData
                            font.pixelSize: 12
                            font.bold: currentTab === index
                            color: currentTab === index ? "white" : textColor
                        }

                        MouseArea {
                            id: tabMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: currentTab = index
                        }
                    }
                }
            }

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: borderColor
            }
        }

        // Tab content
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            Flickable {
                contentWidth: parent.width
                contentHeight: contentColumn.implicitHeight

                Column {
                    id: contentColumn
                    width: parent.width
                    spacing: 0
                    padding: 16

                    // --- General Tab ---
                    Column {
                        width: parent.width - 32
                        spacing: 12
                        visible: currentTab === 0

                        // Default save directory
                        Column {
                            width: parent.width
                            spacing: 4

                            Text {
                                text: "Default Save Directory"
                                font.pixelSize: 12
                                font.bold: true
                                color: textColor
                            }

                            Rectangle {
                                width: parent.width
                                height: 30
                                color: inputBgColor
                                border.color: saveDirField.activeFocus ? highlightColor : borderColor
                                border.width: 1
                                radius: 3

                                TextInput {
                                    id: saveDirField
                                    anchors.fill: parent
                                    anchors.margins: 6
                                    verticalAlignment: TextInput.AlignVCenter
                                    font.pixelSize: 12
                                    color: textColor
                                    clip: true
                                    selectByMouse: true
                                    text: readSetting("General/defaultSaveDir", "")

                                    onEditingFinished: writeSetting("General/defaultSaveDir", text)

                                    Text {
                                        anchors.fill: parent
                                        text: "Browse or type a path..."
                                        color: dimTextColor
                                        font.pixelSize: 12
                                        visible: !saveDirField.text && !saveDirField.activeFocus
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                }
                            }
                        }

                        // Recent files count
                        Column {
                            width: parent.width
                            spacing: 4

                            Text {
                                text: "Recent Files Count"
                                font.pixelSize: 12
                                font.bold: true
                                color: textColor
                            }

                            RowLayout {
                                spacing: 8

                                Rectangle {
                                    width: 80
                                    height: 30
                                    color: inputBgColor
                                    border.color: recentCountField.activeFocus ? highlightColor : borderColor
                                    border.width: 1
                                    radius: 3

                                    TextInput {
                                        id: recentCountField
                                        anchors.fill: parent
                                        anchors.margins: 6
                                        verticalAlignment: TextInput.AlignVCenter
                                        font.pixelSize: 12
                                        color: textColor
                                        clip: true
                                        selectByMouse: true
                                        validator: IntValidator { bottom: 1; top: 50 }
                                        text: readSetting("General/recentFilesCount", 10).toString()

                                        onEditingFinished: writeSetting("General/recentFilesCount", parseInt(text) || 10)
                                    }
                                }

                                Text {
                                    text: "(1-50)"
                                    font.pixelSize: 11
                                    color: dimTextColor
                                }
                            }
                        }

                        // Telemetry opt-out
                        Rectangle {
                            width: parent.width
                            height: 40
                            color: "transparent"

                            RowLayout {
                                anchors.fill: parent
                                spacing: 8

                                CheckBox {
                                    id: telemetryCheck
                                    checked: readSetting("Telemetry/enabled", true) === true
                                           || readSetting("Telemetry/enabled", true) === "true"
                                    onToggled: writeSetting("Telemetry/enabled", checked)
                                }

                                Text {
                                    text: "Enable anonymous telemetry"
                                    font.pixelSize: 12
                                    color: textColor
                                    Layout.fillWidth: true

                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: telemetryCheck.toggle()
                                    }
                                }
                            }
                        }

                        Text {
                            text: "Telemetry helps improve QtMeshEditor by sending anonymous usage data."
                            font.pixelSize: 11
                            font.italic: true
                            color: dimTextColor
                            wrapMode: Text.WordWrap
                            width: parent.width
                        }
                    }

                    // --- Appearance Tab ---
                    Column {
                        width: parent.width - 32
                        spacing: 12
                        visible: currentTab === 1

                        Column {
                            width: parent.width
                            spacing: 4

                            Text {
                                text: "Theme"
                                font.pixelSize: 12
                                font.bold: true
                                color: textColor
                            }

                            Rectangle {
                                width: parent.width
                                height: 30
                                color: inputBgColor
                                border.color: borderColor
                                border.width: 1
                                radius: 3

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.margins: 4
                                    spacing: 4

                                    Repeater {
                                        model: ["light", "dark", "custom"]

                                        Rectangle {
                                            Layout.fillWidth: true
                                            Layout.fillHeight: true
                                            radius: 2
                                            color: {
                                                var current = readSetting("palette", "dark");
                                                return current === modelData ? highlightColor : (themeMouse.containsMouse ? Qt.lighter(backgroundColor, 1.1) : "transparent");
                                            }

                                            Text {
                                                anchors.centerIn: parent
                                                text: modelData.charAt(0).toUpperCase() + modelData.slice(1)
                                                font.pixelSize: 11
                                                color: {
                                                    var current = readSetting("palette", "dark");
                                                    return current === modelData ? "white" : textColor;
                                                }
                                            }

                                            MouseArea {
                                                id: themeMouse
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: {
                                                    writeSetting("palette", modelData);
                                                    themeNote.visible = true;
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            Text {
                                id: themeNote
                                text: "Restart the application to apply the theme change."
                                font.pixelSize: 11
                                font.italic: true
                                color: highlightColor
                                visible: false
                                topPadding: 4
                            }
                        }
                    }

                    // --- Viewport Tab ---
                    Column {
                        width: parent.width - 32
                        spacing: 12
                        visible: currentTab === 2

                        // Grid visibility
                        Rectangle {
                            width: parent.width
                            height: 40
                            color: "transparent"

                            RowLayout {
                                anchors.fill: parent
                                spacing: 8

                                CheckBox {
                                    id: gridCheck
                                    checked: readSetting("Viewport/gridVisible", true) === true
                                           || readSetting("Viewport/gridVisible", true) === "true"
                                    onToggled: writeSetting("Viewport/gridVisible", checked)
                                }

                                Text {
                                    text: "Show Grid by Default"
                                    font.pixelSize: 12
                                    color: textColor
                                    Layout.fillWidth: true

                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: gridCheck.toggle()
                                    }
                                }
                            }
                        }

                        // Camera speed
                        Column {
                            width: parent.width
                            spacing: 4

                            Text {
                                text: "Default Camera Speed"
                                font.pixelSize: 12
                                font.bold: true
                                color: textColor
                            }

                            RowLayout {
                                width: parent.width
                                spacing: 8

                                Slider {
                                    id: camSpeedSlider
                                    Layout.fillWidth: true
                                    from: 0.1
                                    to: 10.0
                                    stepSize: 0.1
                                    value: parseFloat(readSetting("Viewport/cameraSpeed", 1.0)) || 1.0
                                    onMoved: writeSetting("Viewport/cameraSpeed", value.toFixed(1))
                                }

                                Text {
                                    text: camSpeedSlider.value.toFixed(1)
                                    font.pixelSize: 12
                                    color: textColor
                                    Layout.preferredWidth: 30
                                    horizontalAlignment: Text.AlignRight
                                }
                            }
                        }

                        // Near clip
                        Column {
                            width: parent.width
                            spacing: 4

                            Text {
                                text: "Near Clip Distance"
                                font.pixelSize: 12
                                font.bold: true
                                color: textColor
                            }

                            Rectangle {
                                width: 120
                                height: 30
                                color: inputBgColor
                                border.color: nearClipField.activeFocus ? highlightColor : borderColor
                                border.width: 1
                                radius: 3

                                TextInput {
                                    id: nearClipField
                                    anchors.fill: parent
                                    anchors.margins: 6
                                    verticalAlignment: TextInput.AlignVCenter
                                    font.pixelSize: 12
                                    color: textColor
                                    clip: true
                                    selectByMouse: true
                                    validator: DoubleValidator { bottom: 0.001; top: 1000; decimals: 3 }
                                    text: readSetting("Viewport/nearClip", "0.1")

                                    onEditingFinished: writeSetting("Viewport/nearClip", parseFloat(text) || 0.1)
                                }
                            }
                        }

                        // Far clip
                        Column {
                            width: parent.width
                            spacing: 4

                            Text {
                                text: "Far Clip Distance"
                                font.pixelSize: 12
                                font.bold: true
                                color: textColor
                            }

                            Rectangle {
                                width: 120
                                height: 30
                                color: inputBgColor
                                border.color: farClipField.activeFocus ? highlightColor : borderColor
                                border.width: 1
                                radius: 3

                                TextInput {
                                    id: farClipField
                                    anchors.fill: parent
                                    anchors.margins: 6
                                    verticalAlignment: TextInput.AlignVCenter
                                    font.pixelSize: 12
                                    color: textColor
                                    clip: true
                                    selectByMouse: true
                                    validator: DoubleValidator { bottom: 1; top: 100000; decimals: 1 }
                                    text: readSetting("Viewport/farClip", "10000")

                                    onEditingFinished: writeSetting("Viewport/farClip", parseFloat(text) || 10000)
                                }
                            }
                        }
                    }

                    // --- AI Tab ---
                    Column {
                        width: parent.width - 32
                        spacing: 12
                        visible: currentTab === 3

                        // Max tokens
                        Column {
                            width: parent.width
                            spacing: 4

                            Text {
                                text: "Default Max Tokens"
                                font.pixelSize: 12
                                font.bold: true
                                color: textColor
                            }

                            Rectangle {
                                width: 120
                                height: 30
                                color: inputBgColor
                                border.color: maxTokensField.activeFocus ? highlightColor : borderColor
                                border.width: 1
                                radius: 3

                                TextInput {
                                    id: maxTokensField
                                    anchors.fill: parent
                                    anchors.margins: 6
                                    verticalAlignment: TextInput.AlignVCenter
                                    font.pixelSize: 12
                                    color: textColor
                                    clip: true
                                    selectByMouse: true
                                    validator: IntValidator { bottom: 64; top: 8192 }
                                    text: readSetting("AI/maxTokens", 512).toString()

                                    onEditingFinished: writeSetting("AI/maxTokens", parseInt(text) || 512)
                                }
                            }

                            Text {
                                text: "Maximum number of tokens to generate (64-8192)"
                                font.pixelSize: 11
                                color: dimTextColor
                            }
                        }

                        // Temperature
                        Column {
                            width: parent.width
                            spacing: 4

                            Text {
                                text: "Temperature"
                                font.pixelSize: 12
                                font.bold: true
                                color: textColor
                            }

                            RowLayout {
                                width: parent.width
                                spacing: 8

                                Slider {
                                    id: temperatureSlider
                                    Layout.fillWidth: true
                                    from: 0.0
                                    to: 1.0
                                    stepSize: 0.05
                                    value: parseFloat(readSetting("AI/temperature", 0.7)) || 0.7
                                    onMoved: writeSetting("AI/temperature", value.toFixed(2))
                                }

                                Text {
                                    text: temperatureSlider.value.toFixed(2)
                                    font.pixelSize: 12
                                    color: textColor
                                    Layout.preferredWidth: 35
                                    horizontalAlignment: Text.AlignRight
                                }
                            }

                            Text {
                                text: "Lower values produce more deterministic output; higher values are more creative."
                                font.pixelSize: 11
                                color: dimTextColor
                                wrapMode: Text.WordWrap
                                width: parent.width
                            }
                        }

                        // Context size
                        Column {
                            width: parent.width
                            spacing: 4

                            Text {
                                text: "Context Size"
                                font.pixelSize: 12
                                font.bold: true
                                color: textColor
                            }

                            Rectangle {
                                width: 120
                                height: 30
                                color: inputBgColor
                                border.color: contextSizeField.activeFocus ? highlightColor : borderColor
                                border.width: 1
                                radius: 3

                                TextInput {
                                    id: contextSizeField
                                    anchors.fill: parent
                                    anchors.margins: 6
                                    verticalAlignment: TextInput.AlignVCenter
                                    font.pixelSize: 12
                                    color: textColor
                                    clip: true
                                    selectByMouse: true
                                    validator: IntValidator { bottom: 512; top: 32768 }
                                    text: readSetting("AI/contextSize", 2048).toString()

                                    onEditingFinished: writeSetting("AI/contextSize", parseInt(text) || 2048)
                                }
                            }

                            Text {
                                text: "Number of context tokens for inference (512-32768)"
                                font.pixelSize: 11
                                color: dimTextColor
                            }
                        }
                    }
                }
            }
        }

        // Footer
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 36
            color: panelColor

            Rectangle {
                anchors.top: parent.top
                width: parent.width
                height: 1
                color: borderColor
            }

            Text {
                anchors.centerIn: parent
                text: "Settings are saved automatically"
                font.pixelSize: 11
                color: dimTextColor
            }
        }
    }
}
