import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import PropertiesPanel 1.0
import Updater 1.0

Column {
    id: updatesRoot
    width: parent ? parent.width : 400
    spacing: 12
    property int currentTab: 3
    visible: currentTab === 3
    property color textColor: PropertiesPanelController.textColor
    property color dimTextColor: Qt.darker(PropertiesPanelController.textColor, 1.35)
    property color borderColor: PropertiesPanelController.borderColor
    property color highlightColor: PropertiesPanelController.highlightColor
    property color inputBgColor: PropertiesPanelController.inputColor

    Text {
        width: parent.width
        text: "Updates"
        font.pixelSize: 12
        font.bold: true
        color: textColor
    }

    Text {
        width: parent.width
        wrapMode: Text.WordWrap
        font.pixelSize: 11
        color: dimTextColor
        text: "Install flavor: " + UpdaterController.flavorDisplayName +
              (UpdaterController.isPackageManaged
               ? " — in-app download is disabled; use your package manager."
               : "")
    }

    Column {
        width: parent.width
        spacing: 4
        Text { text: "Channel"; font.pixelSize: 12; font.bold: true; color: textColor }
        Row {
            spacing: 6
            Repeater {
                model: ["stable", "beta"]
                Rectangle {
                    width: 72; height: 26; radius: 3
                    border.color: borderColor; border.width: 1
                    color: UpdaterController.channel === modelData ? highlightColor
                         : channelMouse.containsMouse ? Qt.lighter(inputBgColor, 1.1) : inputBgColor
                    Text {
                        anchors.centerIn: parent
                        text: modelData
                        font.pixelSize: 11
                        color: UpdaterController.channel === modelData ? "white" : textColor
                    }
                    MouseArea {
                        id: channelMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: UpdaterController.channel = modelData
                    }
                }
            }
        }
    }

    Row {
        spacing: 6
        width: parent.width
        property bool enabled: UpdaterController.checkOnStartup
        Rectangle {
            width: 14; height: 14; anchors.verticalCenter: parent.verticalCenter
            border.color: borderColor; border.width: 1; radius: 2
            color: parent.enabled ? highlightColor : "transparent"
            Text { anchors.centerIn: parent; text: parent.parent.enabled ? "\u2713" : ""; color: "white"; font.pixelSize: 10 }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: UpdaterController.checkOnStartup = !parent.parent.enabled
            }
        }
        Text {
            text: "Check for updates on startup"
            font.pixelSize: 12
            color: textColor
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    Row {
        spacing: 6
        width: parent.width
        property bool enabled: UpdaterController.autoDownload
        Rectangle {
            width: 14; height: 14; anchors.verticalCenter: parent.verticalCenter
            border.color: borderColor; border.width: 1; radius: 2
            color: parent.enabled ? highlightColor : "transparent"
            Text { anchors.centerIn: parent; text: parent.parent.enabled ? "\u2713" : ""; color: "white"; font.pixelSize: 10 }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: UpdaterController.autoDownload = !parent.parent.enabled
            }
        }
        Text {
            text: "Auto-download in background (opt-in)"
            font.pixelSize: 12
            color: textColor
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    Text {
        width: parent.width
        font.pixelSize: 11
        color: dimTextColor
        text: UpdaterController.lastCheckedAt.length > 0
              ? "Last checked: " + UpdaterController.lastCheckedAt
              : "Last checked: never"
    }

    Rectangle {
        width: 120; height: 28; radius: 3
        color: checkMouse.containsMouse ? Qt.lighter(highlightColor, 1.1) : highlightColor
        Text {
            anchors.centerIn: parent
            text: "Check now"
            color: "white"
            font.pixelSize: 12
        }
        MouseArea {
            id: checkMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: UpdaterController.requestCheckDialog()
        }
    }
}
