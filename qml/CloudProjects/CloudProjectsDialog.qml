import QtQuick
import QtQuick.Layouts
import QtQuick.Window
import MaterialEditorQML 1.0
import PropertiesPanel 1.0
import CloudProjects 1.0

// Issue #691: paginated My Cloud Projects list (download stubbed for a follow-up epic).
Window {
    id: dialog
    title: "My Cloud Projects"
    width: 720
    height: 520
    minimumWidth: 640
    minimumHeight: 420
    flags: Qt.Dialog
    modality: Qt.ApplicationModal
    color: PropertiesPanelController.panelColor

    property string pendingDeleteId: ""
    property string pendingDeleteName: ""
    property string statusMessage: ""
    property bool statusIsError: false

    function open() {
        dialog.statusMessage = ""
        dialog.statusIsError = false
        dialog.show()
        dialog.raise()
        dialog.requestActivate()
        CloudProjectsController.refresh()
        keyCapture.forceActiveFocus()
    }

    Connections {
        target: CloudProjectsController
        function onSignInRequired() {
            dialog.statusMessage = "Sign in to view your cloud projects."
            dialog.statusIsError = true
        }
        function onDeleteFailed(projectId, error) {
            dialog.statusMessage = "Could not delete project: " + error
            dialog.statusIsError = true
        }
        function onUploadRequested() {
            dialog.close()
        }
    }

    Item {
        id: keyCapture
        anchors.fill: parent
        focus: true
        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_Escape) {
                if (confirmDelete.visible) {
                    confirmDelete.visible = false
                } else {
                    dialog.close()
                }
                event.accepted = true
            }
        }
    }

    component InspectorButton: Rectangle {
        id: btn
        property string label: ""
        property bool buttonEnabled: true
        signal clicked()
        activeFocusOnTab: buttonEnabled
        height: 26
        radius: 3
        color: btnMa.containsMouse && buttonEnabled
            ? PropertiesPanelController.highlightColor
            : PropertiesPanelController.headerColor
        border.color: btn.activeFocus
            ? PropertiesPanelController.highlightColor
            : PropertiesPanelController.borderColor
        border.width: btn.activeFocus ? 2 : 1
        opacity: buttonEnabled ? 1.0 : 0.45
        Text {
            anchors.centerIn: parent
            text: btn.label
            color: PropertiesPanelController.textColor
            font.pixelSize: 11
        }
        MouseArea {
            id: btnMa
            anchors.fill: parent
            hoverEnabled: true
            enabled: btn.buttonEnabled
            cursorShape: btn.buttonEnabled ? Qt.PointingHandCursor : Qt.ForbiddenCursor
            onClicked: btn.clicked()
        }
    }

    component InspectorLabel: Text {
        color: PropertiesPanelController.textColor
        font.pixelSize: 11
        wrapMode: Text.WordWrap
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            InspectorLabel {
                text: "Projects on QtMesh Cloud"
                font.pixelSize: 14
                font.bold: true
                Layout.fillWidth: true
            }
            InspectorButton {
                label: "Refresh"
                buttonEnabled: !CloudProjectsController.loading
                onClicked: CloudProjectsController.refresh()
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: PropertiesPanelController.headerColor
            border.color: PropertiesPanelController.borderColor
            radius: 4

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 8
                spacing: 8
                visible: CloudProjectsController.projects.length > 0

                ListView {
                    id: projectList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 6
                    model: CloudProjectsController.projects

                    delegate: Rectangle {
                        width: projectList.width
                        height: 72
                        radius: 4
                        color: PropertiesPanelController.panelColor
                        border.color: PropertiesPanelController.borderColor

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 10
                            spacing: 10

                            Text {
                                text: CloudProjectsController.formatIconForSource(
                                          modelData.sourceFormat || "")
                                font.pixelSize: 22
                                Layout.alignment: Qt.AlignVCenter
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                InspectorLabel {
                                    text: modelData.name || modelData.projectSlug || modelData.id
                                    font.bold: true
                                    font.pixelSize: 12
                                    Layout.fillWidth: true
                                }
                                InspectorLabel {
                                    text: (modelData.sourceFormat || "asset").toUpperCase()
                                          + " · "
                                          + CloudProjectsController.formatFileSize(
                                                modelData.sizeBytes || 0)
                                          + " · "
                                          + CloudProjectsController.formatUpdatedAt(
                                                modelData.updatedAt || "")
                                    color: "#9a9a9a"
                                    Layout.fillWidth: true
                                }
                                InspectorLabel {
                                    visible: !!(modelData.mainFile)
                                    text: modelData.mainFile
                                    color: "#9a9a9a"
                                    font.pixelSize: 10
                                    Layout.fillWidth: true
                                    elide: Text.ElideMiddle
                                }
                            }

                            InspectorButton {
                                label: "Open in browser"
                                onClicked: CloudProjectsController.openInBrowser(modelData.id)
                            }
                            InspectorButton {
                                label: "Delete"
                                onClicked: {
                                    dialog.pendingDeleteId = modelData.id
                                    dialog.pendingDeleteName = modelData.name
                                                          || modelData.projectSlug
                                                          || modelData.id
                                    confirmDelete.visible = true
                                }
                            }
                        }
                    }

                    footer: Item {
                        width: projectList.width
                        height: CloudProjectsController.hasMore ? 40 : 0
                        InspectorButton {
                            anchors.centerIn: parent
                            visible: CloudProjectsController.hasMore
                            label: CloudProjectsController.loadingMore
                                   ? "Loading…" : "Load more"
                            buttonEnabled: !CloudProjectsController.loadingMore
                            onClicked: CloudProjectsController.loadMore()
                        }
                    }
                }
            }

            ColumnLayout {
                anchors.centerIn: parent
                spacing: 12
                visible: !CloudProjectsController.loading
                         && CloudProjectsController.projects.length === 0
                         && !CloudProjectsController.listError

                InspectorLabel {
                    text: "You haven't uploaded any projects yet."
                    horizontalAlignment: Text.AlignHCenter
                    font.pixelSize: 13
                }
                InspectorButton {
                    label: "Upload to QtMesh Cloud…"
                    Layout.alignment: Qt.AlignHCenter
                    onClicked: CloudProjectsController.requestUpload()
                }
            }

            InspectorLabel {
                anchors.centerIn: parent
                visible: CloudProjectsController.loading
                         && CloudProjectsController.projects.length === 0
                text: "Loading cloud projects…"
            }

            InspectorLabel {
                anchors.centerIn: parent
                anchors.margins: 16
                width: parent.width - 32
                visible: !!CloudProjectsController.listError
                         && CloudProjectsController.projects.length === 0
                text: CloudProjectsController.listError
                color: "#e07070"
                horizontalAlignment: Text.AlignHCenter
            }
        }

        InspectorLabel {
            Layout.fillWidth: true
            visible: dialog.statusMessage.length > 0
            text: dialog.statusMessage
            color: dialog.statusIsError ? "#e07070" : PropertiesPanelController.textColor
        }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            InspectorButton {
                label: "Close"
                onClicked: dialog.close()
            }
        }
    }

    Rectangle {
        id: confirmDelete
        visible: false
        anchors.fill: parent
        color: "#aa000000"
        z: 10

        Rectangle {
            anchors.centerIn: parent
            width: Math.min(parent.width - 48, 420)
            color: PropertiesPanelController.panelColor
            border.color: PropertiesPanelController.borderColor
            radius: 6

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 12

                InspectorLabel {
                    text: "Delete this cloud project?"
                    font.bold: true
                    font.pixelSize: 13
                    Layout.fillWidth: true
                }
                InspectorLabel {
                    text: "“" + dialog.pendingDeleteName + "” will be removed from QtMesh Cloud."
                    Layout.fillWidth: true
                }
                RowLayout {
                    Layout.fillWidth: true
                    Item { Layout.fillWidth: true }
                    InspectorButton {
                        label: "Cancel"
                        onClicked: confirmDelete.visible = false
                    }
                    InspectorButton {
                        label: "Delete"
                        onClicked: {
                            CloudProjectsController.deleteProject(dialog.pendingDeleteId)
                            confirmDelete.visible = false
                        }
                    }
                }
            }
        }
    }
}
