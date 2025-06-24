import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Dialogs 1.3
import MaterialEditorQML 1.0

GroupBox {
    title: "Texture Properties"
    
    ColumnLayout {
        anchors.fill: parent
        spacing: 10
        
        // Texture Selection
        GroupBox {
            title: "Texture"
            Layout.fillWidth: true
            
            ColumnLayout {
                anchors.fill: parent
                
                RowLayout {
                    Label {
                        text: "Current Texture:"
                        Layout.preferredWidth: 100
                    }
                    
                    Label {
                        text: MaterialEditorQML.textureName
                        Layout.fillWidth: true
                        color: MaterialEditorQML.textureName === "*Select a texture*" ? "#999" : "#000"
                        font.italic: MaterialEditorQML.textureName === "*Select a texture*"
                    }
                }
                
                RowLayout {
                    Button {
                        text: "Browse..."
                        icon.source: "qrc:/icons/folder.png"
                        onClicked: MaterialEditorQML.selectTexture()
                    }
                    
                    Button {
                        text: "Remove"
                        icon.source: "qrc:/icons/remove.png"
                        enabled: MaterialEditorQML.textureName !== "*Select a texture*"
                        onClicked: MaterialEditorQML.removeTexture()
                    }
                    
                    Item { Layout.fillWidth: true }
                }
                
                // Texture preview (if available)
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 150
                    color: "#f0f0f0"
                    border.color: "#ccc"
                    border.width: 1
                    visible: MaterialEditorQML.textureName !== "*Select a texture*"
                    
                    Label {
                        anchors.centerIn: parent
                        text: "Texture Preview\n(Not implemented)"
                        color: "#999"
                        horizontalAlignment: Text.AlignHCenter
                        font.italic: true
                    }
                    
                    // TODO: Add actual texture preview using Image or custom renderer
                }
            }
        }
        
        // Texture Animation
        GroupBox {
            title: "Texture Animation"
            Layout.fillWidth: true
            
            GridLayout {
                anchors.fill: parent
                columns: 2
                columnSpacing: 10
                rowSpacing: 8
                
                Label { text: "U Scroll Speed:" }
                RowLayout {
                    Slider {
                        id: uScrollSlider
                        Layout.fillWidth: true
                        from: -5.0
                        to: 5.0
                        value: MaterialEditorQML.scrollAnimUSpeed
                        stepSize: 0.1
                        onValueChanged: {
                            if (Math.abs(value - MaterialEditorQML.scrollAnimUSpeed) > 0.01) {
                                MaterialEditorQML.scrollAnimUSpeed = value
                            }
                        }
                    }
                    SpinBox {
                        from: -500
                        to: 500
                        value: Math.round(uScrollSlider.value * 100)
                        onValueChanged: uScrollSlider.value = value / 100.0
                        textFromValue: function(value, locale) {
                            return Number(value / 100).toLocaleString(locale, 'f', 2)
                        }
                        valueFromText: function(text, locale) {
                            return Math.round(Number.fromLocaleString(locale, text) * 100)
                        }
                    }
                }
                
                Label { text: "V Scroll Speed:" }
                RowLayout {
                    Slider {
                        id: vScrollSlider
                        Layout.fillWidth: true
                        from: -5.0
                        to: 5.0
                        value: MaterialEditorQML.scrollAnimVSpeed
                        stepSize: 0.1
                        onValueChanged: {
                            if (Math.abs(value - MaterialEditorQML.scrollAnimVSpeed) > 0.01) {
                                MaterialEditorQML.scrollAnimVSpeed = value
                            }
                        }
                    }
                    SpinBox {
                        from: -500
                        to: 500
                        value: Math.round(vScrollSlider.value * 100)
                        onValueChanged: vScrollSlider.value = value / 100.0
                        textFromValue: function(value, locale) {
                            return Number(value / 100).toLocaleString(locale, 'f', 2)
                        }
                        valueFromText: function(text, locale) {
                            return Math.round(Number.fromLocaleString(locale, text) * 100)
                        }
                    }
                }
            }
        }
        
        // Texture Coordinate Transformation (Future Enhancement)
        GroupBox {
            title: "Texture Coordinates"
            Layout.fillWidth: true
            visible: false // Hidden for now, can be enabled for future features
            
            GridLayout {
                anchors.fill: parent
                columns: 2
                columnSpacing: 10
                rowSpacing: 5
                
                Label { text: "U Scale:" }
                SpinBox {
                    Layout.fillWidth: true
                    from: 1
                    to: 1000
                    value: 100
                    textFromValue: function(value, locale) {
                        return Number(value / 100).toLocaleString(locale, 'f', 2)
                    }
                    valueFromText: function(text, locale) {
                        return Math.round(Number.fromLocaleString(locale, text) * 100)
                    }
                }
                
                Label { text: "V Scale:" }
                SpinBox {
                    Layout.fillWidth: true
                    from: 1
                    to: 1000
                    value: 100
                    textFromValue: function(value, locale) {
                        return Number(value / 100).toLocaleString(locale, 'f', 2)
                    }
                    valueFromText: function(text, locale) {
                        return Math.round(Number.fromLocaleString(locale, text) * 100)
                    }
                }
                
                Label { text: "U Offset:" }
                SpinBox {
                    Layout.fillWidth: true
                    from: -1000
                    to: 1000
                    value: 0
                    textFromValue: function(value, locale) {
                        return Number(value / 100).toLocaleString(locale, 'f', 2)
                    }
                    valueFromText: function(text, locale) {
                        return Math.round(Number.fromLocaleString(locale, text) * 100)
                    }
                }
                
                Label { text: "V Offset:" }
                SpinBox {
                    Layout.fillWidth: true
                    from: -1000
                    to: 1000
                    value: 0
                    textFromValue: function(value, locale) {
                        return Number(value / 100).toLocaleString(locale, 'f', 2)
                    }
                    valueFromText: function(text, locale) {
                        return Math.round(Number.fromLocaleString(locale, text) * 100)
                    }
                }
                
                Label { text: "Rotation:" }
                RowLayout {
                    Slider {
                        Layout.fillWidth: true
                        from: 0
                        to: 360
                        value: 0
                    }
                    Label {
                        text: "0°"
                        Layout.preferredWidth: 30
                    }
                }
            }
        }
        
        // Texture Filtering (Future Enhancement)
        GroupBox {
            title: "Filtering"
            Layout.fillWidth: true
            visible: false // Hidden for now
            
            GridLayout {
                anchors.fill: parent
                columns: 2
                columnSpacing: 10
                rowSpacing: 5
                
                Label { text: "Min Filter:" }
                ComboBox {
                    Layout.fillWidth: true
                    model: ["None", "Point", "Linear", "Anisotropic"]
                    currentIndex: 2
                }
                
                Label { text: "Mag Filter:" }
                ComboBox {
                    Layout.fillWidth: true
                    model: ["None", "Point", "Linear", "Anisotropic"]
                    currentIndex: 2
                }
                
                Label { text: "Mip Filter:" }
                ComboBox {
                    Layout.fillWidth: true
                    model: ["None", "Point", "Linear"]
                    currentIndex: 2
                }
            }
        }
    }
} 