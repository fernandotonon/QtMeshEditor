import QtQuick 6.0
import QtQuick.Controls 6.0
import QtQuick.Layouts 6.0
import QtQuick.Dialogs
import MaterialEditorQML 1.0

GroupBox {
    title: "Texture Properties"
    
    Component.onCompleted: {
        console.log("TexturePropertiesPanel: loaded successfully")
    }

    // Enhanced dynamic theme colors based on system palette
    readonly property color backgroundColor: palette.window
    readonly property color panelColor: palette.base
    readonly property color textColor: palette.windowText
    readonly property color borderColor: palette.mid
    readonly property color highlightColor: palette.highlight
    readonly property color buttonColor: palette.button
    readonly property color buttonTextColor: palette.buttonText
    readonly property color disabledTextColor: palette.placeholderText
    
    SystemPalette {
        id: palette
        colorGroup: SystemPalette.Active
    }
    
    ColumnLayout {
        anchors.fill: parent
        spacing: 15
        
        // Texture Selection
        GroupBox {
            title: "Texture Selection"
            Layout.fillWidth: true
            
            ColumnLayout {
                anchors.fill: parent
                spacing: 10
                
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    
                    ThemedLabel {
                        text: "Texture:"
                        Layout.alignment: Qt.AlignVCenter
                    }
                    
                    ThemedTextField {
                        id: textureNameField
                        Layout.fillWidth: true
                        text: MaterialEditorQML.textureName
                        placeholderText: "Select a texture..."
                        readOnly: true
                    }
                    
                    ThemedButton {
                        text: "Browse..."
                        onClicked: textureFileDialog.open()
                    }
                }
                
                // Available textures dropdown
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    
                    ThemedLabel {
                        text: "Available:"
                        Layout.alignment: Qt.AlignVCenter
                    }
                    
                    ThemedComboBox {
                        id: availableTexturesCombo
                        Layout.fillWidth: true
                        model: MaterialEditorQML.getAvailableTextures()
                        displayText: "Select from available textures..."
                        onCurrentTextChanged: {
                            if (currentIndex > 0) { // Skip the placeholder
                                MaterialEditorQML.setTextureName(currentText)
                                textureNameField.text = currentText
                                currentIndex = 0 // Reset to placeholder
                            }
                        }
                    }
                }
            }
        }
        
        // Texture Preview
        GroupBox {
            title: "Preview"
            Layout.fillWidth: true
            Layout.preferredHeight: 200
            
            Rectangle {
                anchors.fill: parent
                anchors.margins: 5
                color: panelColor
                border.color: borderColor
                border.width: 1
                
                Image {
                    id: texturePreview
                    anchors.centerIn: parent
                    width: Math.min(parent.width - 20, sourceSize.width)
                    height: Math.min(parent.height - 20, sourceSize.height)
                    fillMode: Image.PreserveAspectFit
                    source: MaterialEditorQML.getTexturePreviewPath()
                    
                    // Update source when texture name changes
                    Connections {
                        target: MaterialEditorQML
                        function onTextureNameChanged() {
                            texturePreview.source = MaterialEditorQML.getTexturePreviewPath()
                        }
                    }

                    onStatusChanged: {
                        if (status === Image.Error) {
                            // If the constructed path fails, show placeholder
                            texturePreview.visible = false
                            placeholderText.visible = true
                        } else if (status === Image.Ready) {
                            texturePreview.visible = true
                            placeholderText.visible = false
                        }
                    }
                }
                
                Text {
                    id: placeholderText
                    anchors.centerIn: parent
                    text: MaterialEditorQML.textureName === "*Select a texture*" ? 
                          "No texture selected" : 
                          "Texture preview\nnot available"
                    color: disabledTextColor
                    horizontalAlignment: Text.AlignHCenter
                    visible: !texturePreview.visible || texturePreview.status === Image.Error
                }
            }
        }
        
        // Texture Coordinates Group
        GroupBox {
            title: "Texture Coordinates"
            background: Rectangle {
                color: MaterialEditorQML.panelColor
                border.color: MaterialEditorQML.borderColor
                border.width: 1
                radius: 4
            }
            label: ThemedLabel {
                text: parent.title
                font.bold: true
            }
            
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 8
                
                // Texture Coordinate Set
                RowLayout {
                    ThemedLabel { text: "Coord Set:" }
                    ThemedSpinBox {
                        id: texCoordSetSpin
                        Layout.fillWidth: true
                        from: 0
                        to: 7
                        value: MaterialEditorQML.texCoordSet
                        onValueChanged: {
                            if (value !== MaterialEditorQML.texCoordSet) {
                                MaterialEditorQML.texCoordSet = value
                            }
                        }
                        Connections {
                            target: MaterialEditorQML
                            function onTexCoordSetChanged() {
                                texCoordSetSpin.value = MaterialEditorQML.texCoordSet
                            }
                        }
                    }
                }
                
                // Texture Address Mode
                RowLayout {
                    ThemedLabel { text: "Address Mode:" }
                    ThemedComboBox {
                        id: textureAddressModeCombo
                        Layout.fillWidth: true
                        model: MaterialEditorQML.getTextureAddressModeNames()
                        currentIndex: MaterialEditorQML.textureAddressMode
                        onCurrentIndexChanged: {
                            if (currentIndex !== MaterialEditorQML.textureAddressMode) {
                                MaterialEditorQML.textureAddressMode = currentIndex
                            }
                        }
                        Connections {
                            target: MaterialEditorQML
                            function onTextureAddressModeChanged() {
                                textureAddressModeCombo.currentIndex = MaterialEditorQML.textureAddressMode
                            }
                        }
                    }
                }
                
                // Texture Border Color (only shown when using Border address mode)
                RowLayout {
                    visible: MaterialEditorQML.textureAddressMode === 3
                    ThemedLabel { text: "Border Color:" }
                    Rectangle {
                        id: textureBorderColorRect
                        width: 30
                        height: 20
                        color: MaterialEditorQML.textureBorderColor
                        border.color: MaterialEditorQML.borderColor
                        border.width: 1
                        radius: 2
                        
                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                console.log("Texture border color rectangle clicked")
                                colorPickerPopup.openForColor("textureBorder", MaterialEditorQML.textureBorderColor)
                            }
                        }
                    }
                }
            }
        }
        
        // Texture Filtering Group
        GroupBox {
            title: "Texture Filtering"
            background: Rectangle {
                color: MaterialEditorQML.panelColor
                border.color: MaterialEditorQML.borderColor
                border.width: 1
                radius: 4
            }
            label: ThemedLabel {
                text: parent.title
                font.bold: true
            }
            
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 8
                
                // Texture Filtering
                RowLayout {
                    ThemedLabel { text: "Filtering:" }
                    ThemedComboBox {
                        id: textureFilteringCombo
                        Layout.fillWidth: true
                        model: MaterialEditorQML.getTextureFilteringNames()
                        currentIndex: MaterialEditorQML.textureFiltering
                        onCurrentIndexChanged: {
                            if (currentIndex !== MaterialEditorQML.textureFiltering) {
                                MaterialEditorQML.textureFiltering = currentIndex
                            }
                        }
                        Connections {
                            target: MaterialEditorQML
                            function onTextureFilteringChanged() {
                                textureFilteringCombo.currentIndex = MaterialEditorQML.textureFiltering
                            }
                        }
                    }
                }
                
                // Max Anisotropy (only shown when using Anisotropic filtering)
                RowLayout {
                    visible: MaterialEditorQML.textureFiltering === 3
                    ThemedLabel { text: "Max Anisotropy:" }
                    ThemedSpinBox {
                        id: maxAnisotropySpin
                        Layout.fillWidth: true
                        from: 1
                        to: 16
                        value: MaterialEditorQML.maxAnisotropy
                        onValueChanged: {
                            if (value !== MaterialEditorQML.maxAnisotropy) {
                                MaterialEditorQML.maxAnisotropy = value
                            }
                        }
                        Connections {
                            target: MaterialEditorQML
                            function onMaxAnisotropyChanged() {
                                maxAnisotropySpin.value = MaterialEditorQML.maxAnisotropy
                            }
                        }
                    }
                }
            }
        }
        
        // Texture Transform Group
        GroupBox {
            title: "Texture Transform"
            background: Rectangle {
                color: MaterialEditorQML.panelColor
                border.color: MaterialEditorQML.borderColor
                border.width: 1
                radius: 4
            }
            label: ThemedLabel {
                text: parent.title
                font.bold: true
            }
            
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 8
                
                // U Offset
                RowLayout {
                    ThemedLabel { text: "U Offset:" }
                    ThemedSpinBox {
                        id: textureUOffsetSpin
                        Layout.fillWidth: true
                        from: -10000
                        to: 10000
                        stepSize: 1
                        value: Math.round(MaterialEditorQML.textureUOffset * 1000)
                        onValueChanged: {
                            if (value !== Math.round(MaterialEditorQML.textureUOffset * 1000)) {
                                MaterialEditorQML.textureUOffset = value / 1000.0
                            }
                        }
                        Connections {
                            target: MaterialEditorQML
                            function onTextureUOffsetChanged() {
                                textureUOffsetSpin.value = Math.round(MaterialEditorQML.textureUOffset * 1000)
                            }
                        }
                    }
                }
                
                // V Offset
                RowLayout {
                    ThemedLabel { text: "V Offset:" }
                    ThemedSpinBox {
                        id: textureVOffsetSpin
                        Layout.fillWidth: true
                        from: -10000
                        to: 10000
                        stepSize: 1
                        value: Math.round(MaterialEditorQML.textureVOffset * 1000)
                        onValueChanged: {
                            if (value !== Math.round(MaterialEditorQML.textureVOffset * 1000)) {
                                MaterialEditorQML.textureVOffset = value / 1000.0
                            }
                        }
                        Connections {
                            target: MaterialEditorQML
                            function onTextureVOffsetChanged() {
                                textureVOffsetSpin.value = Math.round(MaterialEditorQML.textureVOffset * 1000)
                            }
                        }
                    }
                }
                
                // U Scale
                RowLayout {
                    ThemedLabel { text: "U Scale:" }
                    ThemedSpinBox {
                        id: textureUScaleSpin
                        Layout.fillWidth: true
                        from: -10000
                        to: 10000
                        stepSize: 1
                        value: Math.round(MaterialEditorQML.textureUScale * 1000)
                        onValueChanged: {
                            if (value !== Math.round(MaterialEditorQML.textureUScale * 1000)) {
                                MaterialEditorQML.textureUScale = value / 1000.0
                            }
                        }
                        Connections {
                            target: MaterialEditorQML
                            function onTextureUScaleChanged() {
                                textureUScaleSpin.value = Math.round(MaterialEditorQML.textureUScale * 1000)
                            }
                        }
                    }
                }
                
                // V Scale
                RowLayout {
                    ThemedLabel { text: "V Scale:" }
                    ThemedSpinBox {
                        id: textureVScaleSpin
                        Layout.fillWidth: true
                        from: -10000
                        to: 10000
                        stepSize: 1
                        value: Math.round(MaterialEditorQML.textureVScale * 1000)
                        onValueChanged: {
                            if (value !== Math.round(MaterialEditorQML.textureVScale * 1000)) {
                                MaterialEditorQML.textureVScale = value / 1000.0
                            }
                        }
                        Connections {
                            target: MaterialEditorQML
                            function onTextureVScaleChanged() {
                                textureVScaleSpin.value = Math.round(MaterialEditorQML.textureVScale * 1000)
                            }
                        }
                    }
                }
                
                // Rotation
                RowLayout {
                    ThemedLabel { text: "Rotation (degrees):" }
                    ThemedSpinBox {
                        id: textureRotationSpin
                        Layout.fillWidth: true
                        from: -36000
                        to: 36000
                        stepSize: 1
                        value: Math.round(MaterialEditorQML.textureRotation * 100)
                        onValueChanged: {
                            if (value !== Math.round(MaterialEditorQML.textureRotation * 100)) {
                                MaterialEditorQML.textureRotation = value / 100.0
                            }
                        }
                        Connections {
                            target: MaterialEditorQML
                            function onTextureRotationChanged() {
                                textureRotationSpin.value = Math.round(MaterialEditorQML.textureRotation * 100)
                            }
                        }
                    }
                }
            }
        }
        
        // Environment Mapping Group
        GroupBox {
            title: "Environment Mapping"
            background: Rectangle {
                color: MaterialEditorQML.panelColor
                border.color: MaterialEditorQML.borderColor
                border.width: 1
                radius: 4
            }
            label: ThemedLabel {
                text: parent.title
                font.bold: true
            }
            
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 8
                
                // Environment Mapping
                RowLayout {
                    ThemedLabel { text: "Environment Map:" }
                    ThemedComboBox {
                        id: environmentMappingCombo
                        Layout.fillWidth: true
                        model: MaterialEditorQML.getEnvironmentMappingNames()
                        currentIndex: MaterialEditorQML.environmentMapping
                        onCurrentIndexChanged: {
                            if (currentIndex !== MaterialEditorQML.environmentMapping) {
                                MaterialEditorQML.environmentMapping = currentIndex
                            }
                        }
                        Connections {
                            target: MaterialEditorQML
                            function onEnvironmentMappingChanged() {
                                environmentMappingCombo.currentIndex = MaterialEditorQML.environmentMapping
                            }
                        }
                    }
                }
            }
        }
        
        // Texture Animation Group  
        GroupBox {
            title: "Texture Animation"
            background: Rectangle {
                color: MaterialEditorQML.panelColor
                border.color: MaterialEditorQML.borderColor
                border.width: 1
                radius: 4
            }
            label: ThemedLabel {
                text: parent.title
                font.bold: true
            }
            
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 8
                
                // Scroll Animation U Speed
                RowLayout {
                    ThemedLabel { text: "Scroll U Speed:" }
                    ThemedSpinBox {
                        id: scrollUSpeedSpin
                        Layout.fillWidth: true
                        from: -10000
                        to: 10000
                        stepSize: 1
                        value: Math.round(MaterialEditorQML.scrollAnimUSpeed * 1000)
                        onValueChanged: {
                            if (value !== Math.round(MaterialEditorQML.scrollAnimUSpeed * 1000)) {
                                MaterialEditorQML.scrollAnimUSpeed = value / 1000.0
                            }
                        }
                        Connections {
                            target: MaterialEditorQML
                            function onScrollAnimUSpeedChanged() {
                                scrollUSpeedSpin.value = Math.round(MaterialEditorQML.scrollAnimUSpeed * 1000)
                            }
                        }
                    }
                }
                
                // Scroll Animation V Speed
                RowLayout {
                    ThemedLabel { text: "Scroll V Speed:" }
                    ThemedSpinBox {
                        id: scrollVSpeedSpin
                        Layout.fillWidth: true
                        from: -10000
                        to: 10000
                        stepSize: 1
                        value: Math.round(MaterialEditorQML.scrollAnimVSpeed * 1000)
                        onValueChanged: {
                            if (value !== Math.round(MaterialEditorQML.scrollAnimVSpeed * 1000)) {
                                MaterialEditorQML.scrollAnimVSpeed = value / 1000.0
                            }
                        }
                        Connections {
                            target: MaterialEditorQML
                            function onScrollAnimVSpeedChanged() {
                                scrollVSpeedSpin.value = Math.round(MaterialEditorQML.scrollAnimVSpeed * 1000)
                            }
                        }
                    }
                }
                
                // Rotate Animation Speed
                RowLayout {
                    ThemedLabel { text: "Rotate Speed (deg/sec):" }
                    ThemedSpinBox {
                        id: rotateAnimSpeedSpin
                        Layout.fillWidth: true
                        from: -36000
                        to: 36000
                        stepSize: 1
                        value: Math.round(MaterialEditorQML.rotateAnimSpeed * 100)
                        onValueChanged: {
                            if (value !== Math.round(MaterialEditorQML.rotateAnimSpeed * 100)) {
                                MaterialEditorQML.rotateAnimSpeed = value / 100.0
                            }
                        }
                        Connections {
                            target: MaterialEditorQML
                            function onRotateAnimSpeedChanged() {
                                rotateAnimSpeedSpin.value = Math.round(MaterialEditorQML.rotateAnimSpeed * 100)
                            }
                        }
                    }
                }
            }
        }
        
        // Texture Information
        GroupBox {
            title: "Information"
            Layout.fillWidth: true
            
            ColumnLayout {
                anchors.fill: parent
                spacing: 5
                
                Text {
                    text: "Texture: " + (MaterialEditorQML.textureName || "None")
                    font.pointSize: 10
                    color: textColor
                }
                
                Text {
                    text: texturePreview.source != "" && texturePreview.status === Image.Ready ? 
                          "Size: " + texturePreview.sourceSize.width + " x " + texturePreview.sourceSize.height :
                          "Size: Unknown"
                    font.pointSize: 10
                    color: textColor
                }
                
                Text {
                    text: "Animation: " + 
                          (MaterialEditorQML.scrollAnimUSpeed != 0.0 || MaterialEditorQML.scrollAnimVSpeed != 0.0 ? 
                           "Enabled (" + MaterialEditorQML.scrollAnimUSpeed.toFixed(2) + ", " + MaterialEditorQML.scrollAnimVSpeed.toFixed(2) + ")" : 
                           "Disabled")
                    font.pointSize: 10
                    color: textColor
                }
            }
        }
    }

    // File browser dialog for texture selection
    Dialog {
        id: textureFileDialog
        title: "Select Texture File"
        modal: true
        anchors.centerIn: parent
        width: 700
        height: 500
        
        background: Rectangle {
            color: backgroundColor
            border.color: borderColor
            border.width: 2
            radius: 8
        }
        
        header: Rectangle {
            height: 45
            color: panelColor
            border.color: borderColor
            border.width: 1
            radius: 8
            
            Text {
                text: "Select Texture File"
                font.pointSize: 14
                font.bold: true
                color: textColor
                anchors.centerIn: parent
            }
        }
        
        property string currentPath: "/media/materials/textures"
        property var fileList: []
        
        function refreshFileList() {
            // This would ideally call a C++ function to list real files
            // For now, we'll simulate a file browser with some common texture files
            var simulatedFiles = [
                {name: "..", type: "dir", size: "", path: getParentPath(currentPath)},
                {name: "textures", type: "dir", size: "", path: currentPath + "/textures"},
                {name: "materials", type: "dir", size: "", path: currentPath + "/materials"},
                {name: "concrete01.jpg", type: "file", size: "2.4 MB", path: currentPath + "/concrete01.jpg"},
                {name: "metal_brushed.png", type: "file", size: "1.8 MB", path: currentPath + "/metal_brushed.png"},
                {name: "wood_oak.dds", type: "file", size: "4.2 MB", path: currentPath + "/wood_oak.dds"},
                {name: "brick_red.tga", type: "file", size: "3.1 MB", path: currentPath + "/brick_red.tga"},
                {name: "grass_summer.jpg", type: "file", size: "1.9 MB", path: currentPath + "/grass_summer.jpg"},
                {name: "stone_cobble.png", type: "file", size: "2.7 MB", path: currentPath + "/stone_cobble.png"},
                {name: "water_normal.dds", type: "file", size: "5.5 MB", path: currentPath + "/water_normal.dds"},
                {name: "sand_desert.jpg", type: "file", size: "2.2 MB", path: currentPath + "/sand_desert.jpg"},
                {name: "fabric_canvas.png", type: "file", size: "1.6 MB", path: currentPath + "/fabric_canvas.png"},
                {name: "plastic_white.jpg", type: "file", size: "0.8 MB", path: currentPath + "/plastic_white.jpg"},
                {name: "rubber_black.dds", type: "file", size: "3.8 MB", path: currentPath + "/rubber_black.dds"},
                {name: "glass_clear.png", type: "file", size: "1.2 MB", path: currentPath + "/glass_clear.png"}
            ]
            
            fileListModel.clear()
            for (var i = 0; i < simulatedFiles.length; i++) {
                fileListModel.append(simulatedFiles[i])
            }
        }
        
        function getParentPath(path) {
            var parts = path.split('/')
            if (parts.length > 1) {
                parts.pop()
                return parts.join('/')
            }
            return path
        }
        
        function getFileName(fullPath) {
            return fullPath.split('/').pop()
        }
        
        Component.onCompleted: refreshFileList()
        
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 15
            spacing: 15
            
            // Navigation bar
            RowLayout {
                Layout.fillWidth: true
                
                ThemedLabel {
                    text: "Path:"
                }
                
                ThemedTextField {
                    id: pathField
                    Layout.fillWidth: true
                    text: textureFileDialog.currentPath
                    onTextChanged: {
                        if (text !== textureFileDialog.currentPath) {
                            textureFileDialog.currentPath = text
                        }
                    }
                }
                
                ThemedButton {
                    text: "↑ Up"
                    onClicked: {
                        textureFileDialog.currentPath = textureFileDialog.getParentPath(textureFileDialog.currentPath)
                        pathField.text = textureFileDialog.currentPath
                        textureFileDialog.refreshFileList()
                    }
                }
                
                ThemedButton {
                    text: "🔄 Refresh"
                    onClicked: textureFileDialog.refreshFileList()
                }
            }
            
            // File type filter
            RowLayout {
                Layout.fillWidth: true
                
                ThemedLabel {
                    text: "Filter:"
                }
                
                ThemedComboBox {
                    id: filterCombo
                    model: [
                        "All Image Files (*.jpg *.png *.dds *.tga *.bmp)",
                        "JPEG Files (*.jpg *.jpeg)",
                        "PNG Files (*.png)",
                        "DDS Files (*.dds)",
                        "TGA Files (*.tga)",
                        "All Files (*.*)"
                    ]
                    currentIndex: 0
                }
            }
            
            // File list
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: backgroundColor
                border.color: borderColor
                border.width: 1
                radius: 4
                
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 5
                    spacing: 0
                    
                    // Header row
                    Rectangle {
                        Layout.fillWidth: true
                        height: 30
                        color: alternateColor
                        border.color: borderColor
                        border.width: 1
                        
                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 5
                            spacing: 10
                            
                            Text {
                                text: "Name"
                                font.bold: true
                                color: textColor
                                Layout.preferredWidth: 300
                            }
                            
                            Text {
                                text: "Size"
                                font.bold: true
                                color: textColor
                                Layout.preferredWidth: 80
                            }
                            
                            Text {
                                text: "Type"
                                font.bold: true
                                color: textColor
                                Layout.fillWidth: true
                            }
                        }
                    }
                    
                    // File list view
                    ListView {
                        id: fileListView
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        
                        model: ListModel {
                            id: fileListModel
                        }
                        
                        delegate: ItemDelegate {
                            width: fileListView.width
                            height: 35
                            
                            property bool isDirectory: type === "dir"
                            property bool isImageFile: name.match(/\.(jpg|jpeg|png|dds|tga|bmp)$/i)
                            
                            Rectangle {
                                anchors.fill: parent
                                color: parent.hovered ? highlightColor : 
                                       (index % 2 === 0 ? "transparent" : Qt.darker(backgroundColor, 1.05))
                                radius: 2
                                
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.margins: 5
                                    spacing: 10
                                    
                                    // Icon and name
                                    RowLayout {
                                        Layout.preferredWidth: 300
                                        spacing: 5
                                        
                                        Text {
                                            text: isDirectory ? "📁" : (isImageFile ? "🖼️" : "📄")
                                            font.pointSize: 12
                                        }
                                        
                                        Text {
                                            text: name
                                            color: textColor
                                            font.pointSize: 11
                                            elide: Text.ElideRight
                                            Layout.fillWidth: true
                                        }
                                    }
                                    
                                    // Size
                                    Text {
                                        text: size
                                        color: disabledTextColor
                                        font.pointSize: 10
                                        Layout.preferredWidth: 80
                                    }
                                    
                                    // Type
                                    Text {
                                        text: isDirectory ? "Folder" : "Image File"
                                        color: disabledTextColor
                                        font.pointSize: 10
                                        Layout.fillWidth: true
                                    }
                                }
                            }
                            
                            onClicked: {
                                if (isDirectory) {
                                    // Navigate to directory
                                    if (name === "..") {
                                        textureFileDialog.currentPath = textureFileDialog.getParentPath(textureFileDialog.currentPath)
                                    } else {
                                        textureFileDialog.currentPath = path
                                    }
                                    pathField.text = textureFileDialog.currentPath
                                    textureFileDialog.refreshFileList()
                                } else {
                                    // Select file
                                    selectedFileField.text = name
                                }
                            }
                            
                            onDoubleClicked: {
                                if (!isDirectory) {
                                    // Double-click on file to select and close
                                    MaterialEditorQML.setTextureName(name)
                                    textureNameField.text = name
                                    textureFileDialog.close()
                                }
                            }
                        }
                        
                        ScrollIndicator.vertical: ScrollIndicator {
                            active: true
                        }
                    }
                }
            }
            
            // Selected file
            RowLayout {
                Layout.fillWidth: true
                
                ThemedLabel {
                    text: "Selected file:"
                }
                
                ThemedTextField {
                    id: selectedFileField
                    Layout.fillWidth: true
                    placeholderText: "Select a file from the list above..."
                }
            }
            
            // Buttons
            RowLayout {
                Layout.fillWidth: true
                
                ThemedButton {
                    text: "Create New Folder"
                    onClicked: {
                        // This would create a new folder in a real implementation
                        console.log("Create new folder functionality would go here")
                    }
                }
                
                Item { Layout.fillWidth: true }
                
                ThemedButton {
                    text: "Cancel"
                    onClicked: {
                        selectedFileField.text = ""
                        textureFileDialog.close()
                    }
                }
                
                ThemedButton {
                    text: "Open"
                    enabled: selectedFileField.text.trim() !== ""
                    onClicked: {
                        if (selectedFileField.text.trim() !== "") {
                            MaterialEditorQML.setTextureName(selectedFileField.text.trim())
                            textureNameField.text = selectedFileField.text.trim()
                            selectedFileField.text = ""
                            textureFileDialog.close()
                        }
                    }
                }
            }
        }
    }

    // Texture Border Color Picker Popup
    Popup {
        id: textureBorderColorPicker
        width: 280
        height: 320
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        
        background: Rectangle {
            color: MaterialEditorQML.panelColor
            border.color: MaterialEditorQML.borderColor
            border.width: 1
            radius: 4
        }
        
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 10
            spacing: 10
            
            ThemedLabel {
                text: "Select Border Color"
                font.bold: true
                Layout.alignment: Qt.AlignHCenter
            }
            
            // Color preview
            Rectangle {
                id: borderColorPreview
                Layout.fillWidth: true
                height: 40
                color: MaterialEditorQML.textureBorderColor
                border.color: MaterialEditorQML.borderColor
                border.width: 1
                radius: 4
            }
            
            // RGBA sliders
            GridLayout {
                columns: 2
                Layout.fillWidth: true
                
                ThemedLabel { text: "Red:" }
                Slider {
                    id: borderRedSlider
                    Layout.fillWidth: true
                    from: 0
                    to: 255
                    value: MaterialEditorQML.textureBorderColor.r * 255
                    onValueChanged: {
                        var newColor = Qt.rgba(value/255, MaterialEditorQML.textureBorderColor.g, MaterialEditorQML.textureBorderColor.b, MaterialEditorQML.textureBorderColor.a)
                        MaterialEditorQML.textureBorderColor = newColor
                    }
                    background: Rectangle {
                        x: borderRedSlider.leftPadding
                        y: borderRedSlider.topPadding + borderRedSlider.availableHeight / 2 - height / 2
                        implicitWidth: 200
                        implicitHeight: 4
                        width: borderRedSlider.availableWidth
                        height: implicitHeight
                        radius: 2
                        color: MaterialEditorQML.borderColor
                    }
                    handle: Rectangle {
                        x: borderRedSlider.leftPadding + borderRedSlider.visualPosition * (borderRedSlider.availableWidth - width)
                        y: borderRedSlider.topPadding + borderRedSlider.availableHeight / 2 - height / 2
                        implicitWidth: 20
                        implicitHeight: 20
                        radius: 10
                        color: MaterialEditorQML.accentColor
                        border.color: MaterialEditorQML.borderColor
                        border.width: 1
                    }
                }
                
                ThemedLabel { text: "Green:" }
                Slider {
                    id: borderGreenSlider
                    Layout.fillWidth: true
                    from: 0
                    to: 255
                    value: MaterialEditorQML.textureBorderColor.g * 255
                    onValueChanged: {
                        var newColor = Qt.rgba(MaterialEditorQML.textureBorderColor.r, value/255, MaterialEditorQML.textureBorderColor.b, MaterialEditorQML.textureBorderColor.a)
                        MaterialEditorQML.textureBorderColor = newColor
                    }
                    background: Rectangle {
                        x: borderGreenSlider.leftPadding
                        y: borderGreenSlider.topPadding + borderGreenSlider.availableHeight / 2 - height / 2
                        implicitWidth: 200
                        implicitHeight: 4
                        width: borderGreenSlider.availableWidth
                        height: implicitHeight
                        radius: 2
                        color: MaterialEditorQML.borderColor
                    }
                    handle: Rectangle {
                        x: borderGreenSlider.leftPadding + borderGreenSlider.visualPosition * (borderGreenSlider.availableWidth - width)
                        y: borderGreenSlider.topPadding + borderGreenSlider.availableHeight / 2 - height / 2
                        implicitWidth: 20
                        implicitHeight: 20
                        radius: 10
                        color: MaterialEditorQML.accentColor
                        border.color: MaterialEditorQML.borderColor
                        border.width: 1
                    }
                }
                
                ThemedLabel { text: "Blue:" }
                Slider {
                    id: borderBlueSlider
                    Layout.fillWidth: true
                    from: 0
                    to: 255
                    value: MaterialEditorQML.textureBorderColor.b * 255
                    onValueChanged: {
                        var newColor = Qt.rgba(MaterialEditorQML.textureBorderColor.r, MaterialEditorQML.textureBorderColor.g, value/255, MaterialEditorQML.textureBorderColor.a)
                        MaterialEditorQML.textureBorderColor = newColor
                    }
                    background: Rectangle {
                        x: borderBlueSlider.leftPadding
                        y: borderBlueSlider.topPadding + borderBlueSlider.availableHeight / 2 - height / 2
                        implicitWidth: 200
                        implicitHeight: 4
                        width: borderBlueSlider.availableWidth
                        height: implicitHeight
                        radius: 2
                        color: MaterialEditorQML.borderColor
                    }
                    handle: Rectangle {
                        x: borderBlueSlider.leftPadding + borderBlueSlider.visualPosition * (borderBlueSlider.availableWidth - width)
                        y: borderBlueSlider.topPadding + borderBlueSlider.availableHeight / 2 - height / 2
                        implicitWidth: 20
                        implicitHeight: 20
                        radius: 10
                        color: MaterialEditorQML.accentColor
                        border.color: MaterialEditorQML.borderColor
                        border.width: 1
                    }
                }
                
                ThemedLabel { text: "Alpha:" }
                Slider {
                    id: borderAlphaSlider
                    Layout.fillWidth: true
                    from: 0
                    to: 255
                    value: MaterialEditorQML.textureBorderColor.a * 255
                    onValueChanged: {
                        var newColor = Qt.rgba(MaterialEditorQML.textureBorderColor.r, MaterialEditorQML.textureBorderColor.g, MaterialEditorQML.textureBorderColor.b, value/255)
                        MaterialEditorQML.textureBorderColor = newColor
                    }
                    background: Rectangle {
                        x: borderAlphaSlider.leftPadding
                        y: borderAlphaSlider.topPadding + borderAlphaSlider.availableHeight / 2 - height / 2
                        implicitWidth: 200
                        implicitHeight: 4
                        width: borderAlphaSlider.availableWidth
                        height: implicitHeight
                        radius: 2
                        color: MaterialEditorQML.borderColor
                    }
                    handle: Rectangle {
                        x: borderAlphaSlider.leftPadding + borderAlphaSlider.visualPosition * (borderAlphaSlider.availableWidth - width)
                        y: borderAlphaSlider.topPadding + borderAlphaSlider.availableHeight / 2 - height / 2
                        implicitWidth: 20
                        implicitHeight: 20
                        radius: 10
                        color: MaterialEditorQML.accentColor
                        border.color: MaterialEditorQML.borderColor
                        border.width: 1
                    }
                }
            }
            
            RowLayout {
                Layout.fillWidth: true
                
                ThemedButton {
                    text: "OK"
                    Layout.fillWidth: true
                    onClicked: textureBorderColorPicker.close()
                }
            }
        }
    }

    // Update UI when texture properties change
    Connections {
        target: MaterialEditorQML
        function onTextureNameChanged() {
            textureNameField.text = MaterialEditorQML.textureName
            texturePreview.source = texturePreview.getTexturePreviewSource()
        }
    }
} 