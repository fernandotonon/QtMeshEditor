import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
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
                    model: ["General", "Appearance", "Viewport"]

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        color: currentTab === index ? highlightColor
                             : tabMouse.containsMouse ? Qt.lighter(panelColor, 1.5)
                             : Qt.darker(panelColor, 1.1)
                        border.color: borderColor; border.width: 1
                        radius: 3
                        Behavior on color { ColorAnimation { duration: 50 } }

                        Text {
                            anchors.centerIn: parent
                            text: modelData
                            font.pixelSize: 11
                            color: textColor
                        }

                        MouseArea {
                            id: tabMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                currentTab = index
                                // Force repaint of all tabs
                                var m = parent.parent.parent.model
                                parent.parent.parent.model = null
                                parent.parent.parent.model = m
                            }
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

                        // Telemetry opt-out (themed checkbox matching snap settings)
                        Row {
                            spacing: 6
                            width: parent.width

                            property bool telemetryOn: readSetting("Sentry/enabled", true) === true
                                                    || readSetting("Sentry/enabled", true) === "true"

                            Rectangle {
                                width: 14; height: 14; anchors.verticalCenter: parent.verticalCenter
                                border.color: borderColor; border.width: 1; radius: 2
                                color: parent.telemetryOn ? highlightColor : "transparent"
                                Text { anchors.centerIn: parent; text: parent.parent.telemetryOn ? "\u2713" : ""; color: "white"; font.pixelSize: 10 }
                                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                    onClicked: { parent.parent.telemetryOn = !parent.parent.telemetryOn; writeSetting("Sentry/enabled", parent.parent.telemetryOn) }
                                }
                            }
                            Text { text: "Enable anonymous telemetry"; font.pixelSize: 12; color: textColor; anchors.verticalCenter: parent.verticalCenter }
                        }

                        Text {
                            text: "Telemetry helps improve QtMeshEditor by sending anonymous usage data."
                            font.pixelSize: 11; font.italic: true; color: dimTextColor; wrapMode: Text.WordWrap; width: parent.width
                        }

                        // Welcome screen toggle
                        Row {
                            spacing: 6
                            width: parent.width

                            property bool welcomeOn: !(readSetting("WelcomeScreen/dontShowAgain", false) === true
                                                    || readSetting("WelcomeScreen/dontShowAgain", false) === "true")

                            Rectangle {
                                width: 14; height: 14; anchors.verticalCenter: parent.verticalCenter
                                border.color: borderColor; border.width: 1; radius: 2
                                color: parent.welcomeOn ? highlightColor : "transparent"
                                Text { anchors.centerIn: parent; text: parent.parent.welcomeOn ? "\u2713" : ""; color: "white"; font.pixelSize: 10 }
                                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                    onClicked: { parent.parent.welcomeOn = !parent.parent.welcomeOn; writeSetting("WelcomeScreen/dontShowAgain", !parent.parent.welcomeOn) }
                                }
                            }
                            Text { text: "Show welcome screen on startup"; font.pixelSize: 12; color: textColor; anchors.verticalCenter: parent.verticalCenter }
                        }
                    }

                    // --- Appearance Tab ---
                    Column {
                        id: appearanceCol
                        width: parent.width - 32
                        spacing: 12
                        visible: currentTab === 1

                        property int activeThemeIdx: {
                            var saved = readSetting("palette", "dark")
                            if (saved === "light") return 0
                            if (saved === "dark") return 1
                            return 1
                        }

                        Column {
                            width: parent.width
                            spacing: 4

                            Text {
                                text: "Theme"
                                font.pixelSize: 12; font.bold: true; color: textColor
                            }

                            Flow {
                                spacing: 3
                                width: parent.width

                                Repeater {
                                    model: ["Light", "Dark"]

                                    Rectangle {
                                        width: Math.max(60, (parent.width - 3) / 2)
                                        height: 22; radius: 3
                                        color: index === appearanceCol.activeThemeIdx ? highlightColor
                                             : themeBtnMa.containsMouse ? Qt.lighter(panelColor, 1.5)
                                             : Qt.darker(panelColor, 1.1)
                                        border.color: borderColor; border.width: 1
                                        Behavior on color { ColorAnimation { duration: 50 } }

                                        Text { anchors.centerIn: parent; text: modelData; color: textColor; font.pixelSize: 11 }

                                        MouseArea {
                                            id: themeBtnMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                            onClicked: {
                                                appearanceCol.activeThemeIdx = index
                                                writeSetting("palette", modelData.toLowerCase())
                                                writeSetting("Appearance/theme", modelData)
                                            }
                                        }
                                    }
                                }
                            }

                            Text {
                                text: "Theme is applied immediately."
                                font.pixelSize: 11; font.italic: true; color: dimTextColor; topPadding: 4
                            }
                        }
                    }

                    // --- Viewport Tab ---
                    Column {
                        id: viewportTabColumn
                        width: parent.width - 32
                        spacing: 12
                        visible: currentTab === 2

                        property int msaaSelection: 4
                        Component.onCompleted: {
                            var v = parseInt(readSetting("Viewport/fsaaSamples", 4))
                            if (v === 0 || v === 2 || v === 4 || v === 8)
                                msaaSelection = v
                        }

                        // Grid visibility (themed checkbox)
                        Row {
                            spacing: 6
                            width: parent.width

                            property bool gridOn: readSetting("Viewport/gridVisible", true) === true
                                               || readSetting("Viewport/gridVisible", true) === "true"

                            Rectangle {
                                width: 14; height: 14; anchors.verticalCenter: parent.verticalCenter
                                border.color: borderColor; border.width: 1; radius: 2
                                color: parent.gridOn ? highlightColor : "transparent"
                                Text { anchors.centerIn: parent; text: parent.parent.gridOn ? "\u2713" : ""; color: "white"; font.pixelSize: 10 }
                                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                    onClicked: { parent.parent.gridOn = !parent.parent.gridOn; writeSetting("Viewport/gridVisible", parent.parent.gridOn) }
                                }
                            }
                            Text { text: "Show Grid"; font.pixelSize: 12; color: textColor; anchors.verticalCenter: parent.verticalCenter }
                        }

                        // MSAA (applied immediately; render windows are recreated)
                        Column {
                            width: parent.width
                            spacing: 4

                            Text {
                                text: "Anti-aliasing (MSAA)"
                                font.pixelSize: 12
                                font.bold: true
                                color: textColor
                            }

                            Flow {
                                spacing: 3
                                width: parent.width

                                Repeater {
                                    model: [
                                        { "label": "Off", "value": 0 },
                                        { "label": "2×", "value": 2 },
                                        { "label": "4×", "value": 4 },
                                        { "label": "8×", "value": 8 }
                                    ]

                                    Rectangle {
                                        width: Math.max(44, (parent.width - 9) / 4)
                                        height: 24
                                        radius: 3
                                        color: modelData.value === viewportTabColumn.msaaSelection ? highlightColor
                                             : msaaBtnMa.containsMouse ? Qt.lighter(panelColor, 1.5)
                                             : Qt.darker(panelColor, 1.1)
                                        border.color: borderColor
                                        border.width: 1
                                        Behavior on color { ColorAnimation { duration: 50 } }

                                        Text {
                                            anchors.centerIn: parent
                                            text: modelData.label
                                            color: textColor
                                            font.pixelSize: 11
                                        }

                                        MouseArea {
                                            id: msaaBtnMa
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: {
                                                viewportTabColumn.msaaSelection = modelData.value
                                                writeSetting("Viewport/fsaaSamples", modelData.value)
                                            }
                                        }
                                    }
                                }
                            }

                            Text {
                                text: "Higher values smooth edges but cost more GPU time. Off may look jagged on high-DPI displays."
                                font.pixelSize: 11
                                font.italic: true
                                color: dimTextColor
                                wrapMode: Text.WordWrap
                                width: parent.width
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
                                    text: readSetting("Viewport/nearClip", "0.01")

                                    onEditingFinished: writeSetting("Viewport/nearClip", parseFloat(text) || 0.01)
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
