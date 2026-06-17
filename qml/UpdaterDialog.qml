import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import PropertiesPanel 1.0
import Updater 1.0

Window {
    id: dialog
    title: "Check for Updates"
    width: 560
    height: 520
    minimumWidth: 480
    minimumHeight: 420
    flags: Qt.Dialog
    modality: Qt.ApplicationModal
    color: PropertiesPanelController.panelColor

    property bool autoCheck: true

    function open(runCheck) {
        if (runCheck !== undefined)
            dialog.autoCheck = runCheck
        dialog.show()
        dialog.raise()
        dialog.requestActivate()
        keyCapture.forceActiveFocus()
        if (dialog.autoCheck)
            UpdaterController.checkForUpdates()
    }

    Item {
        id: keyCapture
        anchors.fill: parent
        focus: true
        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_Escape) {
                if (UpdaterController.state === UpdaterController.Checking
                    || UpdaterController.state === UpdaterController.Downloading
                    || UpdaterController.state === UpdaterController.Verifying) {
                    UpdaterController.cancel()
                }
                UpdaterController.dismiss()
                dialog.close()
                event.accepted = true
            }
        }
    }

    component InspectorButton: Rectangle {
        id: btn
        property string label: ""
        property bool buttonEnabled: true
        property bool primary: false
        signal clicked()
        height: 28
        radius: 3
        color: !buttonEnabled ? Qt.darker(PropertiesPanelController.controlBgColor, 1.2)
             : btnMouse.containsMouse ? Qt.lighter(primary ? PropertiesPanelController.highlightColor
                                                          : PropertiesPanelController.controlBgColor, 1.15)
             : (primary ? PropertiesPanelController.highlightColor
                        : PropertiesPanelController.controlBgColor)
        border.color: PropertiesPanelController.borderColor
        border.width: 1
        Text {
            anchors.centerIn: parent
            text: btn.label
            color: primary ? "white" : PropertiesPanelController.textColor
            font.pixelSize: 12
        }
        MouseArea {
            id: btnMouse
            anchors.fill: parent
            hoverEnabled: true
            enabled: btn.buttonEnabled
            cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: btn.clicked()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            font.pixelSize: 16
            font.bold: true
            color: PropertiesPanelController.textColor
            text: {
                switch (UpdaterController.state) {
                case UpdaterController.Checking: return "Checking for updates…"
                case UpdaterController.UnknownInstall: return "Unrecognized install layout"
                case UpdaterController.PackageManaged: return "Updates via " + UpdaterController.flavorDisplayName
                case UpdaterController.UpdateAvailable: return "Update available"
                case UpdaterController.UpToDate: return "You're up to date"
                case UpdaterController.Error: return "Update check failed"
                case UpdaterController.Downloading: return "Downloading update…"
                case UpdaterController.Verifying: return "Verifying download…"
                case UpdaterController.ReadyToInstall: return "Ready to install"
                default: return "Software updates"
                }
            }
        }

        Text {
            Layout.fillWidth: true
            visible: UpdaterController.currentVersion.length > 0
                     && UpdaterController.state !== UpdaterController.PackageManaged
            text: "Current version: " + UpdaterController.currentVersion
            color: Qt.darker(PropertiesPanelController.textColor, 1.35)
            font.pixelSize: 11
        }

        BusyIndicator {
            Layout.alignment: Qt.AlignHCenter
            running: UpdaterController.state === UpdaterController.Checking
                     || UpdaterController.state === UpdaterController.Downloading
                     || UpdaterController.state === UpdaterController.Verifying
            visible: running
        }

        // Unknown portable install confirmation (#443)
        ColumnLayout {
            Layout.fillWidth: true
            visible: UpdaterController.state === UpdaterController.UnknownInstall
            spacing: 8
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: PropertiesPanelController.textColor
                font.pixelSize: 12
                text: "This install path doesn't match a known portable or package-manager layout. " +
                      "In-app updates are only supported for direct-download installs. Continue anyway?"
            }
            RowLayout {
                spacing: 8
                InspectorButton {
                    label: "Continue"
                    primary: true
                    onClicked: UpdaterController.confirmUnknownInstall()
                }
                InspectorButton {
                    label: "Cancel"
                    onClicked: { UpdaterController.dismiss(); dialog.close() }
                }
            }
        }

        // Package-manager guidance (#443)
        ColumnLayout {
            Layout.fillWidth: true
            visible: UpdaterController.state === UpdaterController.PackageManaged
            spacing: 8
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: PropertiesPanelController.textColor
                font.pixelSize: 12
                text: "Your copy was installed via " + UpdaterController.flavorDisplayName +
                      ". Use your package manager to update:"
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: commandField.implicitHeight + 12
                color: PropertiesPanelController.inputColor
                border.color: PropertiesPanelController.borderColor
                radius: 3
                Text {
                    id: commandField
                    anchors.fill: parent
                    anchors.margins: 6
                    wrapMode: Text.Wrap
                    font.family: "monospace"
                    font.pixelSize: 11
                    color: PropertiesPanelController.textColor
                    text: UpdaterController.updateCommandHint
                }
            }
            RowLayout {
                spacing: 8
                InspectorButton {
                    label: "Copy command"
                    buttonEnabled: UpdaterController.updateCommandHint.length > 0
                    onClicked: UpdaterController.copyUpdateCommand()
                }
                InspectorButton {
                    label: "Close"
                    onClicked: { UpdaterController.dismiss(); dialog.close() }
                }
            }
        }

        // Update available
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: UpdaterController.state === UpdaterController.UpdateAvailable
            spacing: 8
            Text {
                Layout.fillWidth: true
                color: PropertiesPanelController.textColor
                font.pixelSize: 12
                text: UpdaterController.latestVersion +
                      (UpdaterController.publishedAt.length > 0
                       ? "  ·  published " + UpdaterController.publishedAt.substring(0, 10)
                       : "")
            }
            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                TextArea {
                    readOnly: true
                    wrapMode: TextArea.Wrap
                    textFormat: TextEdit.MarkdownText
                    text: UpdaterController.changelog.length > 0
                          ? UpdaterController.changelog
                          : "_No release notes provided._"
                    color: PropertiesPanelController.textColor
                    background: Rectangle {
                        color: PropertiesPanelController.inputColor
                        border.color: PropertiesPanelController.borderColor
                        radius: 3
                    }
                }
            }
            RowLayout {
                spacing: 8
                InspectorButton {
                    label: "Open release page"
                    primary: true
                    onClicked: UpdaterController.openReleasePage()
                }
                InspectorButton {
                    label: "Download & install"
                    onClicked: UpdaterController.downloadAndInstall()
                }
            }
            RowLayout {
                spacing: 8
                InspectorButton {
                    label: "Remind me later"
                    onClicked: { UpdaterController.remindLater(); dialog.close() }
                }
                InspectorButton {
                    label: "Skip this version"
                    onClicked: { UpdaterController.skipThisVersion(); dialog.close() }
                }
            }
        }

        // Up to date
        ColumnLayout {
            Layout.fillWidth: true
            visible: UpdaterController.state === UpdaterController.UpToDate
            spacing: 8
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: PropertiesPanelController.textColor
                font.pixelSize: 12
                text: UpdaterController.latestVersion.length > 0
                      ? "You already have the latest release (" + UpdaterController.latestVersion + ")."
                      : "You already have the latest release."
            }
            InspectorButton {
                label: "Close"
                onClicked: { UpdaterController.dismiss(); dialog.close() }
            }
        }

        // Ready to install (#444 — install step lands in #446–448)
        ColumnLayout {
            Layout.fillWidth: true
            visible: UpdaterController.state === UpdaterController.ReadyToInstall
            spacing: 8
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: PropertiesPanelController.textColor
                font.pixelSize: 12
                text: "Update " + UpdaterController.latestVersion +
                      " downloaded and verified. Automatic installation will be available in a future release."
            }
            InspectorButton {
                label: "Close"
                onClicked: { UpdaterController.dismiss(); dialog.close() }
            }
        }

        // Error
        ColumnLayout {
            Layout.fillWidth: true
            visible: UpdaterController.state === UpdaterController.Error
            spacing: 8
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: "#cc4444"
                font.pixelSize: 12
                text: UpdaterController.error
            }
            InspectorButton {
                label: "Close"
                onClicked: { UpdaterController.dismiss(); dialog.close() }
            }
        }

        ProgressBar {
            Layout.fillWidth: true
            visible: UpdaterController.state === UpdaterController.Downloading
            from: 0
            to: 100
            value: UpdaterController.progress
        }
    }
}
