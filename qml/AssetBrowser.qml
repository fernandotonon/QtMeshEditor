import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import AssetBrowser 1.0
import PropertiesPanel 1.0

Rectangle {
    id: root
    color: PropertiesPanelController.panelColor

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---- Path bar ----
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 36
            color: PropertiesPanelController.headerColor

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 6
                anchors.rightMargin: 6
                spacing: 4

                // Up button
                Rectangle {
                    width: 28
                    height: 24
                    radius: 3
                    color: upMouse.containsMouse ? Qt.lighter(PropertiesPanelController.inputColor, 1.3)
                                                : PropertiesPanelController.inputColor
                    border.color: PropertiesPanelController.borderColor
                    border.width: 1

                    Text {
                        anchors.centerIn: parent
                        text: "\u2191"
                        color: PropertiesPanelController.textColor
                        font.pixelSize: 14
                    }

                    MouseArea {
                        id: upMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: AssetBrowserController.navigateUp()
                    }
                }

                // Path display
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 24
                    radius: 3
                    color: PropertiesPanelController.inputColor
                    border.color: PropertiesPanelController.borderColor
                    border.width: 1
                    clip: true

                    Text {
                        anchors.fill: parent
                        anchors.leftMargin: 6
                        anchors.rightMargin: 6
                        verticalAlignment: Text.AlignVCenter
                        text: AssetBrowserController.rootPath
                        color: PropertiesPanelController.textColor
                        font.pixelSize: 11
                        elide: Text.ElideLeft
                    }
                }

                // Browse button
                Rectangle {
                    width: 60
                    height: 24
                    radius: 3
                    color: browseMouse.containsMouse ? Qt.lighter(PropertiesPanelController.inputColor, 1.3)
                                                    : PropertiesPanelController.inputColor
                    border.color: PropertiesPanelController.borderColor
                    border.width: 1

                    Text {
                        anchors.centerIn: parent
                        text: "Browse..."
                        color: PropertiesPanelController.textColor
                        font.pixelSize: 11
                    }

                    MouseArea {
                        id: browseMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: AssetBrowserController.browseForDirectory()
                    }
                }
            }
        }

        // ---- Filter bar ----
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 32
            color: Qt.darker(PropertiesPanelController.headerColor, 1.05)

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 6
                anchors.rightMargin: 6
                spacing: 4

                Repeater {
                    model: [
                        { label: "All",       value: "all" },
                        { label: "Meshes",    value: "meshes" },
                        { label: "Textures",  value: "textures" },
                        { label: "Materials", value: "materials" }
                    ]

                    Rectangle {
                        required property var modelData
                        width: filterText.implicitWidth + 16
                        height: 22
                        radius: 11
                        color: AssetBrowserController.filter === modelData.value
                               ? PropertiesPanelController.highlightColor
                               : (filterMouse.containsMouse ? Qt.lighter(PropertiesPanelController.inputColor, 1.2)
                                                            : "transparent")
                        border.color: AssetBrowserController.filter === modelData.value
                                      ? "transparent"
                                      : PropertiesPanelController.borderColor
                        border.width: 1

                        Text {
                            id: filterText
                            anchors.centerIn: parent
                            text: modelData.label
                            color: AssetBrowserController.filter === modelData.value
                                   ? "#ffffff"
                                   : PropertiesPanelController.textColor
                            font.pixelSize: 11
                            font.bold: AssetBrowserController.filter === modelData.value
                        }

                        MouseArea {
                            id: filterMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: AssetBrowserController.filter = modelData.value
                        }
                    }
                }

                Item { Layout.fillWidth: true }

                // Search field
                Rectangle {
                    Layout.preferredWidth: 140
                    Layout.preferredHeight: 22
                    radius: 3
                    color: PropertiesPanelController.inputColor
                    border.color: searchInput.activeFocus
                                  ? PropertiesPanelController.highlightColor
                                  : PropertiesPanelController.borderColor
                    border.width: 1

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 4
                        anchors.rightMargin: 4
                        spacing: 2

                        Text {
                            text: "\uD83D\uDD0D"
                            font.pixelSize: 10
                            color: PropertiesPanelController.textColor
                            opacity: 0.5
                        }

                        TextInput {
                            id: searchInput
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            verticalAlignment: TextInput.AlignVCenter
                            color: PropertiesPanelController.textColor
                            font.pixelSize: 11
                            clip: true
                            selectByMouse: true
                            onTextChanged: AssetBrowserController.searchQuery = text

                            Text {
                                anchors.fill: parent
                                verticalAlignment: Text.AlignVCenter
                                text: "Search..."
                                color: PropertiesPanelController.textColor
                                opacity: 0.4
                                font.pixelSize: 11
                                visible: !searchInput.text && !searchInput.activeFocus
                            }
                        }
                    }
                }
            }
        }

        // ---- File list ----
        ListView {
            id: fileList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: AssetBrowserController.files

            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AsNeeded
            }

            delegate: Rectangle {
                required property var modelData
                required property int index
                width: fileList.width
                height: 30
                color: delegateMouse.containsMouse
                       ? Qt.lighter(PropertiesPanelController.panelColor, 1.15)
                       : (index % 2 === 0 ? PropertiesPanelController.panelColor
                                          : Qt.darker(PropertiesPanelController.panelColor, 1.03))

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    spacing: 8

                    // Type icon / thumbnail
                    Item {
                        Layout.preferredWidth: 28
                        Layout.preferredHeight: 28

                        // Image thumbnail for textures
                        Image {
                            anchors.fill: parent
                            visible: modelData.type === "texture"
                            source: modelData.type === "texture" ? "file:///" + modelData.path : ""
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            sourceSize.width: 28
                            sourceSize.height: 28
                        }

                        // Emoji icon for non-texture files
                        Text {
                            anchors.centerIn: parent
                            visible: modelData.type !== "texture"
                            text: {
                                if (modelData.isDir) return "\uD83D\uDCC1"
                                if (modelData.type === "mesh") return "\uD83D\uDFE6"
                                if (modelData.type === "material") return "\uD83D\uDD35"
                                return "\uD83D\uDCC4"
                            }
                            font.pixelSize: 14
                        }
                    }

                    // File name
                    Text {
                        Layout.fillWidth: true
                        text: modelData.name
                        color: PropertiesPanelController.textColor
                        font.pixelSize: 12
                        font.bold: modelData.isDir
                        elide: Text.ElideRight
                    }

                    // File size (skip for directories)
                    Text {
                        visible: !modelData.isDir
                        text: {
                            var size = modelData.size
                            if (size < 1024) return size + " B"
                            if (size < 1048576) return (size / 1024).toFixed(1) + " KB"
                            return (size / 1048576).toFixed(1) + " MB"
                        }
                        color: PropertiesPanelController.textColor
                        opacity: 0.6
                        font.pixelSize: 10
                    }
                }

                MouseArea {
                    id: delegateMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: {
                        // Single click navigates into directories
                        if (modelData.isDir)
                            AssetBrowserController.navigateToDirectory(modelData.path)
                    }
                    onDoubleClicked: {
                        // Double click opens/loads files
                        if (!modelData.isDir)
                            AssetBrowserController.openFile(modelData.path)
                    }
                }
            }

            // Empty state
            Text {
                anchors.centerIn: parent
                visible: fileList.count === 0
                text: AssetBrowserController.filter !== "all"
                      ? "No " + AssetBrowserController.filter + " files found"
                      : "No files found"
                color: PropertiesPanelController.textColor
                opacity: 0.5
                font.pixelSize: 13
            }
        }

        // ---- Status bar ----
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 22
            color: PropertiesPanelController.headerColor

            Text {
                anchors.fill: parent
                anchors.leftMargin: 8
                verticalAlignment: Text.AlignVCenter
                text: {
                    var count = AssetBrowserController.files.length
                    var dirs = 0
                    var filesCount = 0
                    for (var i = 0; i < count; i++) {
                        if (AssetBrowserController.files[i].isDir) dirs++
                        else filesCount++
                    }
                    var parts = []
                    if (dirs > 0) parts.push(dirs + (dirs === 1 ? " folder" : " folders"))
                    if (filesCount > 0) parts.push(filesCount + (filesCount === 1 ? " file" : " files"))
                    return parts.join(", ") || "Empty"
                }
                color: PropertiesPanelController.textColor
                opacity: 0.7
                font.pixelSize: 10
            }
        }
    }
}
