import QtQuick 6.0
import QtQuick.Controls 6.0
import QtQuick.Layouts 6.0
import QtQuick.Dialogs
import MaterialEditorQML 1.0
import "." as Local

Dialog {
    id: aiSettingsDialog
    title: "AI Model Settings"
    modal: true
    width: 600
    height: 500

    // Theme colors from parent or system
    property color backgroundColor: palette.window
    property color panelColor: palette.base
    property color textColor: palette.windowText
    property color borderColor: palette.mid
    property color highlightColor: palette.highlight
    property color buttonColor: palette.button
    property color buttonTextColor: palette.buttonText

    SystemPalette {
        id: palette
        colorGroup: SystemPalette.Active
    }

    background: Rectangle {
        color: backgroundColor
        border.color: borderColor
        border.width: 1
        radius: 8
    }

    header: Rectangle {
        height: 50
        color: panelColor
        radius: 8

        Text {
            text: "AI Model Settings"
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

    contentItem: ColumnLayout {
        spacing: 0

        TabBar {
            id: tabBar
            Layout.fillWidth: true

            TabButton {
                text: "Models"
                width: implicitWidth
            }
            TabButton {
                text: "Download"
                width: implicitWidth
            }
            TabButton {
                text: "Settings"
                width: implicitWidth
            }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: tabBar.currentIndex

            // Models Tab
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 15
                    spacing: 15

                    // Current Status
                    Rectangle {
                        Layout.fillWidth: true
                        height: 60
                        color: LLMManager.modelLoaded ? "#e8f5e9" : "#fff3e0"
                        border.color: LLMManager.modelLoaded ? "#4caf50" : "#ff9800"
                        border.width: 1
                        radius: 4

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 10
                            spacing: 10

                            Rectangle {
                                width: 12
                                height: 12
                                radius: 6
                                color: LLMManager.modelLoaded ? "#4caf50" :
                                       LLMManager.isLoading ? "#ff9800" : "#9e9e9e"

                                SequentialAnimation on opacity {
                                    running: LLMManager.isLoading
                                    loops: Animation.Infinite
                                    NumberAnimation { to: 0.3; duration: 500 }
                                    NumberAnimation { to: 1.0; duration: 500 }
                                }
                            }

                            Text {
                                Layout.fillWidth: true
                                text: LLMManager.isLoading ? "Loading model..." :
                                      LLMManager.modelLoaded ? "Model loaded: " + LLMManager.currentModelName :
                                      "No model loaded"
                                color: textColor
                                font.pointSize: 11
                            }

                            Local.ThemedButton {
                                text: "Unload"
                                visible: LLMManager.modelLoaded && !LLMManager.isLoading
                                onClicked: LLMManager.unloadModel()
                            }
                        }
                    }

                    // Model Selection
                    GroupBox {
                        Layout.fillWidth: true
                        title: "Available Models"

                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 10

                            ComboBox {
                                id: modelCombo
                                Layout.fillWidth: true
                                model: LLMManager.availableModels
                                enabled: !LLMManager.isLoading && LLMManager.availableModels.length > 0

                                displayText: LLMManager.availableModels.length > 0 ?
                                            currentText : "No models available - download one first"
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 10

                                Local.ThemedButton {
                                    text: LLMManager.isLoading ? "Loading..." : "Load Model"
                                    enabled: !LLMManager.isLoading && modelCombo.currentIndex >= 0 &&
                                             LLMManager.availableModels.length > 0
                                    onClicked: LLMManager.loadModel(modelCombo.currentText)
                                }

                                Local.ThemedButton {
                                    text: "Refresh"
                                    enabled: !LLMManager.isLoading
                                    onClicked: LLMManager.scanForModels()
                                }

                                Item { Layout.fillWidth: true }

                                Local.ThemedButton {
                                    text: "Open Models Folder"
                                    onClicked: Qt.openUrlExternally("file://" + LLMManager.modelsDirectory)
                                }
                            }
                        }
                    }

                    Item { Layout.fillHeight: true }
                }
            }

            // Download Tab
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 15
                    spacing: 15

                    Text {
                        text: "Recommended Models"
                        font.pointSize: 12
                        font.bold: true
                        color: textColor
                    }

                    ListView {
                        id: modelListView
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: LLMManager.recommendedModels

                        delegate: Rectangle {
                            width: modelListView.width
                            height: 70
                            color: modelListView.currentIndex === index ? highlightColor : "transparent"
                            border.color: borderColor
                            border.width: modelListView.currentIndex === index ? 1 : 0
                            radius: 4

                            MouseArea {
                                anchors.fill: parent
                                onClicked: modelListView.currentIndex = index
                            }

                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 10
                                spacing: 10

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2

                                    Text {
                                        text: modelData.name
                                        font.pointSize: 11
                                        font.bold: true
                                        color: modelData.isDownloaded ? "#4caf50" : textColor
                                    }

                                    Text {
                                        text: modelData.description
                                        font.pointSize: 9
                                        color: Qt.darker(textColor, 1.3)
                                        elide: Text.ElideRight
                                        Layout.fillWidth: true
                                    }

                                    Text {
                                        text: formatSize(modelData.size) + (modelData.isDownloaded ? " [Downloaded]" : "")
                                        font.pointSize: 9
                                        color: modelData.isDownloaded ? "#4caf50" : Qt.darker(textColor, 1.5)
                                    }
                                }
                            }
                        }

                        ScrollBar.vertical: ScrollBar { active: true }
                    }

                    // Download Progress
                    Rectangle {
                        Layout.fillWidth: true
                        height: 80
                        color: panelColor
                        border.color: borderColor
                        border.width: 1
                        radius: 4
                        visible: ModelDownloader.isDownloading || downloadButton.enabled

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 10
                            spacing: 8

                            RowLayout {
                                Layout.fillWidth: true

                                Local.ThemedButton {
                                    id: downloadButton
                                    text: ModelDownloader.isDownloading ? "Cancel" : "Download Selected"
                                    enabled: modelListView.currentIndex >= 0

                                    onClicked: {
                                        if (ModelDownloader.isDownloading) {
                                            ModelDownloader.cancelDownload()
                                        } else {
                                            var model = LLMManager.recommendedModels[modelListView.currentIndex]
                                            var destPath = LLMManager.modelsDirectory + "/" + model.fileName
                                            ModelDownloader.startDownload(model.url, destPath, model.name)
                                        }
                                    }
                                }

                                Item { Layout.fillWidth: true }

                                Text {
                                    text: ModelDownloader.isDownloading ?
                                          formatSize(ModelDownloader.bytesReceived) + " / " +
                                          formatSize(ModelDownloader.bytesTotal) + " (" +
                                          formatSize(ModelDownloader.downloadSpeed) + "/s)" : ""
                                    font.pointSize: 9
                                    color: textColor
                                }
                            }

                            ProgressBar {
                                Layout.fillWidth: true
                                from: 0
                                to: 1
                                value: ModelDownloader.downloadProgress
                                visible: ModelDownloader.isDownloading
                            }
                        }
                    }
                }
            }

            // Settings Tab
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 15
                    spacing: 15

                    GroupBox {
                        Layout.fillWidth: true
                        title: "Inference Settings"

                        GridLayout {
                            anchors.fill: parent
                            columns: 2
                            columnSpacing: 15
                            rowSpacing: 10

                            Text { text: "Context Size:"; color: textColor }
                            SpinBox {
                                from: 512
                                to: 32768
                                stepSize: 512
                                value: LLMManager.contextSize
                                onValueModified: LLMManager.contextSize = value
                                editable: true
                            }

                            Text { text: "Max Output Tokens:"; color: textColor }
                            SpinBox {
                                from: 64
                                to: 8192
                                stepSize: 64
                                value: LLMManager.maxTokens
                                onValueModified: LLMManager.maxTokens = value
                                editable: true
                            }

                            Text { text: "Temperature:"; color: textColor }
                            RowLayout {
                                Slider {
                                    id: tempSlider
                                    from: 0
                                    to: 2
                                    stepSize: 0.1
                                    value: LLMManager.temperature
                                    onMoved: LLMManager.temperature = value
                                    Layout.fillWidth: true
                                }
                                Text {
                                    text: tempSlider.value.toFixed(1)
                                    color: textColor
                                    Layout.minimumWidth: 30
                                }
                            }

                            Text { text: "GPU Layers:"; color: textColor }
                            SpinBox {
                                from: 0
                                to: 999
                                value: LLMManager.gpuLayers
                                onValueModified: LLMManager.gpuLayers = value
                                editable: true

                                textFromValue: function(value) {
                                    return value === 0 ? "CPU only" : value.toString()
                                }
                            }
                        }
                    }

                    GroupBox {
                        Layout.fillWidth: true
                        title: "Models Directory"

                        RowLayout {
                            anchors.fill: parent
                            spacing: 10

                            TextField {
                                Layout.fillWidth: true
                                text: LLMManager.modelsDirectory
                                readOnly: true
                            }

                            Local.ThemedButton {
                                text: "Browse..."
                                onClicked: folderDialog.open()
                            }
                        }
                    }

                    GroupBox {
                        Layout.fillWidth: true
                        title: "Startup"

                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 10

                            CheckBox {
                                id: autoLoadCheckBox
                                text: "Auto-load last model on startup"
                                checked: LLMManager.autoLoadModel
                                onCheckedChanged: LLMManager.autoLoadModel = checked

                                contentItem: Text {
                                    text: autoLoadCheckBox.text
                                    color: textColor
                                    leftPadding: autoLoadCheckBox.indicator.width + autoLoadCheckBox.spacing
                                    verticalAlignment: Text.AlignVCenter
                                }
                            }

                            Text {
                                visible: LLMManager.lastModelName !== ""
                                text: "Last model: " + LLMManager.lastModelName
                                font.pointSize: 9
                                color: Qt.darker(textColor, 1.3)
                            }
                        }
                    }

                    Item { Layout.fillHeight: true }

                    Text {
                        Layout.fillWidth: true
                        text: "Note: Settings changes will take effect when loading a new model."
                        font.pointSize: 9
                        font.italic: true
                        color: Qt.darker(textColor, 1.5)
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }
    }

    footer: Rectangle {
        height: 50
        color: panelColor
        radius: 8

        Rectangle {
            anchors.top: parent.top
            width: parent.width
            height: 1
            color: borderColor
        }

        RowLayout {
            anchors.fill: parent
            anchors.margins: 10

            Item { Layout.fillWidth: true }

            Local.ThemedButton {
                text: "Close"
                onClicked: aiSettingsDialog.accept()
            }
        }
    }

    // Folder dialog for models directory
    FolderDialog {
        id: folderDialog
        title: "Select Models Directory"
        currentFolder: "file://" + LLMManager.modelsDirectory
        onAccepted: {
            var path = selectedFolder.toString().replace("file://", "")
            LLMManager.modelsDirectory = path
        }
    }

    // Connections for download completion
    Connections {
        target: ModelDownloader
        function onDownloadCompleted(modelName, filePath) {
            LLMManager.scanForModels()
        }
    }

    // Helper function
    function formatSize(bytes) {
        if (bytes < 1024) return bytes + " B"
        if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + " KB"
        if (bytes < 1024 * 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(1) + " MB"
        return (bytes / (1024 * 1024 * 1024)).toFixed(2) + " GB"
    }
}
