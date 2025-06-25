import QtQuick 6.0
import QtQuick.Controls 6.0
import QtQuick.Layouts 6.0
import QtQuick.Dialogs
import MaterialEditorQML 1.0

GroupBox {
    title: "Texture Properties"

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

    // Simplified Button component
    component ThemedButton: Button {
        background: Rectangle {
            color: parent.down ? Qt.darker(buttonColor, 1.2) : 
                   parent.hovered ? Qt.lighter(buttonColor, 1.1) : buttonColor
            border.color: borderColor
            border.width: 1
            radius: 4
        }
        contentItem: Text {
            text: parent.text
            font: parent.font
            color: parent.enabled ? buttonTextColor : disabledTextColor
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }

    // Simplified ComboBox component
    component ThemedComboBox: ComboBox {
        background: Rectangle {
            color: panelColor
            border.color: borderColor
            border.width: 1
            radius: 4
        }
        contentItem: Text {
            text: parent.displayText
            font: parent.font
            color: textColor
            verticalAlignment: Text.AlignVCenter
            leftPadding: 8
            rightPadding: 30
        }
        indicator: Text {
            x: parent.width - width - 8
            y: parent.topPadding + (parent.availableHeight - height) / 2
            text: "▼"
            color: textColor
            font.pointSize: 8
        }
    }

    // Simplified SpinBox component
    component ThemedSpinBox: SpinBox {
        background: Rectangle {
            color: panelColor
            border.color: borderColor
            border.width: 1
            radius: 4
        }
        contentItem: TextInput {
            text: parent.textFromValue(parent.value, parent.locale)
            font: parent.font
            color: textColor
            selectionColor: highlightColor
            selectedTextColor: backgroundColor
            horizontalAlignment: Qt.AlignHCenter
            verticalAlignment: Qt.AlignVCenter
            readOnly: !parent.editable
            validator: parent.validator
            inputMethodHints: parent.inputMethodHints
        }
        up.indicator: Rectangle {
            x: parent.mirrored ? 0 : parent.width - width
            height: parent.height / 2
            color: parent.up.pressed ? Qt.darker(buttonColor, 1.2) : 
                   parent.up.hovered ? Qt.lighter(buttonColor, 1.1) : buttonColor
            border.color: borderColor
            border.width: 1
            radius: 4
            Text {
                text: "+"
                font.pointSize: 10
                color: buttonTextColor
                anchors.centerIn: parent
            }
        }
        down.indicator: Rectangle {
            x: parent.mirrored ? 0 : parent.width - width
            y: parent.height / 2
            height: parent.height / 2
            color: parent.down.pressed ? Qt.darker(buttonColor, 1.2) : 
                   parent.down.hovered ? Qt.lighter(buttonColor, 1.1) : buttonColor
            border.color: borderColor
            border.width: 1
            radius: 4
            Text {
                text: "-"
                font.pointSize: 10
                color: buttonTextColor
                anchors.centerIn: parent
            }
        }
    }

    // Simplified TextField component
    component ThemedTextField: TextField {
        color: textColor
        selectionColor: highlightColor
        selectedTextColor: backgroundColor
        background: Rectangle {
            color: panelColor
            border.color: borderColor
            border.width: 1
            radius: 4
        }
    }

    // Simplified Label component
    component ThemedLabel: Label {
        color: textColor
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
                    source: getTexturePreviewSource()
                    
                    function getTexturePreviewSource() {
                        var texName = MaterialEditorQML.textureName
                        if (texName && texName !== "*Select a texture*" && texName.trim() !== "") {
                            // Try to construct a file path for the texture
                            // This would need to be adapted based on your texture path structure
                            return "file:///media/materials/textures/" + texName
                        }
                        return ""
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
        
        // Animation Controls
        GroupBox {
            title: "Texture Animation"
            Layout.fillWidth: true
            
            GridLayout {
                anchors.fill: parent
                columns: 2
                rowSpacing: 10
                columnSpacing: 15
                
                // U Scroll Speed
                ThemedLabel { 
                    text: "U Scroll Speed:"
                    Layout.alignment: Qt.AlignVCenter
                }
                RowLayout {
                    Layout.fillWidth: true
                    
                    Slider {
                        id: uScrollSlider
                        Layout.fillWidth: true
                        from: -10.0
                        to: 10.0
                        value: MaterialEditorQML.scrollAnimUSpeed
                        onValueChanged: MaterialEditorQML.setScrollAnimUSpeed(value)
                        stepSize: 0.01
                    }
                    
                    ThemedSpinBox {
                        property int decimals: 2
                        
                        from: -1000
                        to: 1000
                        value: uScrollSlider.value * 100
                        onValueChanged: uScrollSlider.value = value / 100.0
                        
                        validator: DoubleValidator {
                            bottom: Math.min(uScrollSlider.from, uScrollSlider.to)
                            top: Math.max(uScrollSlider.from, uScrollSlider.to)
                        }
                        
                        textFromValue: function(value, locale) {
                            return Number(value / 100).toLocaleString(locale, 'f', decimals)
                        }
                        
                        valueFromText: function(text, locale) {
                            return Number.fromLocaleString(locale, text) * 100
                        }
                    }
                }
                
                // V Scroll Speed
                ThemedLabel { 
                    text: "V Scroll Speed:"
                    Layout.alignment: Qt.AlignVCenter
                }
                RowLayout {
                    Layout.fillWidth: true
                    
                    Slider {
                        id: vScrollSlider
                        Layout.fillWidth: true
                        from: -10.0
                        to: 10.0
                        value: MaterialEditorQML.scrollAnimVSpeed
                        onValueChanged: MaterialEditorQML.setScrollAnimVSpeed(value)
                        stepSize: 0.01
                    }
                    
                    ThemedSpinBox {
                        property int decimals: 2
                        
                        from: -1000
                        to: 1000
                        value: vScrollSlider.value * 100
                        onValueChanged: vScrollSlider.value = value / 100.0
                        
                        validator: DoubleValidator {
                            bottom: Math.min(vScrollSlider.from, vScrollSlider.to)
                            top: Math.max(vScrollSlider.from, vScrollSlider.to)
                        }
                        
                        textFromValue: function(value, locale) {
                            return Number(value / 100).toLocaleString(locale, 'f', decimals)
                        }
                        
                        valueFromText: function(text, locale) {
                            return Number.fromLocaleString(locale, text) * 100
                        }
                    }
                }
                
                // Reset button
                Item { Layout.fillWidth: true }
                ThemedButton {
                    text: "Reset Animation"
                    onClicked: {
                        MaterialEditorQML.setScrollAnimUSpeed(0.0)
                        MaterialEditorQML.setScrollAnimVSpeed(0.0)
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

    // Update UI when texture properties change
    Connections {
        target: MaterialEditorQML
        function onTextureNameChanged() {
            textureNameField.text = MaterialEditorQML.textureName
            texturePreview.source = texturePreview.getTexturePreviewSource()
        }
    }
} 
} 