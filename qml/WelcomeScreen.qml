import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import WelcomeScreen 1.0
import PropertiesPanel 1.0

Rectangle {
    id: root
    color: "#c0000000"  // semi-transparent dark overlay
    visible: WelcomeScreenController.visible

    // Click on the overlay background dismisses the welcome screen
    MouseArea {
        anchors.fill: parent
        onClicked: WelcomeScreenController.dismiss(dontShowCheckbox.checked)
    }

    // Centered card
    Rectangle {
        id: card
        anchors.centerIn: parent
        width: Math.min(parent.width - 60, 560)
        height: cardLayout.implicitHeight + 48
        radius: 12
        color: PropertiesPanelController.panelColor
        border.color: PropertiesPanelController.borderColor
        border.width: 1

        // Prevent clicks on the card from dismissing
        MouseArea {
            anchors.fill: parent
            onClicked: {} // absorb click
        }

        ColumnLayout {
            id: cardLayout
            anchors {
                top: parent.top; topMargin: 24
                left: parent.left; leftMargin: 28
                right: parent.right; rightMargin: 28
            }
            spacing: 16

            // ---- Header: Title ----
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4
                Layout.alignment: Qt.AlignHCenter

                Text {
                    text: "QtMeshEditor"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 22
                    font.bold: true
                    Layout.alignment: Qt.AlignHCenter
                }

                Text {
                    text: "3D Mesh Editor & Converter"
                    color: Qt.darker(PropertiesPanelController.textColor, 1.4)
                    font.pixelSize: 12
                    Layout.alignment: Qt.AlignHCenter
                }
            }

            // ---- Separator ----
            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: PropertiesPanelController.borderColor
            }

            // ---- Quick Start Buttons ----
            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                // New Scene button
                Rectangle {
                    Layout.fillWidth: true
                    height: 40
                    radius: 6
                    color: newSceneMA.containsMouse
                        ? Qt.lighter(PropertiesPanelController.highlightColor, 1.1)
                        : PropertiesPanelController.highlightColor
                    Text {
                        anchors.centerIn: parent
                        text: "New Scene"
                        color: "#ffffff"
                        font.pixelSize: 13
                        font.bold: true
                    }
                    MouseArea {
                        id: newSceneMA
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: WelcomeScreenController.newScene()
                    }
                }

                // Open File button
                Rectangle {
                    Layout.fillWidth: true
                    height: 40
                    radius: 6
                    color: openFileMA.containsMouse
                        ? Qt.lighter(PropertiesPanelController.inputColor, 1.3)
                        : PropertiesPanelController.inputColor
                    border.color: PropertiesPanelController.borderColor
                    border.width: 1
                    Text {
                        anchors.centerIn: parent
                        text: "Open File..."
                        color: PropertiesPanelController.textColor
                        font.pixelSize: 13
                        font.bold: true
                    }
                    MouseArea {
                        id: openFileMA
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: WelcomeScreenController.openFileDialog()
                    }
                }
            }

            // ---- Recent Files ----
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4
                visible: WelcomeScreenController.recentFiles.length > 0

                Text {
                    text: "Recent Files"
                    color: Qt.darker(PropertiesPanelController.textColor, 1.3)
                    font.pixelSize: 11
                    font.bold: true
                    font.capitalization: Font.AllUppercase
                }

                Repeater {
                    model: WelcomeScreenController.recentFiles

                    Rectangle {
                        Layout.fillWidth: true
                        height: 30
                        radius: 4
                        color: recentMA.containsMouse
                            ? Qt.lighter(PropertiesPanelController.headerColor, 1.2)
                            : "transparent"

                        RowLayout {
                            anchors {
                                fill: parent
                                leftMargin: 8
                                rightMargin: 8
                            }
                            spacing: 6

                            Text {
                                text: WelcomeScreenController.recentFileNames[index]
                                color: PropertiesPanelController.textColor
                                font.pixelSize: 12
                                elide: Text.ElideMiddle
                                Layout.fillWidth: true
                            }
                            Text {
                                text: modelData
                                color: Qt.darker(PropertiesPanelController.textColor, 1.6)
                                font.pixelSize: 10
                                elide: Text.ElideLeft
                                Layout.maximumWidth: 200
                            }
                        }

                        MouseArea {
                            id: recentMA
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: WelcomeScreenController.openFile(modelData)
                        }
                    }
                }
            }

            // ---- Separator ----
            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: PropertiesPanelController.borderColor
            }

            // ---- Feature Tips ----
            Text {
                text: "Quick Tips"
                color: Qt.darker(PropertiesPanelController.textColor, 1.3)
                font.pixelSize: 11
                font.bold: true
                font.capitalization: Font.AllUppercase
            }

            GridLayout {
                Layout.fillWidth: true
                columns: 3
                columnSpacing: 10
                rowSpacing: 8

                // Tip 1: AI Chat
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: tipCol1.implicitHeight + 16
                    radius: 6
                    color: PropertiesPanelController.headerColor
                    border.color: PropertiesPanelController.borderColor
                    border.width: 1

                    ColumnLayout {
                        id: tipCol1
                        anchors {
                            left: parent.left; right: parent.right
                            top: parent.top
                            margins: 8
                        }
                        spacing: 2

                        Text {
                            text: "\u2728 AI Chat"
                            color: PropertiesPanelController.textColor
                            font.pixelSize: 12
                            font.bold: true
                        }
                        Text {
                            text: "Describe changes in natural language. Open from the toolbar or AI menu."
                            color: Qt.darker(PropertiesPanelController.textColor, 1.4)
                            font.pixelSize: 10
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                    }
                }

                // Tip 2: Keyboard Shortcuts
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: tipCol2.implicitHeight + 16
                    radius: 6
                    color: PropertiesPanelController.headerColor
                    border.color: PropertiesPanelController.borderColor
                    border.width: 1

                    ColumnLayout {
                        id: tipCol2
                        anchors {
                            left: parent.left; right: parent.right
                            top: parent.top
                            margins: 8
                        }
                        spacing: 2

                        Text {
                            text: "\u2328 Shortcuts"
                            color: PropertiesPanelController.textColor
                            font.pixelSize: 12
                            font.bold: true
                        }
                        Text {
                            text: "Q Select, W Move, E Rotate, R Scale, F Frame, X Toggle space."
                            color: Qt.darker(PropertiesPanelController.textColor, 1.4)
                            font.pixelSize: 10
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                    }
                }

                // Tip 3: CLI Pipeline
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: tipCol3.implicitHeight + 16
                    radius: 6
                    color: PropertiesPanelController.headerColor
                    border.color: PropertiesPanelController.borderColor
                    border.width: 1

                    ColumnLayout {
                        id: tipCol3
                        anchors {
                            left: parent.left; right: parent.right
                            top: parent.top
                            margins: 8
                        }
                        spacing: 2

                        Text {
                            text: "> CLI Pipeline"
                            color: PropertiesPanelController.textColor
                            font.pixelSize: 12
                            font.bold: true
                        }
                        Text {
                            text: "Use 'qtmesh' for batch ops: convert, validate, LOD, animations."
                            color: Qt.darker(PropertiesPanelController.textColor, 1.4)
                            font.pixelSize: 10
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                    }
                }
            }

            // ---- Separator ----
            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: PropertiesPanelController.borderColor
            }

            // ---- Don't show again + Dismiss ----
            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                CheckBox {
                    id: dontShowCheckbox
                    text: "Don't show again"
                    checked: false

                    contentItem: Text {
                        text: dontShowCheckbox.text
                        color: Qt.darker(PropertiesPanelController.textColor, 1.3)
                        font.pixelSize: 11
                        leftPadding: dontShowCheckbox.indicator.width + 6
                        verticalAlignment: Text.AlignVCenter
                    }

                    indicator: Rectangle {
                        implicitWidth: 16
                        implicitHeight: 16
                        x: dontShowCheckbox.leftPadding
                        y: (dontShowCheckbox.height - height) / 2
                        radius: 3
                        color: PropertiesPanelController.inputColor
                        border.color: PropertiesPanelController.borderColor
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: "\u2713"
                            color: PropertiesPanelController.textColor
                            font.pixelSize: 12
                            visible: dontShowCheckbox.checked
                        }
                    }
                }

                Item { Layout.fillWidth: true }

                Rectangle {
                    width: 90
                    height: 28
                    radius: 4
                    color: dismissMA.containsMouse
                        ? Qt.lighter(PropertiesPanelController.inputColor, 1.3)
                        : PropertiesPanelController.inputColor
                    border.color: PropertiesPanelController.borderColor
                    border.width: 1

                    Text {
                        anchors.centerIn: parent
                        text: "Get Started"
                        color: PropertiesPanelController.textColor
                        font.pixelSize: 12
                    }
                    MouseArea {
                        id: dismissMA
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: WelcomeScreenController.dismiss(dontShowCheckbox.checked)
                    }
                }
            }

            // Bottom spacer for padding
            Item { height: 4 }
        }
    }

    // Dismiss on Escape key
    Keys.onEscapePressed: WelcomeScreenController.dismiss(dontShowCheckbox.checked)

    // Grab focus when visible so Escape works
    onVisibleChanged: {
        if (visible) forceActiveFocus()
    }
}
