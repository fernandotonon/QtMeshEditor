import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import PropertiesPanel 1.0

import Updater 1.0

Window {
    id: toast
    width: 340
    height: 72
    flags: Qt.FramelessWindowHint | Qt.Tool | Qt.WindowStaysOnTopHint
    color: "transparent"
    visible: false

    property string versionText: ""

    function showForVersion(version) {
        versionText = version
        reposition()
        visible = true
        show()
        raise()
        dismissTimer.restart()
    }

    function reposition() {
        const screen = toast.screen
        if (!screen)
            return
        const margin = 16
        x = screen.virtualX + screen.width - width - margin
        y = screen.virtualY + screen.height - height - margin
    }

    onScreenChanged: reposition

    Timer {
        id: dismissTimer
        interval: 12000
        onTriggered: toast.close()
    }

    Rectangle {
        anchors.fill: parent
        radius: 6
        color: PropertiesPanelController.panelColor
        border.color: PropertiesPanelController.borderColor
        border.width: 1

        RowLayout {
            anchors.fill: parent
            anchors.margins: 12
            spacing: 12

            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: PropertiesPanelController.textColor
                font.pixelSize: 12
                text: toast.versionText.length > 0
                      ? qsTr("Version %1 is available").arg(toast.versionText)
                      : qsTr("An update is available")
            }

            Rectangle {
                Layout.preferredWidth: viewLabel.implicitWidth + 16
                Layout.preferredHeight: 28
                radius: 3
                color: viewMouse.containsMouse
                       ? Qt.lighter(PropertiesPanelController.highlightColor, 1.12)
                       : PropertiesPanelController.highlightColor
                Text {
                    id: viewLabel
                    anchors.centerIn: parent
                    text: qsTr("View update")
                    color: "white"
                    font.pixelSize: 12
                }
                MouseArea {
                    id: viewMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        dismissTimer.stop()
                        UpdaterController.openUpdateDialog()
                        toast.close()
                    }
                }
            }
        }
    }
}
