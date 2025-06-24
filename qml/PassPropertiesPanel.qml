import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Dialogs 1.3
import MaterialEditorQML 1.0

GroupBox {
    title: "Pass Properties"
    
    ColumnLayout {
        anchors.fill: parent
        spacing: 10
        
        // Basic Properties
        GridLayout {
            columns: 2
            columnSpacing: 10
            rowSpacing: 5
            Layout.fillWidth: true
            
            Label { text: "Lighting:" }
            CheckBox {
                checked: MaterialEditorQML.lightingEnabled
                onCheckedChanged: MaterialEditorQML.lightingEnabled = checked
            }
            
            Label { text: "Depth Write:" }
            CheckBox {
                checked: MaterialEditorQML.depthWriteEnabled
                onCheckedChanged: MaterialEditorQML.depthWriteEnabled = checked
            }
            
            Label { text: "Depth Check:" }
            CheckBox {
                checked: MaterialEditorQML.depthCheckEnabled
                onCheckedChanged: MaterialEditorQML.depthCheckEnabled = checked
            }
            
            Label { text: "Polygon Mode:" }
            ComboBox {
                Layout.fillWidth: true
                model: MaterialEditorQML.getPolygonModeNames()
                currentIndex: MaterialEditorQML.polygonMode
                onCurrentIndexChanged: {
                    if (currentIndex !== MaterialEditorQML.polygonMode) {
                        MaterialEditorQML.polygonMode = currentIndex
                    }
                }
            }
        }
        
        // Colors Section
        GroupBox {
            title: "Colors"
            Layout.fillWidth: true
            
            GridLayout {
                anchors.fill: parent
                columns: 3
                columnSpacing: 10
                rowSpacing: 8
                
                // Ambient Color
                Label { text: "Ambient:" }
                Rectangle {
                    width: 40
                    height: 25
                    color: MaterialEditorQML.ambientColor
                    border.color: "#999"
                    border.width: 1
                    
                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            ambientColorDialog.color = MaterialEditorQML.ambientColor
                            ambientColorDialog.open()
                        }
                    }
                }
                CheckBox {
                    text: "Use Vertex Color"
                    checked: MaterialEditorQML.useVertexColorToAmbient
                    onCheckedChanged: MaterialEditorQML.useVertexColorToAmbient = checked
                }
                
                // Diffuse Color
                Label { text: "Diffuse:" }
                Rectangle {
                    width: 40
                    height: 25
                    color: MaterialEditorQML.diffuseColor
                    border.color: "#999"
                    border.width: 1
                    
                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            diffuseColorDialog.color = MaterialEditorQML.diffuseColor
                            diffuseColorDialog.open()
                        }
                    }
                }
                CheckBox {
                    text: "Use Vertex Color"
                    checked: MaterialEditorQML.useVertexColorToDiffuse
                    onCheckedChanged: MaterialEditorQML.useVertexColorToDiffuse = checked
                }
                
                // Specular Color
                Label { text: "Specular:" }
                Rectangle {
                    width: 40
                    height: 25
                    color: MaterialEditorQML.specularColor
                    border.color: "#999"
                    border.width: 1
                    
                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            specularColorDialog.color = MaterialEditorQML.specularColor
                            specularColorDialog.open()
                        }
                    }
                }
                CheckBox {
                    text: "Use Vertex Color"
                    checked: MaterialEditorQML.useVertexColorToSpecular
                    onCheckedChanged: MaterialEditorQML.useVertexColorToSpecular = checked
                }
                
                // Emissive Color
                Label { text: "Emissive:" }
                Rectangle {
                    width: 40
                    height: 25
                    color: MaterialEditorQML.emissiveColor
                    border.color: "#999"
                    border.width: 1
                    
                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            emissiveColorDialog.color = MaterialEditorQML.emissiveColor
                            emissiveColorDialog.open()
                        }
                    }
                }
                CheckBox {
                    text: "Use Vertex Color"
                    checked: MaterialEditorQML.useVertexColorToEmissive
                    onCheckedChanged: MaterialEditorQML.useVertexColorToEmissive = checked
                }
            }
        }
        
        // Alpha and Shininess
        GroupBox {
            title: "Material Properties"
            Layout.fillWidth: true
            
            GridLayout {
                anchors.fill: parent
                columns: 2
                columnSpacing: 10
                rowSpacing: 5
                
                Label { text: "Diffuse Alpha:" }
                RowLayout {
                    Slider {
                        id: diffuseAlphaSlider
                        Layout.fillWidth: true
                        from: 0.0
                        to: 1.0
                        value: MaterialEditorQML.diffuseAlpha
                        onValueChanged: {
                            if (Math.abs(value - MaterialEditorQML.diffuseAlpha) > 0.001) {
                                MaterialEditorQML.diffuseAlpha = value
                            }
                        }
                    }
                    SpinBox {
                        from: 0
                        to: 100
                        value: Math.round(diffuseAlphaSlider.value * 100)
                        onValueChanged: diffuseAlphaSlider.value = value / 100.0
                    }
                }
                
                Label { text: "Specular Alpha:" }
                RowLayout {
                    Slider {
                        id: specularAlphaSlider
                        Layout.fillWidth: true
                        from: 0.0
                        to: 1.0
                        value: MaterialEditorQML.specularAlpha
                        onValueChanged: {
                            if (Math.abs(value - MaterialEditorQML.specularAlpha) > 0.001) {
                                MaterialEditorQML.specularAlpha = value
                            }
                        }
                    }
                    SpinBox {
                        from: 0
                        to: 100
                        value: Math.round(specularAlphaSlider.value * 100)
                        onValueChanged: specularAlphaSlider.value = value / 100.0
                    }
                }
                
                Label { text: "Shininess:" }
                RowLayout {
                    Slider {
                        id: shininessSlider
                        Layout.fillWidth: true
                        from: 0.0
                        to: 128.0
                        value: MaterialEditorQML.shininess
                        onValueChanged: {
                            if (Math.abs(value - MaterialEditorQML.shininess) > 0.1) {
                                MaterialEditorQML.shininess = value
                            }
                        }
                    }
                    SpinBox {
                        from: 0
                        to: 128
                        value: Math.round(shininessSlider.value)
                        onValueChanged: shininessSlider.value = value
                    }
                }
            }
        }
        
        // Blending
        GroupBox {
            title: "Scene Blending"
            Layout.fillWidth: true
            
            GridLayout {
                anchors.fill: parent
                columns: 2
                columnSpacing: 10
                rowSpacing: 5
                
                Label { text: "Source Blend:" }
                ComboBox {
                    Layout.fillWidth: true
                    model: MaterialEditorQML.getBlendFactorNames()
                    currentIndex: MaterialEditorQML.sourceBlendFactor
                    onCurrentIndexChanged: {
                        if (currentIndex !== MaterialEditorQML.sourceBlendFactor) {
                            MaterialEditorQML.sourceBlendFactor = currentIndex
                        }
                    }
                }
                
                Label { text: "Dest Blend:" }
                ComboBox {
                    Layout.fillWidth: true
                    model: MaterialEditorQML.getBlendFactorNames()
                    currentIndex: MaterialEditorQML.destBlendFactor
                    onCurrentIndexChanged: {
                        if (currentIndex !== MaterialEditorQML.destBlendFactor) {
                            MaterialEditorQML.destBlendFactor = currentIndex
                        }
                    }
                }
            }
        }
    }
    
    // Color Dialogs
    ColorDialog {
        id: ambientColorDialog
        title: "Select Ambient Color"
        onAccepted: MaterialEditorQML.ambientColor = color
    }
    
    ColorDialog {
        id: diffuseColorDialog
        title: "Select Diffuse Color"
        onAccepted: MaterialEditorQML.diffuseColor = color
    }
    
    ColorDialog {
        id: specularColorDialog
        title: "Select Specular Color"
        onAccepted: MaterialEditorQML.specularColor = color
    }
    
    ColorDialog {
        id: emissiveColorDialog
        title: "Select Emissive Color"
        onAccepted: MaterialEditorQML.emissiveColor = color
    }
} 