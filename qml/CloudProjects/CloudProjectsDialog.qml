import QtQuick
import QtQuick.Layouts
import QtQuick.Window
import MaterialEditorQML 1.0
import PropertiesPanel 1.0
import CloudProjects 1.0

// Issue #691: paginated My Cloud Projects list with per-file open.
Window {
    id: dialog
    title: CloudProjectsController.viewingProjectFiles
           ? ("Project files — " + CloudProjectsController.activeProjectName)
           : "My Cloud Projects"
    width: 780
    height: 520
    minimumWidth: 700
    minimumHeight: 420
    flags: Qt.Dialog
    modality: Qt.ApplicationModal
    color: PropertiesPanelController.panelColor

    property string pendingDeleteId: ""
    property string pendingDeleteName: ""
    property string statusMessage: ""
    property bool statusIsError: false

    function open(ownerSlug, projectSlug) {
        dialog.statusMessage = ""
        dialog.statusIsError = false
        dialog.show()
        dialog.raise()
        dialog.requestActivate()
        if (ownerSlug && projectSlug)
            CloudProjectsController.browseProjectBySlug(ownerSlug, projectSlug)
        else
            CloudProjectsController.refresh()
    }

    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_Escape) {
            if (confirmDelete.visible) {
                confirmDelete.visible = false
            } else if (CloudProjectsController.viewingProjectFiles) {
                CloudProjectsController.closeProjectFiles()
            } else {
                dialog.close()
            }
            event.accepted = true
        }
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
        function onCloudOpenFailed(error) {
            dialog.statusMessage = error
            dialog.statusIsError = true
        }
    }

    component InspectorButton: Rectangle {
        id: btn
        property string label: ""
        property bool buttonEnabled: true
        signal clicked()
        activeFocusOnTab: buttonEnabled
        implicitWidth: labelText.implicitWidth + 16
        implicitHeight: 26
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
            id: labelText
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
            visible: !CloudProjectsController.viewingProjectFiles
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

        RowLayout {
            Layout.fillWidth: true
            visible: CloudProjectsController.viewingProjectFiles
            InspectorButton {
                label: "← Back"
                onClicked: CloudProjectsController.closeProjectFiles()
            }
            InspectorLabel {
                text: CloudProjectsController.activeProjectName
                font.pixelSize: 14
                font.bold: true
                Layout.fillWidth: true
                elide: Text.ElideRight
            }
            InspectorButton {
                label: "Refresh"
                buttonEnabled: !CloudProjectsController.loadingProjectFiles
                onClicked: {
                    if (CloudProjectsController.activeProjectId.length > 0)
                        CloudProjectsController.browseProjectFiles(
                            CloudProjectsController.activeProjectId)
                    else
                        CloudProjectsController.browseProjectBySlug(
                            CloudProjectsController.activeOwnerSlug,
                            CloudProjectsController.activeProjectSlug)
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: PropertiesPanelController.headerColor
            border.color: PropertiesPanelController.borderColor
            radius: 4

            // Project list
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 8
                spacing: 8
                visible: !CloudProjectsController.viewingProjectFiles
                         && CloudProjectsController.projects.length > 0

                ListView {
                    id: projectList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 6
                    model: CloudProjectsController.projects

                    delegate: Rectangle {
                        id: projectRow
                        required property var modelData
                        required property int index

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
                                          projectRow.modelData.sourceFormat || "")
                                font.pixelSize: 22
                                Layout.alignment: Qt.AlignVCenter
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                InspectorLabel {
                                    text: projectRow.modelData.name
                                            || projectRow.modelData.projectSlug
                                            || projectRow.modelData.id
                                    font.bold: true
                                    font.pixelSize: 12
                                    Layout.fillWidth: true
                                }
                                InspectorLabel {
                                    text: CloudProjectsController.formatProjectSubtitle(
                                              projectRow.modelData)
                                    color: "#9a9a9a"
                                    Layout.fillWidth: true
                                }
                                InspectorLabel {
                                    visible: !!(projectRow.modelData.mainFile)
                                    text: projectRow.modelData.mainFile
                                    color: "#9a9a9a"
                                    font.pixelSize: 10
                                    Layout.fillWidth: true
                                    elide: Text.ElideMiddle
                                }
                            }

                            InspectorButton {
                                label: "Open in browser"
                                Layout.preferredWidth: 118
                                onClicked: CloudProjectsController.openInBrowser(
                                               projectRow.modelData.id)
                            }
                            InspectorButton {
                                label: "Open…"
                                Layout.preferredWidth: 72
                                onClicked: CloudProjectsController.browseProjectFiles(
                                               projectRow.modelData.id)
                            }
                            InspectorButton {
                                label: "Delete"
                                Layout.preferredWidth: 64
                                onClicked: {
                                    dialog.pendingDeleteId = projectRow.modelData.id
                                    dialog.pendingDeleteName = projectRow.modelData.name
                                                          || projectRow.modelData.projectSlug
                                                          || projectRow.modelData.id
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

            // Per-project file list
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 8
                spacing: 8
                visible: CloudProjectsController.viewingProjectFiles
                         && CloudProjectsController.projectFiles.length > 0

                InspectorLabel {
                    text: "Choose a file to open in the editor. Textures and materials are downloaded automatically when needed."
                    color: "#9a9a9a"
                    Layout.fillWidth: true
                }

                ListView {
                    id: fileList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 6
                    model: CloudProjectsController.projectFiles

                    delegate: Rectangle {
                        id: fileRow
                        required property var modelData
                        required property int index

                        width: fileList.width
                        height: 58
                        radius: 4
                        color: PropertiesPanelController.panelColor
                        border.color: PropertiesPanelController.borderColor

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 10
                            spacing: 10

                            Text {
                                text: CloudProjectsController.canOpenFile(fileRow.modelData)
                                      ? "📄" : "📎"
                                font.pixelSize: 18
                                Layout.alignment: Qt.AlignVCenter
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                InspectorLabel {
                                    text: fileRow.modelData.originalName
                                            || fileRow.modelData.name
                                            || fileRow.modelData.id
                                    font.bold: true
                                    font.pixelSize: 12
                                    Layout.fillWidth: true
                                    elide: Text.ElideMiddle
                                }
                                InspectorLabel {
                                    text: CloudProjectsController.formatFileSubtitle(
                                              fileRow.modelData)
                                    color: "#9a9a9a"
                                    Layout.fillWidth: true
                                }
                            }

                            InspectorButton {
                                label: "Open"
                                Layout.preferredWidth: 64
                                buttonEnabled: CloudProjectsController.canOpenFile(
                                                   fileRow.modelData)
                                onClicked: CloudProjectsController.openProjectFile(
                                               fileRow.modelData.id)
                            }
                        }
                    }
                }
            }

            ColumnLayout {
                anchors.centerIn: parent
                spacing: 12
                visible: !CloudProjectsController.viewingProjectFiles
                         && !CloudProjectsController.loading
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
                visible: !CloudProjectsController.viewingProjectFiles
                         && CloudProjectsController.loading
                         && CloudProjectsController.projects.length === 0
                text: "Loading cloud projects…"
            }

            InspectorLabel {
                anchors.centerIn: parent
                visible: CloudProjectsController.viewingProjectFiles
                         && CloudProjectsController.loadingProjectFiles
                         && CloudProjectsController.projectFiles.length === 0
                text: "Loading project files…"
            }

            InspectorLabel {
                anchors.centerIn: parent
                anchors.margins: 16
                width: parent.width - 32
                visible: !CloudProjectsController.viewingProjectFiles
                         && !!CloudProjectsController.listError
                         && CloudProjectsController.projects.length === 0
                text: CloudProjectsController.listError
                color: "#e07070"
                horizontalAlignment: Text.AlignHCenter
            }

            InspectorLabel {
                anchors.centerIn: parent
                anchors.margins: 16
                width: parent.width - 32
                visible: CloudProjectsController.viewingProjectFiles
                         && !CloudProjectsController.loadingProjectFiles
                         && CloudProjectsController.projectFiles.length === 0
                text: "This project has no uploaded files yet."
                color: "#9a9a9a"
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

        onVisibleChanged: {
            if (visible)
                confirmCancelButton.forceActiveFocus()
        }

        Rectangle {
            anchors.centerIn: parent
            width: Math.min(parent.width - 48, 420)
            height: confirmContent.implicitHeight + 32
            color: PropertiesPanelController.panelColor
            border.color: PropertiesPanelController.borderColor
            radius: 6

            ColumnLayout {
                id: confirmContent
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
                        id: confirmCancelButton
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
