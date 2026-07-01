import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MaterialEditorQML 1.0
import PropertiesPanel 1.0
import "." as Local

Dialog {
    id: aiSettingsDialog
    title: "AI Model Settings"
    modal: true
    width: 680
    height: 620

    property int tabMargin: 20

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
            TabButton {
                text: "SD Models"
                visible: MaterialEditorQML.stableDiffusionEnabled
                width: visible ? implicitWidth : 0
            }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: tabBar.currentIndex

            // ============ Models Tab ============
            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true

                Flickable {
                    contentWidth: parent.width
                    contentHeight: modelsCol.implicitHeight

                    ColumnLayout {
                        id: modelsCol
                        width: parent.width - tabMargin * 2
                        anchors.horizontalCenter: parent.horizontalCenter
                        spacing: 15

                        Item { Layout.preferredHeight: 10 }

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
                                    width: 12; height: 12; radius: 6
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
                                        text: "Load from file…"
                                        enabled: !LLMManager.isLoading
                                        onClicked: LLMManager.browseForModelFile()
                                    }
                                    Local.ThemedButton {
                                        text: "Refresh"
                                        enabled: !LLMManager.isLoading
                                        onClicked: LLMManager.scanForModels()
                                    }
                                    Item { Layout.fillWidth: true }
                                    Local.ThemedButton {
                                        text: "Open Models Folder"
                                        onClicked: Qt.openUrlExternally(LLMManager.modelsDirectoryUrl)
                                    }
                                }
                            }
                        }

                        Item { Layout.preferredHeight: 10 }
                    }
                }
            }

            // ============ Download Tab ============
            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true

                Flickable {
                    contentWidth: parent.width
                    contentHeight: downloadCol.implicitHeight

                    ColumnLayout {
                        id: downloadCol
                        width: parent.width - tabMargin * 2
                        anchors.horizontalCenter: parent.horizontalCenter
                        spacing: 12

                        Item { Layout.preferredHeight: 8 }

                        Text {
                            text: "Recommended Models"
                            font.pointSize: 12
                            font.bold: true
                            color: textColor
                        }

                        ListView {
                            id: modelListView
                            Layout.fillWidth: true
                            Layout.preferredHeight: Math.min(contentHeight, 350)
                            clip: true
                            model: LLMManager.recommendedModels

                            delegate: Rectangle {
                                width: modelListView.width
                                height: 80
                                color: modelListView.currentIndex === index ? highlightColor : (index % 2 === 0 ? "transparent" : Qt.rgba(0,0,0,0.03))
                                border.color: modelListView.currentIndex === index ? borderColor : "transparent"
                                border.width: modelListView.currentIndex === index ? 1 : 0
                                radius: 4

                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: modelListView.currentIndex = index
                                }

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.margins: 12
                                    spacing: 12

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 4

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
                                            wrapMode: Text.WordWrap
                                            Layout.fillWidth: true
                                        }
                                        Text {
                                            text: formatSize(modelData.size) + (modelData.isDownloaded ? "  [Downloaded]" : "")
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
                                    from: 0; to: 1
                                    value: ModelDownloader.downloadProgress
                                    visible: ModelDownloader.isDownloading
                                }
                            }
                        }

                        // ── AI-Assist models (image-to-3D, #764) ──────────────
                        // Pre-download the TripoSR encoder/decoder + U²-Net bg
                        // remover so first use is instant. Reuses ModelDownloader's
                        // shared progress bar above. Only shown on an ONNX build.
                        Text {
                            visible: MeshGenController.available
                            text: "AI-Assist Models"
                            font.pointSize: 12
                            font.bold: true
                            color: textColor
                        }
                        Rectangle {
                            visible: MeshGenController.available
                            Layout.fillWidth: true
                            Layout.preferredHeight: gen3dCol.implicitHeight + 20
                            color: panelColor
                            border.color: borderColor
                            border.width: 1
                            radius: 4
                            ColumnLayout {
                                id: gen3dCol
                                anchors.fill: parent
                                anchors.margins: 10
                                spacing: 8

                                Text {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    text: "Image → 3D (TripoSR) + background removal. Pick a size tier and pre-download so first use is instant. Downloads on first use too."
                                    font.pointSize: 9
                                    color: Qt.darker(textColor, 1.3)
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8
                                    ComboBox {
                                        id: gen3dTier
                                        Layout.preferredWidth: 200
                                        model: ["fp32 (best, ~1.7GB)", "fp16 (~840MB)", "int8 (~430MB)"]
                                        currentIndex: 0
                                        enabled: !MeshGenController.busy
                                    }
                                    Item { Layout.fillWidth: true }
                                    Text {
                                        text: MeshGenController.modelsPresent(gen3dTier.currentIndex)
                                              ? "Downloaded" : "Not downloaded"
                                        font.pointSize: 9
                                        color: MeshGenController.modelsPresent(gen3dTier.currentIndex)
                                               ? "#4caf50" : Qt.darker(textColor, 1.5)
                                    }
                                    Local.ThemedButton {
                                        text: "Download"
                                        enabled: !MeshGenController.busy
                                                 && !MeshGenController.modelsPresent(gen3dTier.currentIndex)
                                        onClicked: MeshGenController.downloadModels(gen3dTier.currentIndex)
                                    }
                                }
                            }
                        }

                        Item { Layout.preferredHeight: 10 }
                    }
                }
            }

            // ============ Settings Tab ============
            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true

                Flickable {
                    contentWidth: parent.width
                    contentHeight: settingsCol.implicitHeight

                    ColumnLayout {
                        id: settingsCol
                        width: parent.width - tabMargin * 2
                        anchors.horizontalCenter: parent.horizontalCenter
                        spacing: 15

                        Item { Layout.preferredHeight: 8 }

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
                                    from: 512; to: 32768; stepSize: 512
                                    value: LLMManager.contextSize
                                    onValueModified: LLMManager.contextSize = value
                                    editable: true
                                }

                                Text { text: "Max Output Tokens:"; color: textColor }
                                SpinBox {
                                    from: 64; to: 8192; stepSize: 64
                                    value: LLMManager.maxTokens
                                    onValueModified: LLMManager.maxTokens = value
                                    editable: true
                                }

                                Text { text: "Temperature:"; color: textColor }
                                RowLayout {
                                    Slider {
                                        id: tempSlider
                                        from: 0; to: 2; stepSize: 0.1
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
                                    from: 0; to: 999
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
                                    onClicked: LLMManager.browseForModelsDirectory()
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

                        Text {
                            Layout.fillWidth: true
                            text: "Note: Settings changes will take effect when loading a new model."
                            font.pointSize: 9
                            font.italic: true
                            color: Qt.darker(textColor, 1.5)
                            wrapMode: Text.WordWrap
                        }

                        Item { Layout.preferredHeight: 10 }
                    }
                }
            }

            // ============ SD Models Tab ============
            ScrollView {
                visible: MaterialEditorQML.stableDiffusionEnabled
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true

                Flickable {
                    contentWidth: parent.width
                    contentHeight: sdCol.implicitHeight

                    ColumnLayout {
                        id: sdCol
                        width: parent.width - tabMargin * 2
                        anchors.horizontalCenter: parent.horizontalCenter
                        spacing: 15

                        Item { Layout.preferredHeight: 8 }

                        // SD Model Status
                        Rectangle {
                            Layout.fillWidth: true
                            height: 60
                            color: SDManager.modelLoaded ? "#e8f5e9" : "#fff3e0"
                            border.color: SDManager.modelLoaded ? "#4caf50" : "#ff9800"
                            border.width: 1
                            radius: 4

                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 10
                                spacing: 10

                                Rectangle {
                                    width: 12; height: 12; radius: 6
                                    color: SDManager.modelLoaded ? "#4caf50" :
                                           SDManager.isLoading ? "#ff9800" : "#9e9e9e"

                                    SequentialAnimation on opacity {
                                        running: SDManager.isLoading
                                        loops: Animation.Infinite
                                        NumberAnimation { to: 0.3; duration: 500 }
                                        NumberAnimation { to: 1.0; duration: 500 }
                                    }
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: SDManager.isLoading ? "Loading SD model..." :
                                          SDManager.modelLoaded ? "SD Model loaded: " + SDManager.currentModelName :
                                          "No SD model loaded"
                                    color: textColor
                                    font.pointSize: 11
                                }

                                Local.ThemedButton {
                                    text: "Unload"
                                    visible: SDManager.modelLoaded && !SDManager.isLoading
                                    onClicked: SDManager.unloadModel()
                                }
                            }
                        }

                        // SD Model Selection
                        GroupBox {
                            Layout.fillWidth: true
                            title: "Available SD Models"

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 10

                                ComboBox {
                                    id: sdModelCombo
                                    Layout.fillWidth: true
                                    model: SDManager.availableModels
                                    enabled: !SDManager.isLoading && SDManager.availableModels.length > 0
                                    displayText: SDManager.availableModels.length > 0 ?
                                                currentText : "No SD models available - download one first"
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 10

                                    Local.ThemedButton {
                                        text: SDManager.isLoading ? "Loading..." : "Load Model"
                                        enabled: !SDManager.isLoading && sdModelCombo.currentIndex >= 0 &&
                                                 SDManager.availableModels.length > 0
                                        onClicked: SDManager.loadModel(sdModelCombo.currentText)
                                    }
                                    Local.ThemedButton {
                                        text: "Refresh"
                                        enabled: !SDManager.isLoading
                                        onClicked: SDManager.scanForModels()
                                    }
                                    Item { Layout.fillWidth: true }
                                    Local.ThemedButton {
                                        text: "Open Folder"
                                        onClicked: Qt.openUrlExternally("file://" + SDManager.modelsDirectory)
                                    }
                                }
                            }
                        }

                        // Recommended SD Models
                        Text {
                            text: "Recommended SD Models"
                            font.pointSize: 12
                            font.bold: true
                            color: textColor
                        }

                        ListView {
                            id: sdModelListView
                            Layout.fillWidth: true
                            Layout.preferredHeight: Math.max(contentHeight, 160)
                            clip: true
                            model: SDManager.recommendedModels

                            delegate: Rectangle {
                                width: sdModelListView.width
                                height: 80
                                color: sdModelListView.currentIndex === index ? highlightColor : (index % 2 === 0 ? "transparent" : Qt.rgba(0,0,0,0.03))
                                border.color: sdModelListView.currentIndex === index ? borderColor : "transparent"
                                border.width: sdModelListView.currentIndex === index ? 1 : 0
                                radius: 4

                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: sdModelListView.currentIndex = index
                                }

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.margins: 12
                                    spacing: 12

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 4

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
                                            wrapMode: Text.WordWrap
                                            Layout.fillWidth: true
                                        }
                                        Text {
                                            text: formatSize(modelData.size) + (modelData.isDownloaded ? "  [Downloaded]" : "")
                                            font.pointSize: 9
                                            color: modelData.isDownloaded ? "#4caf50" : Qt.darker(textColor, 1.5)
                                        }
                                    }
                                }
                            }

                            ScrollBar.vertical: ScrollBar { active: true }
                        }

                        // Download button
                        Rectangle {
                            Layout.fillWidth: true
                            height: 80
                            color: panelColor
                            border.color: borderColor
                            border.width: 1
                            radius: 4

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 10
                                spacing: 8

                                RowLayout {
                                    Layout.fillWidth: true

                                    Local.ThemedButton {
                                        id: sdDownloadButton
                                        text: ModelDownloader.isDownloading ? "Cancel" : "Download Selected"
                                        enabled: sdModelListView.currentIndex >= 0
                                        onClicked: {
                                            if (ModelDownloader.isDownloading) {
                                                ModelDownloader.cancelDownload()
                                            } else {
                                                var model = SDManager.recommendedModels[sdModelListView.currentIndex]
                                                var destPath = SDManager.modelsDirectory + "/" + model.fileName
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
                                    from: 0; to: 1
                                    value: ModelDownloader.downloadProgress
                                    visible: ModelDownloader.isDownloading
                                }
                            }
                        }

                        // SD Generation Settings
                        GroupBox {
                            Layout.fillWidth: true
                            title: "Generation Settings"

                            GridLayout {
                                anchors.fill: parent
                                columns: 2
                                columnSpacing: 15
                                rowSpacing: 10

                                Text { text: "Steps:"; color: textColor }
                                SpinBox {
                                    from: 1; to: 100
                                    value: SDManager.steps
                                    onValueModified: SDManager.steps = value
                                    editable: true
                                }

                                Text { text: "CFG Scale:"; color: textColor }
                                RowLayout {
                                    Slider {
                                        id: cfgSlider
                                        from: 1; to: 20; stepSize: 0.5
                                        value: SDManager.cfgScale
                                        onMoved: SDManager.cfgScale = value
                                        Layout.fillWidth: true
                                    }
                                    Text {
                                        text: cfgSlider.value.toFixed(1)
                                        color: textColor
                                        Layout.minimumWidth: 30
                                    }
                                }

                                Text { text: "Negative Prompt:"; color: textColor }
                                TextField {
                                    Layout.fillWidth: true
                                    text: SDManager.negativePrompt
                                    placeholderText: "blurry, low quality..."
                                    onTextEdited: SDManager.negativePrompt = text
                                }
                            }
                        }

                        Item { Layout.preferredHeight: 10 }
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


    // Connections for download completion
    Connections {
        target: ModelDownloader
        function onDownloadCompleted(modelName, filePath) {
            LLMManager.scanForModels()
            if (MaterialEditorQML.stableDiffusionEnabled) {
                SDManager.scanForModels()
            }
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
