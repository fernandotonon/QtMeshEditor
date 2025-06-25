import QtQuick 6.0
import QtQuick.Controls 6.0
import QtQuick.Layouts 6.0
import QtQuick.Dialogs
import MaterialEditorQML 1.0

GroupBox {
    title: "Pass Properties"

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

    // Simplified ComboBox component (no Canvas)
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
        popup: Popup {
            y: parent.height - 1
            width: parent.width
            implicitHeight: contentItem.implicitHeight
            padding: 1

            contentItem: ListView {
                clip: true
                implicitHeight: contentHeight
                model: parent.parent.popup.visible ? parent.parent.delegateModel : null
                currentIndex: parent.parent.highlightedIndex
                ScrollIndicator.vertical: ScrollIndicator { }
            }

            background: Rectangle {
                color: panelColor
                border.color: borderColor
                border.width: 1
                radius: 4
            }
        }
        delegate: ItemDelegate {
            width: parent.width
            contentItem: Text {
                text: modelData || ""
                color: textColor
                font: parent.font
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
                leftPadding: 8
            }
            background: Rectangle {
                color: parent.hovered ? highlightColor : "transparent"
                radius: 2
            }
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

    // Simplified Label component
    component ThemedLabel: Label {
        color: textColor
    }

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

                    ThemedLabel { 
                        text: "Polygon Mode:" 
                        Layout.alignment: Qt.AlignVCenter
                    }
                    ThemedComboBox {
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

        // Colors
        GroupBox {
            title: "Colors"
            Layout.fillWidth: true

            GridLayout {
                anchors.fill: parent
                columns: 3
                rowSpacing: 10
                columnSpacing: 15

                // Ambient Color
                ThemedLabel { 
                    text: "Ambient:"
                    Layout.alignment: Qt.AlignVCenter
                }
                Rectangle {
                    width: 40
                    height: 25
                    color: MaterialEditorQML.ambientColor
                    border.color: borderColor
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
                ThemedLabel { 
                    text: "Diffuse:"
                    Layout.alignment: Qt.AlignVCenter
                }
                Rectangle {
                    width: 40
                    height: 25
                    color: MaterialEditorQML.diffuseColor
                    border.color: borderColor
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
                ThemedLabel { 
                    text: "Specular:"
                    Layout.alignment: Qt.AlignVCenter
                }
                Rectangle {
                    width: 40
                    height: 25
                    color: MaterialEditorQML.specularColor
                    border.color: borderColor
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
                ThemedLabel { 
                    text: "Emissive:"
                    Layout.alignment: Qt.AlignVCenter
                }
                Rectangle {
                    width: 40
                    height: 25
                    color: MaterialEditorQML.emissiveColor
                    border.color: borderColor
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
            title: "Alpha & Material"
            Layout.fillWidth: true

            GridLayout {
                anchors.fill: parent
                columns: 2
                rowSpacing: 10

                // Diffuse Alpha
                ThemedLabel { text: "Diffuse Alpha:" }
                RowLayout {
                    Slider {
                        id: diffuseAlphaSlider
                        from: 0.0
                        to: 1.0
                        value: MaterialEditorQML.diffuseAlpha
                        onValueChanged: MaterialEditorQML.setDiffuseAlpha(value)
                        Layout.fillWidth: true
                    }
                    ThemedSpinBox {
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
                ThemedLabel { text: "Specular Alpha:" }
                RowLayout {
                    Slider {
                        id: specularAlphaSlider
                        from: 0.0
                        to: 1.0
                        value: MaterialEditorQML.specularAlpha
                        onValueChanged: MaterialEditorQML.setSpecularAlpha(value)
                        Layout.fillWidth: true
                    }
                    ThemedSpinBox {
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
                ThemedLabel { text: "Shininess:" }
                RowLayout {
                    Slider {
                        id: shininessSlider
                        from: 0.0
                        to: 128.0
                        value: MaterialEditorQML.shininess
                        onValueChanged: MaterialEditorQML.setShininess(value)
                        Layout.fillWidth: true
                    }
                    ThemedSpinBox {
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
            title: "Blending"
            Layout.fillWidth: true

            GridLayout {
                anchors.fill: parent
                columns: 2
                rowSpacing: 10
                columnSpacing: 15

                ThemedLabel { text: "Source Blend Factor:" }
                ThemedComboBox {
                    Layout.fillWidth: true
                    model: MaterialEditorQML.getBlendFactorNames()
                    currentIndex: MaterialEditorQML.sourceBlendFactor
                    onCurrentIndexChanged: MaterialEditorQML.setSourceBlendFactor(currentIndex)
                }

                ThemedLabel { text: "Dest Blend Factor:" }
                ThemedComboBox {
                    Layout.fillWidth: true
                    model: MaterialEditorQML.getBlendFactorNames()
                    currentIndex: MaterialEditorQML.destBlendFactor
                    onCurrentIndexChanged: MaterialEditorQML.setDestBlendFactor(currentIndex)
                }
            }
        }
    }

    // Simple color dialogs using standard Dialog components
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