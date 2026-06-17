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
        ButtonGroup { id: channelGroup }
        Row {
            spacing: 6
            Repeater {
                model: ["stable", "beta"]
                Button {
                    ButtonGroup.group: channelGroup
                    checkable: true
                    checked: UpdaterController.channel === modelData
                    text: modelData
                    implicitHeight: 26
                    implicitWidth: 72
                    onClicked: UpdaterController.channel = modelData
                    palette.button: checked ? highlightColor : inputBgColor
                    palette.buttonText: checked ? "white" : textColor
                }
            }
        }
    }

    CheckBox {
        id: startupCheck
        width: parent.width
        text: "Check for updates on startup"
        checked: UpdaterController.checkOnStartup
        onToggled: UpdaterController.checkOnStartup = checked
        font.pixelSize: 12
        indicator: Rectangle {
            implicitWidth: 14
            implicitHeight: 14
            x: startupCheck.leftPadding
            y: parent.height / 2 - height / 2
            radius: 2
            border.color: borderColor
            border.width: 1
            color: startupCheck.checked ? highlightColor : "transparent"
            Text {
                anchors.centerIn: parent
                visible: startupCheck.checked
                text: "\u2713"
                color: "white"
                font.pixelSize: 10
            }
        }
        contentItem: Text {
            text: startupCheck.text
            font: startupCheck.font
            color: textColor
            verticalAlignment: Text.AlignVCenter
            leftPadding: startupCheck.indicator.width + startupCheck.spacing
        }
    }

    CheckBox {
        id: autoDownloadCheck
        width: parent.width
        text: "Auto-download in background (opt-in)"
        checked: UpdaterController.autoDownload
        onToggled: UpdaterController.autoDownload = checked
        font.pixelSize: 12
        indicator: Rectangle {
            implicitWidth: 14
            implicitHeight: 14
            x: autoDownloadCheck.leftPadding
            y: parent.height / 2 - height / 2
            radius: 2
            border.color: borderColor
            border.width: 1
            color: autoDownloadCheck.checked ? highlightColor : "transparent"
            Text {
                anchors.centerIn: parent
                visible: autoDownloadCheck.checked
                text: "\u2713"
                color: "white"
                font.pixelSize: 10
            }
        }
        contentItem: Text {
            text: autoDownloadCheck.text
            font: autoDownloadCheck.font
            color: textColor
            verticalAlignment: Text.AlignVCenter
            leftPadding: autoDownloadCheck.indicator.width + autoDownloadCheck.spacing
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

    Button {
        text: "Check now"
        implicitHeight: 28
        implicitWidth: 120
        onClicked: UpdaterController.requestCheckDialog()
        palette.button: highlightColor
        palette.buttonText: "white"
        font.pixelSize: 12
    }
}
