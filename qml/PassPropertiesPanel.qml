import QtQuick 6.0
import QtQuick.Controls 6.0
import QtQuick.Layouts 6.0
import QtQuick.Dialogs
import MaterialEditorQML 1.0

GroupBox {
    title: "Pass Properties"

    ColumnLayout {
        anchors.fill: parent
        spacing: 15

        // Lighting and Depth Settings
        GroupBox {
            title: "Lighting & Depth"
            Layout.fillWidth: true

            ColumnLayout {
                anchors.fill: parent
                spacing: 10

                // First row: Lighting, Depth Write, Depth Check
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 15

                    CheckBox {
                        text: "Lighting"
                        checked: MaterialEditorQML.lightingEnabled
                        onCheckedChanged: MaterialEditorQML.setLightingEnabled(checked)
                    }

                    CheckBox {
                        text: "Depth Write"
                        checked: MaterialEditorQML.depthWriteEnabled
                        onCheckedChanged: MaterialEditorQML.setDepthWriteEnabled(checked)
                    }

                    CheckBox {
                        text: "Depth Check"
                        checked: MaterialEditorQML.depthCheckEnabled
                        onCheckedChanged: MaterialEditorQML.setDepthCheckEnabled(checked)
                    }

                    // Spacer to push everything to the left
                    Item { Layout.fillWidth: true }
                }

                // Second row: Polygon Mode
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 15

                    Label { 
                        text: "Polygon Mode:" 
                        Layout.alignment: Qt.AlignVCenter
                    }
                    ComboBox {
                        id: polygonModeCombo
                        model: MaterialEditorQML.getPolygonModeNames()
                        currentIndex: MaterialEditorQML.polygonMode
                        onCurrentIndexChanged: {
                            if (currentIndex !== MaterialEditorQML.polygonMode) {
                                MaterialEditorQML.setPolygonMode(currentIndex)
                            }
                        }
                        
                        // Ensure the ComboBox updates when the backend changes
                        Connections {
                            target: MaterialEditorQML
                            function onPolygonModeChanged() {
                                polygonModeCombo.currentIndex = MaterialEditorQML.polygonMode
                            }
                        }
                    }

                    // Spacer to push everything to the left
                    Item { Layout.fillWidth: true }
                }
            }
        }

        // Color Properties
        GroupBox {
            title: "Colors"
            Layout.fillWidth: true

            GridLayout {
                anchors.fill: parent
                columns: 3
                rowSpacing: 10
                columnSpacing: 10

                // Ambient Color
                Label { 
                    text: "Ambient:"
                    Layout.alignment: Qt.AlignVCenter
                }
                Rectangle {
                    width: 40
                    height: 25
                    color: MaterialEditorQML.ambientColor
                    border.color: "#666666"
                    border.width: 1
                    Layout.alignment: Qt.AlignVCenter
                    
                    MouseArea {
                        anchors.fill: parent
                        onClicked: ambientColorDialog.open()
                        cursorShape: Qt.PointingHandCursor
                    }
                }
                CheckBox {
                    text: "Use Vertex Color"
                    checked: MaterialEditorQML.useVertexColorToAmbient
                    onCheckedChanged: MaterialEditorQML.setUseVertexColorToAmbient(checked)
                }

                // Diffuse Color
                Label { 
                    text: "Diffuse:"
                    Layout.alignment: Qt.AlignVCenter
                }
                Rectangle {
                    width: 40
                    height: 25
                    color: MaterialEditorQML.diffuseColor
                    border.color: "#666666"
                    border.width: 1
                    Layout.alignment: Qt.AlignVCenter
                    
                    MouseArea {
                        anchors.fill: parent
                        onClicked: diffuseColorDialog.open()
                        cursorShape: Qt.PointingHandCursor
                    }
                }
                CheckBox {
                    text: "Use Vertex Color"
                    checked: MaterialEditorQML.useVertexColorToDiffuse
                    onCheckedChanged: MaterialEditorQML.setUseVertexColorToDiffuse(checked)
                }

                // Specular Color
                Label { 
                    text: "Specular:"
                    Layout.alignment: Qt.AlignVCenter
                }
                Rectangle {
                    width: 40
                    height: 25
                    color: MaterialEditorQML.specularColor
                    border.color: "#666666"
                    border.width: 1
                    Layout.alignment: Qt.AlignVCenter
                    
                    MouseArea {
                        anchors.fill: parent
                        onClicked: specularColorDialog.open()
                        cursorShape: Qt.PointingHandCursor
                    }
                }
                CheckBox {
                    text: "Use Vertex Color"
                    checked: MaterialEditorQML.useVertexColorToSpecular
                    onCheckedChanged: MaterialEditorQML.setUseVertexColorToSpecular(checked)
                }

                // Emissive Color
                Label { 
                    text: "Emissive:"
                    Layout.alignment: Qt.AlignVCenter
                }
                Rectangle {
                    width: 40
                    height: 25
                    color: MaterialEditorQML.emissiveColor
                    border.color: "#666666"
                    border.width: 1
                    Layout.alignment: Qt.AlignVCenter
                    
                    MouseArea {
                        anchors.fill: parent
                        onClicked: emissiveColorDialog.open()
                        cursorShape: Qt.PointingHandCursor
                    }
                }
                CheckBox {
                    text: "Use Vertex Color"
                    checked: MaterialEditorQML.useVertexColorToEmissive
                    onCheckedChanged: MaterialEditorQML.setUseVertexColorToEmissive(checked)
                }
            }
        }

        // Alpha and Material Properties
        GroupBox {
            title: "Alpha & Material Properties"
            Layout.fillWidth: true

            GridLayout {
                anchors.fill: parent
                columns: 2
                rowSpacing: 10
                columnSpacing: 15

                // Diffuse Alpha
                Label { text: "Diffuse Alpha:" }
                RowLayout {
                    Slider {
                        id: diffuseAlphaSlider
                        from: 0.0
                        to: 1.0
                        value: MaterialEditorQML.diffuseAlpha
                        onValueChanged: MaterialEditorQML.setDiffuseAlpha(value)
                        Layout.fillWidth: true
                    }
                    SpinBox {
                        from: 0
                        to: 100
                        value: Math.round(diffuseAlphaSlider.value * 100)
                        onValueChanged: diffuseAlphaSlider.value = value / 100.0
                        
                        textFromValue: function(value, locale) {
                            return value + "%"
                        }
                        
                        valueFromText: function(text, locale) {
                            return parseInt(text.replace("%", ""))
                        }
                    }
                }

                // Specular Alpha
                Label { text: "Specular Alpha:" }
                RowLayout {
                    Slider {
                        id: specularAlphaSlider
                        from: 0.0
                        to: 1.0
                        value: MaterialEditorQML.specularAlpha
                        onValueChanged: MaterialEditorQML.setSpecularAlpha(value)
                        Layout.fillWidth: true
                    }
                    SpinBox {
                        from: 0
                        to: 100
                        value: Math.round(specularAlphaSlider.value * 100)
                        onValueChanged: specularAlphaSlider.value = value / 100.0
                        
                        textFromValue: function(value, locale) {
                            return value + "%"
                        }
                        
                        valueFromText: function(text, locale) {
                            return parseInt(text.replace("%", ""))
                        }
                    }
                }

                // Shininess
                Label { text: "Shininess:" }
                RowLayout {
                    Slider {
                        id: shininessSlider
                        from: 0.0
                        to: 128.0
                        value: MaterialEditorQML.shininess
                        onValueChanged: MaterialEditorQML.setShininess(value)
                        Layout.fillWidth: true
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

        // Blending Settings
        GroupBox {
            title: "Blending"
            Layout.fillWidth: true

            GridLayout {
                anchors.fill: parent
                columns: 2
                rowSpacing: 10
                columnSpacing: 15

                Label { text: "Source Blend Factor:" }
                ComboBox {
                    Layout.fillWidth: true
                    model: MaterialEditorQML.getBlendFactorNames()
                    currentIndex: MaterialEditorQML.sourceBlendFactor
                    onCurrentIndexChanged: MaterialEditorQML.setSourceBlendFactor(currentIndex)
                }

                Label { text: "Dest Blend Factor:" }
                ComboBox {
                    Layout.fillWidth: true
                    model: MaterialEditorQML.getBlendFactorNames()
                    currentIndex: MaterialEditorQML.destBlendFactor
                    onCurrentIndexChanged: MaterialEditorQML.setDestBlendFactor(currentIndex)
                }
            }
        }
    }

    // Color Dialogs
    ColorDialog {
        id: ambientColorDialog
        title: "Select Ambient Color"
        selectedColor: MaterialEditorQML.ambientColor
        onAccepted: {
            MaterialEditorQML.setAmbientColor(selectedColor)
        }
    }

    ColorDialog {
        id: diffuseColorDialog
        title: "Select Diffuse Color"
        selectedColor: MaterialEditorQML.diffuseColor
        onAccepted: {
            MaterialEditorQML.setDiffuseColor(selectedColor)
        }
    }

    ColorDialog {
        id: specularColorDialog
        title: "Select Specular Color"
        selectedColor: MaterialEditorQML.specularColor
        onAccepted: {
            MaterialEditorQML.setSpecularColor(selectedColor)
        }
    }

    ColorDialog {
        id: emissiveColorDialog
        title: "Select Emissive Color"
        selectedColor: MaterialEditorQML.emissiveColor
        onAccepted: {
            MaterialEditorQML.setEmissiveColor(selectedColor)
        }
    }
} 