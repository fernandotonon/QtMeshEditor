import QtQuick 6.0
import QtQuick.Controls 6.0
import QtQuick.Layouts 6.0
import QtQuick.Dialogs
import MaterialEditorQML 1.0

GroupBox {
    title: "Texture Properties"
    
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
                    
                    Label {
                        text: "Texture:"
                        Layout.alignment: Qt.AlignVCenter
                    }
                    
                    TextField {
                        id: textureNameField
                        Layout.fillWidth: true
                        text: MaterialEditorQML.textureName
                        placeholderText: "Select a texture..."
                        readOnly: true
                        background: Rectangle {
                            color: textureNameField.readOnly ? "#f0f0f0" : "white"
                            border.color: "#cccccc"
                            border.width: 1
                        }
                    }
                    
                    Button {
                        text: "Browse..."
                        onClicked: textureFileDialog.open()
                    }
                }
                
                // Available textures dropdown
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    
                    Label {
                        text: "Available:"
                        Layout.alignment: Qt.AlignVCenter
                    }
                    
                    ComboBox {
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
                color: "#f5f5f5"
                border.color: "#cccccc"
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
                    color: "#666666"
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
                Label { 
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
                    
                    SpinBox {
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
                Label { 
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
                    
                    SpinBox {
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
                Button {
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
                    color: "#444444"
                }
                
                Text {
                    text: texturePreview.source != "" && texturePreview.status === Image.Ready ? 
                          "Size: " + texturePreview.sourceSize.width + " x " + texturePreview.sourceSize.height :
                          "Size: Unknown"
                    font.pointSize: 10
                    color: "#444444"
                }
                
                Text {
                    text: "Animation: " + 
                          (MaterialEditorQML.scrollAnimUSpeed != 0.0 || MaterialEditorQML.scrollAnimVSpeed != 0.0 ? 
                           "Enabled (" + MaterialEditorQML.scrollAnimUSpeed.toFixed(2) + ", " + MaterialEditorQML.scrollAnimVSpeed.toFixed(2) + ")" : 
                           "Disabled")
                    font.pointSize: 10
                    color: "#444444"
                }
            }
        }
    }

    // File dialog for texture selection
    FileDialog {
        id: textureFileDialog
        title: "Select Texture"
        fileMode: FileDialog.OpenFile
        nameFilters: [
            "Image files (*.png *.jpg *.jpeg *.bmp *.tga *.dds)",
            "PNG files (*.png)",
            "JPEG files (*.jpg *.jpeg)",
            "Bitmap files (*.bmp)",
            "TGA files (*.tga)",
            "DDS files (*.dds)",
            "All files (*)"
        ]
        onAccepted: {
            var path = selectedFile.toString()
            var fileName = path.substring(path.lastIndexOf('/') + 1)
            MaterialEditorQML.setTextureName(fileName)
            textureNameField.text = fileName
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