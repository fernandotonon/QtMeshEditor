import QtQuick 6.0
import QtQuick.Controls 6.0
import QtQuick.Layouts 6.0
import QtQuick.Dialogs
import MaterialEditorQML 1.0

GroupBox {
    title: "Pass Properties"
    
    Component.onCompleted: {
        console.log("PassPropertiesPanel: loaded successfully")
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
                        onClicked: {
                            console.log("Ambient color rectangle clicked")
                            colorPickerPopup.openForColor("ambient", MaterialEditorQML.ambientColor)
                        }
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
                        onClicked: {
                            console.log("Diffuse color rectangle clicked")
                            colorPickerPopup.openForColor("diffuse", MaterialEditorQML.diffuseColor)
                        }
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
                        onClicked: {
                            console.log("Specular color rectangle clicked")
                            colorPickerPopup.openForColor("specular", MaterialEditorQML.specularColor)
                        }
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
                        onClicked: {
                            console.log("Emissive color rectangle clicked")
                            colorPickerPopup.openForColor("emissive", MaterialEditorQML.emissiveColor)
                        }
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

        // Advanced Rendering Properties Group
        GroupBox {
            title: "Advanced Rendering"
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
                
                // Shading Mode
                RowLayout {
                    ThemedLabel { text: "Shading Mode:" }
                    ThemedComboBox {
                        id: shadingModeCombo
                        Layout.fillWidth: true
                        model: MaterialEditorQML.getShadingModeNames()
                        currentIndex: MaterialEditorQML.shadingMode
                        onCurrentIndexChanged: {
                            if (currentIndex !== MaterialEditorQML.shadingMode) {
                                MaterialEditorQML.shadingMode = currentIndex
                            }
                        }
                        Connections {
                            target: MaterialEditorQML
                            function onShadingModeChanged() {
                                shadingModeCombo.currentIndex = MaterialEditorQML.shadingMode
                            }
                        }
                    }
                }
                
                // Hardware Culling
                RowLayout {
                    ThemedLabel { text: "Hardware Culling:" }
                    ThemedComboBox {
                        id: cullHardwareCombo
                        Layout.fillWidth: true
                        model: MaterialEditorQML.getCullModeNames()
                        currentIndex: MaterialEditorQML.cullHardware
                        onCurrentIndexChanged: {
                            if (currentIndex !== MaterialEditorQML.cullHardware) {
                                MaterialEditorQML.cullHardware = currentIndex
                            }
                        }
                        Connections {
                            target: MaterialEditorQML
                            function onCullHardwareChanged() {
                                cullHardwareCombo.currentIndex = MaterialEditorQML.cullHardware
                            }
                        }
                    }
                }
                
                // Software Culling
                RowLayout {
                    ThemedLabel { text: "Software Culling:" }
                    ThemedComboBox {
                        id: cullSoftwareCombo
                        Layout.fillWidth: true
                        model: MaterialEditorQML.getCullModeNames()
                        currentIndex: MaterialEditorQML.cullSoftware
                        onCurrentIndexChanged: {
                            if (currentIndex !== MaterialEditorQML.cullSoftware) {
                                MaterialEditorQML.cullSoftware = currentIndex
                            }
                        }
                        Connections {
                            target: MaterialEditorQML
                            function onCullSoftwareChanged() {
                                cullSoftwareCombo.currentIndex = MaterialEditorQML.cullSoftware
                            }
                        }
                    }
                }
            }
        }
        
        // Depth Testing Group
        GroupBox {
            title: "Depth Testing"
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
                
                // Depth Function
                RowLayout {
                    ThemedLabel { text: "Depth Function:" }
                    ThemedComboBox {
                        id: depthFunctionCombo
                        Layout.fillWidth: true
                        model: MaterialEditorQML.getDepthFunctionNames()
                        currentIndex: MaterialEditorQML.depthFunction
                        onCurrentIndexChanged: {
                            if (currentIndex !== MaterialEditorQML.depthFunction) {
                                MaterialEditorQML.depthFunction = currentIndex
                            }
                        }
                        Connections {
                            target: MaterialEditorQML
                            function onDepthFunctionChanged() {
                                depthFunctionCombo.currentIndex = MaterialEditorQML.depthFunction
                            }
                        }
                    }
                }
                
                // Depth Bias Constant
                RowLayout {
                    ThemedLabel { text: "Depth Bias Constant:" }
                    ThemedSpinBox {
                        id: depthBiasConstantSpin
                        Layout.fillWidth: true
                        from: -100000
                        to: 100000
                        stepSize: 1
                        value: Math.round(MaterialEditorQML.depthBiasConstant * 1000)
                        onValueChanged: {
                            if (value !== Math.round(MaterialEditorQML.depthBiasConstant * 1000)) {
                                MaterialEditorQML.depthBiasConstant = value / 1000.0
                            }
                        }
                        Connections {
                            target: MaterialEditorQML
                            function onDepthBiasConstantChanged() {
                                depthBiasConstantSpin.value = Math.round(MaterialEditorQML.depthBiasConstant * 1000)
                            }
                        }
                    }
                }
                
                // Depth Bias Slope Scale
                RowLayout {
                    ThemedLabel { text: "Depth Bias Slope Scale:" }
                    ThemedSpinBox {
                        id: depthBiasSlopeSpin
                        Layout.fillWidth: true
                        from: -100000
                        to: 100000
                        stepSize: 1
                        value: Math.round(MaterialEditorQML.depthBiasSlopeScale * 1000)
                        onValueChanged: {
                            if (value !== Math.round(MaterialEditorQML.depthBiasSlopeScale * 1000)) {
                                MaterialEditorQML.depthBiasSlopeScale = value / 1000.0
                            }
                        }
                        Connections {
                            target: MaterialEditorQML
                            function onDepthBiasSlopeScaleChanged() {
                                depthBiasSlopeSpin.value = Math.round(MaterialEditorQML.depthBiasSlopeScale * 1000)
                            }
                        }
                    }
                }
            }
        }
        
        // Alpha Testing Group
        GroupBox {
            title: "Alpha Testing"
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
                
                // Alpha Rejection Enabled
                CheckBox {
                    id: alphaRejectionEnabledCheck
                    text: "Enable Alpha Rejection"
                    checked: MaterialEditorQML.alphaRejectionEnabled
                    onCheckedChanged: {
                        if (checked !== MaterialEditorQML.alphaRejectionEnabled) {
                            MaterialEditorQML.alphaRejectionEnabled = checked
                        }
                    }
                    Connections {
                        target: MaterialEditorQML
                        function onAlphaRejectionEnabledChanged() {
                            alphaRejectionEnabledCheck.checked = MaterialEditorQML.alphaRejectionEnabled
                        }
                    }
                    indicator: Rectangle {
                        implicitWidth: 16
                        implicitHeight: 16
                        x: alphaRejectionEnabledCheck.leftPadding
                        y: parent.height / 2 - height / 2
                        radius: 2
                        color: alphaRejectionEnabledCheck.checked ? MaterialEditorQML.accentColor : MaterialEditorQML.panelColor
                        border.color: MaterialEditorQML.borderColor
                        border.width: 1
                        
                        Rectangle {
                            width: 6
                            height: 6
                            x: 5
                            y: 5
                            radius: 1
                            color: MaterialEditorQML.textColor
                            visible: alphaRejectionEnabledCheck.checked
                        }
                    }
                    contentItem: Text {
                        text: alphaRejectionEnabledCheck.text
                        font: alphaRejectionEnabledCheck.font
                        color: MaterialEditorQML.textColor
                        verticalAlignment: Text.AlignVCenter
                        leftPadding: alphaRejectionEnabledCheck.indicator.width + alphaRejectionEnabledCheck.spacing
                    }
                }
                
                // Alpha Rejection Function
                RowLayout {
                    enabled: MaterialEditorQML.alphaRejectionEnabled
                    ThemedLabel { 
                        text: "Alpha Function:" 
                        color: enabled ? MaterialEditorQML.textColor : MaterialEditorQML.disabledTextColor
                    }
                    ThemedComboBox {
                        id: alphaRejectionFunctionCombo
                        Layout.fillWidth: true
                        enabled: MaterialEditorQML.alphaRejectionEnabled
                        model: MaterialEditorQML.getAlphaRejectionFunctionNames()
                        currentIndex: MaterialEditorQML.alphaRejectionFunction
                        onCurrentIndexChanged: {
                            if (currentIndex !== MaterialEditorQML.alphaRejectionFunction) {
                                MaterialEditorQML.alphaRejectionFunction = currentIndex
                            }
                        }
                        Connections {
                            target: MaterialEditorQML
                            function onAlphaRejectionFunctionChanged() {
                                alphaRejectionFunctionCombo.currentIndex = MaterialEditorQML.alphaRejectionFunction
                            }
                        }
                    }
                }
                
                // Alpha Rejection Value
                RowLayout {
                    enabled: MaterialEditorQML.alphaRejectionEnabled
                    ThemedLabel { 
                        text: "Alpha Value (0-255):" 
                        color: enabled ? MaterialEditorQML.textColor : MaterialEditorQML.disabledTextColor
                    }
                    ThemedSpinBox {
                        id: alphaRejectionValueSpin
                        Layout.fillWidth: true
                        enabled: MaterialEditorQML.alphaRejectionEnabled
                        from: 0
                        to: 255
                        value: MaterialEditorQML.alphaRejectionValue
                        onValueChanged: {
                            if (value !== MaterialEditorQML.alphaRejectionValue) {
                                MaterialEditorQML.alphaRejectionValue = value
                            }
                        }
                        Connections {
                            target: MaterialEditorQML
                            function onAlphaRejectionValueChanged() {
                                alphaRejectionValueSpin.value = MaterialEditorQML.alphaRejectionValue
                            }
                        }
                    }
                }
                
                // Alpha to Coverage
                CheckBox {
                    id: alphaToCoverageCheck
                    text: "Alpha to Coverage"
                    checked: MaterialEditorQML.alphaToCoverageEnabled
                    onCheckedChanged: {
                        if (checked !== MaterialEditorQML.alphaToCoverageEnabled) {
                            MaterialEditorQML.alphaToCoverageEnabled = checked
                        }
                    }
                    Connections {
                        target: MaterialEditorQML
                        function onAlphaToCoverageEnabledChanged() {
                            alphaToCoverageCheck.checked = MaterialEditorQML.alphaToCoverageEnabled
                        }
                    }
                    indicator: Rectangle {
                        implicitWidth: 16
                        implicitHeight: 16
                        x: alphaToCoverageCheck.leftPadding
                        y: parent.height / 2 - height / 2
                        radius: 2
                        color: alphaToCoverageCheck.checked ? MaterialEditorQML.accentColor : MaterialEditorQML.panelColor
                        border.color: MaterialEditorQML.borderColor
                        border.width: 1
                        
                        Rectangle {
                            width: 6
                            height: 6
                            x: 5
                            y: 5
                            radius: 1
                            color: MaterialEditorQML.textColor
                            visible: alphaToCoverageCheck.checked
                        }
                    }
                    contentItem: Text {
                        text: alphaToCoverageCheck.text
                        font: alphaToCoverageCheck.font
                        color: MaterialEditorQML.textColor
                        verticalAlignment: Text.AlignVCenter
                        leftPadding: alphaToCoverageCheck.indicator.width + alphaToCoverageCheck.spacing
                    }
                }
            }
        }
        
        // Color Writing Group
        GroupBox {
            title: "Color Writing"
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
                
                RowLayout {
                    CheckBox {
                        id: colourWriteRedCheck
                        text: "Red"
                        checked: MaterialEditorQML.colourWriteRed
                        onCheckedChanged: {
                            if (checked !== MaterialEditorQML.colourWriteRed) {
                                MaterialEditorQML.colourWriteRed = checked
                            }
                        }
                        Connections {
                            target: MaterialEditorQML
                            function onColourWriteRedChanged() {
                                colourWriteRedCheck.checked = MaterialEditorQML.colourWriteRed
                            }
                        }
                        indicator: Rectangle {
                            implicitWidth: 16
                            implicitHeight: 16
                            x: colourWriteRedCheck.leftPadding
                            y: parent.height / 2 - height / 2
                            radius: 2
                            color: colourWriteRedCheck.checked ? MaterialEditorQML.accentColor : MaterialEditorQML.panelColor
                            border.color: MaterialEditorQML.borderColor
                            border.width: 1
                            
                            Rectangle {
                                width: 6
                                height: 6
                                x: 5
                                y: 5
                                radius: 1
                                color: MaterialEditorQML.textColor
                                visible: colourWriteRedCheck.checked
                            }
                        }
                        contentItem: Text {
                            text: colourWriteRedCheck.text
                            font: colourWriteRedCheck.font
                            color: MaterialEditorQML.textColor
                            verticalAlignment: Text.AlignVCenter
                            leftPadding: colourWriteRedCheck.indicator.width + colourWriteRedCheck.spacing
                        }
                    }
                    
                    CheckBox {
                        id: colourWriteGreenCheck
                        text: "Green"
                        checked: MaterialEditorQML.colourWriteGreen
                        onCheckedChanged: {
                            if (checked !== MaterialEditorQML.colourWriteGreen) {
                                MaterialEditorQML.colourWriteGreen = checked
                            }
                        }
                        Connections {
                            target: MaterialEditorQML
                            function onColourWriteGreenChanged() {
                                colourWriteGreenCheck.checked = MaterialEditorQML.colourWriteGreen
                            }
                        }
                        indicator: Rectangle {
                            implicitWidth: 16
                            implicitHeight: 16
                            x: colourWriteGreenCheck.leftPadding
                            y: parent.height / 2 - height / 2
                            radius: 2
                            color: colourWriteGreenCheck.checked ? MaterialEditorQML.accentColor : MaterialEditorQML.panelColor
                            border.color: MaterialEditorQML.borderColor
                            border.width: 1
                            
                            Rectangle {
                                width: 6
                                height: 6
                                x: 5
                                y: 5
                                radius: 1
                                color: MaterialEditorQML.textColor
                                visible: colourWriteGreenCheck.checked
                            }
                        }
                        contentItem: Text {
                            text: colourWriteGreenCheck.text
                            font: colourWriteGreenCheck.font
                            color: MaterialEditorQML.textColor
                            verticalAlignment: Text.AlignVCenter
                            leftPadding: colourWriteGreenCheck.indicator.width + colourWriteGreenCheck.spacing
                        }
                    }
                    
                    CheckBox {
                        id: colourWriteBlueCheck
                        text: "Blue"
                        checked: MaterialEditorQML.colourWriteBlue
                        onCheckedChanged: {
                            if (checked !== MaterialEditorQML.colourWriteBlue) {
                                MaterialEditorQML.colourWriteBlue = checked
                            }
                        }
                        Connections {
                            target: MaterialEditorQML
                            function onColourWriteBlueChanged() {
                                colourWriteBlueCheck.checked = MaterialEditorQML.colourWriteBlue
                            }
                        }
                        indicator: Rectangle {
                            implicitWidth: 16
                            implicitHeight: 16
                            x: colourWriteBlueCheck.leftPadding
                            y: parent.height / 2 - height / 2
                            radius: 2
                            color: colourWriteBlueCheck.checked ? MaterialEditorQML.accentColor : MaterialEditorQML.panelColor
                            border.color: MaterialEditorQML.borderColor
                            border.width: 1
                            
                            Rectangle {
                                width: 6
                                height: 6
                                x: 5
                                y: 5
                                radius: 1
                                color: MaterialEditorQML.textColor
                                visible: colourWriteBlueCheck.checked
                            }
                        }
                        contentItem: Text {
                            text: colourWriteBlueCheck.text
                            font: colourWriteBlueCheck.font
                            color: MaterialEditorQML.textColor
                            verticalAlignment: Text.AlignVCenter
                            leftPadding: colourWriteBlueCheck.indicator.width + colourWriteBlueCheck.spacing
                        }
                    }
                    
                    CheckBox {
                        id: colourWriteAlphaCheck
                        text: "Alpha"
                        checked: MaterialEditorQML.colourWriteAlpha
                        onCheckedChanged: {
                            if (checked !== MaterialEditorQML.colourWriteAlpha) {
                                MaterialEditorQML.colourWriteAlpha = checked
                            }
                        }
                        Connections {
                            target: MaterialEditorQML
                            function onColourWriteAlphaChanged() {
                                colourWriteAlphaCheck.checked = MaterialEditorQML.colourWriteAlpha
                            }
                        }
                        indicator: Rectangle {
                            implicitWidth: 16
                            implicitHeight: 16
                            x: colourWriteAlphaCheck.leftPadding
                            y: parent.height / 2 - height / 2
                            radius: 2
                            color: colourWriteAlphaCheck.checked ? MaterialEditorQML.accentColor : MaterialEditorQML.panelColor
                            border.color: MaterialEditorQML.borderColor
                            border.width: 1
                            
                            Rectangle {
                                width: 6
                                height: 6
                                x: 5
                                y: 5
                                radius: 1
                                color: MaterialEditorQML.textColor
                                visible: colourWriteAlphaCheck.checked
                            }
                        }
                        contentItem: Text {
                            text: colourWriteAlphaCheck.text
                            font: colourWriteAlphaCheck.font
                            color: MaterialEditorQML.textColor
                            verticalAlignment: Text.AlignVCenter
                            leftPadding: colourWriteAlphaCheck.indicator.width + colourWriteAlphaCheck.spacing
                        }
                    }
                }
            }
        }
        
        // Blending & Effects Group
        GroupBox {
            title: "Blending & Effects"
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
                
                // Scene Blend Operation
                RowLayout {
                    ThemedLabel { text: "Blend Operation:" }
                    ThemedComboBox {
                        id: sceneBlendOperationCombo
                        Layout.fillWidth: true
                        model: MaterialEditorQML.getSceneBlendOperationNames()
                        currentIndex: MaterialEditorQML.sceneBlendOperation
                        onCurrentIndexChanged: {
                            if (currentIndex !== MaterialEditorQML.sceneBlendOperation) {
                                MaterialEditorQML.sceneBlendOperation = currentIndex
                            }
                        }
                        Connections {
                            target: MaterialEditorQML
                            function onSceneBlendOperationChanged() {
                                sceneBlendOperationCombo.currentIndex = MaterialEditorQML.sceneBlendOperation
                            }
                        }
                    }
                }
                
                // Point Size
                RowLayout {
                    ThemedLabel { text: "Point Size:" }
                    ThemedSpinBox {
                        id: pointSizeSpin
                        Layout.fillWidth: true
                        from: 1
                        to: 100
                        stepSize: 1
                        value: Math.round(MaterialEditorQML.pointSize)
                        onValueChanged: {
                            if (value !== Math.round(MaterialEditorQML.pointSize)) {
                                MaterialEditorQML.pointSize = value
                            }
                        }
                        Connections {
                            target: MaterialEditorQML
                            function onPointSizeChanged() {
                                pointSizeSpin.value = Math.round(MaterialEditorQML.pointSize)
                            }
                        }
                    }
                }
                
                // Line Width
                RowLayout {
                    ThemedLabel { text: "Line Width:" }
                    ThemedSpinBox {
                        id: lineWidthSpin
                        Layout.fillWidth: true
                        from: 1
                        to: 100
                        stepSize: 1
                        value: Math.round(MaterialEditorQML.lineWidth)
                        onValueChanged: {
                            if (value !== Math.round(MaterialEditorQML.lineWidth)) {
                                MaterialEditorQML.lineWidth = value
                            }
                        }
                        Connections {
                            target: MaterialEditorQML
                            function onLineWidthChanged() {
                                lineWidthSpin.value = Math.round(MaterialEditorQML.lineWidth)
                            }
                        }
                    }
                }
                
                // Point Sprites
                CheckBox {
                    id: pointSpritesCheck
                    text: "Point Sprites"
                    checked: MaterialEditorQML.pointSpritesEnabled
                    onCheckedChanged: {
                        if (checked !== MaterialEditorQML.pointSpritesEnabled) {
                            MaterialEditorQML.pointSpritesEnabled = checked
                        }
                    }
                    Connections {
                        target: MaterialEditorQML
                        function onPointSpritesEnabledChanged() {
                            pointSpritesCheck.checked = MaterialEditorQML.pointSpritesEnabled
                        }
                    }
                    indicator: Rectangle {
                        implicitWidth: 16
                        implicitHeight: 16
                        x: pointSpritesCheck.leftPadding
                        y: parent.height / 2 - height / 2
                        radius: 2
                        color: pointSpritesCheck.checked ? MaterialEditorQML.accentColor : MaterialEditorQML.panelColor
                        border.color: MaterialEditorQML.borderColor
                        border.width: 1
                        
                        Rectangle {
                            width: 6
                            height: 6
                            x: 5
                            y: 5
                            radius: 1
                            color: MaterialEditorQML.textColor
                            visible: pointSpritesCheck.checked
                        }
                    }
                    contentItem: Text {
                        text: pointSpritesCheck.text
                        font: pointSpritesCheck.font
                        color: MaterialEditorQML.textColor
                        verticalAlignment: Text.AlignVCenter
                        leftPadding: pointSpritesCheck.indicator.width + pointSpritesCheck.spacing
                    }
                }
            }
        }
        
        // Lighting Control Group
        GroupBox {
            title: "Lighting Control"
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
                
                // Max Lights
                RowLayout {
                    ThemedLabel { text: "Max Lights (0=unlimited):" }
                    ThemedSpinBox {
                        id: maxLightsSpin
                        Layout.fillWidth: true
                        from: 0
                        to: 8
                        value: MaterialEditorQML.maxLights
                        onValueChanged: {
                            if (value !== MaterialEditorQML.maxLights) {
                                MaterialEditorQML.maxLights = value
                            }
                        }
                        Connections {
                            target: MaterialEditorQML
                            function onMaxLightsChanged() {
                                maxLightsSpin.value = MaterialEditorQML.maxLights
                            }
                        }
                    }
                }
                
                // Start Light
                RowLayout {
                    ThemedLabel { text: "Start Light:" }
                    ThemedSpinBox {
                        id: startLightSpin
                        Layout.fillWidth: true
                        from: 0
                        to: 7
                        value: MaterialEditorQML.startLight
                        onValueChanged: {
                            if (value !== MaterialEditorQML.startLight) {
                                MaterialEditorQML.startLight = value
                            }
                        }
                        Connections {
                            target: MaterialEditorQML
                            function onStartLightChanged() {
                                startLightSpin.value = MaterialEditorQML.startLight
                            }
                        }
                    }
                }
            }
        }
        
        // Fog Properties Group
        GroupBox {
            title: "Fog Properties"
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
                
                // Fog Override
                CheckBox {
                    id: fogOverrideCheck
                    text: "Override Fog Settings"
                    checked: MaterialEditorQML.fogOverride
                    onCheckedChanged: {
                        if (checked !== MaterialEditorQML.fogOverride) {
                            MaterialEditorQML.fogOverride = checked
                        }
                    }
                    Connections {
                        target: MaterialEditorQML
                        function onFogOverrideChanged() {
                            fogOverrideCheck.checked = MaterialEditorQML.fogOverride
                        }
                    }
                    indicator: Rectangle {
                        implicitWidth: 16
                        implicitHeight: 16
                        x: fogOverrideCheck.leftPadding
                        y: parent.height / 2 - height / 2
                        radius: 2
                        color: fogOverrideCheck.checked ? MaterialEditorQML.accentColor : MaterialEditorQML.panelColor
                        border.color: MaterialEditorQML.borderColor
                        border.width: 1
                        
                        Rectangle {
                            width: 6
                            height: 6
                            x: 5
                            y: 5
                            radius: 1
                            color: MaterialEditorQML.textColor
                            visible: fogOverrideCheck.checked
                        }
                    }
                    contentItem: Text {
                        text: fogOverrideCheck.text
                        font: fogOverrideCheck.font
                        color: MaterialEditorQML.textColor
                        verticalAlignment: Text.AlignVCenter
                        leftPadding: fogOverrideCheck.indicator.width + fogOverrideCheck.spacing
                    }
                }
                
                // Fog Mode
                RowLayout {
                    enabled: MaterialEditorQML.fogOverride
                    ThemedLabel { 
                        text: "Fog Mode:" 
                        color: enabled ? MaterialEditorQML.textColor : MaterialEditorQML.disabledTextColor
                    }
                    ThemedComboBox {
                        id: fogModeCombo
                        Layout.fillWidth: true
                        enabled: MaterialEditorQML.fogOverride
                        model: MaterialEditorQML.getFogModeNames()
                        currentIndex: MaterialEditorQML.fogMode
                        onCurrentIndexChanged: {
                            if (currentIndex !== MaterialEditorQML.fogMode) {
                                MaterialEditorQML.fogMode = currentIndex
                            }
                        }
                        Connections {
                            target: MaterialEditorQML
                            function onFogModeChanged() {
                                fogModeCombo.currentIndex = MaterialEditorQML.fogMode
                            }
                        }
                    }
                }
                
                // Fog Color
                RowLayout {
                    enabled: MaterialEditorQML.fogOverride
                    ThemedLabel { 
                        text: "Fog Color:" 
                        color: enabled ? MaterialEditorQML.textColor : MaterialEditorQML.disabledTextColor
                    }
                    Rectangle {
                        id: fogColorRect
                        width: 30
                        height: 20
                        color: MaterialEditorQML.fogColor
                        border.color: MaterialEditorQML.borderColor
                        border.width: 1
                        radius: 2
                        enabled: MaterialEditorQML.fogOverride
                        
                        MouseArea {
                            anchors.fill: parent
                            enabled: MaterialEditorQML.fogOverride
                            onClicked: {
                                console.log("Fog color rectangle clicked")
                                colorPickerPopup.openForColor("fog", MaterialEditorQML.fogColor)
                            }
                        }
                    }
                }
                
                // Fog Density (for exponential fog modes)
                RowLayout {
                    enabled: MaterialEditorQML.fogOverride && MaterialEditorQML.fogMode > 0 && MaterialEditorQML.fogMode < 3
                    ThemedLabel { 
                        text: "Fog Density:" 
                        color: enabled ? MaterialEditorQML.textColor : MaterialEditorQML.disabledTextColor
                    }
                    ThemedSpinBox {
                        id: fogDensitySpin
                        Layout.fillWidth: true
                        enabled: MaterialEditorQML.fogOverride && MaterialEditorQML.fogMode > 0 && MaterialEditorQML.fogMode < 3
                        from: 0
                        to: 10000
                        stepSize: 1
                        value: Math.round(MaterialEditorQML.fogDensity * 1000)
                        onValueChanged: {
                            if (value !== Math.round(MaterialEditorQML.fogDensity * 1000)) {
                                MaterialEditorQML.fogDensity = value / 1000.0
                            }
                        }
                        Connections {
                            target: MaterialEditorQML
                            function onFogDensityChanged() {
                                fogDensitySpin.value = Math.round(MaterialEditorQML.fogDensity * 1000)
                            }
                        }
                    }
                }
                
                // Fog Start (for linear fog)
                RowLayout {
                    enabled: MaterialEditorQML.fogOverride && MaterialEditorQML.fogMode === 3
                    ThemedLabel { 
                        text: "Fog Start:" 
                        color: enabled ? MaterialEditorQML.textColor : MaterialEditorQML.disabledTextColor
                    }
                    ThemedSpinBox {
                        id: fogStartSpin
                        Layout.fillWidth: true
                        enabled: MaterialEditorQML.fogOverride && MaterialEditorQML.fogMode === 3
                        from: 0
                        to: 100000
                        stepSize: 1
                        value: Math.round(MaterialEditorQML.fogStart)
                        onValueChanged: {
                            if (value !== Math.round(MaterialEditorQML.fogStart)) {
                                MaterialEditorQML.fogStart = value
                            }
                        }
                        Connections {
                            target: MaterialEditorQML
                            function onFogStartChanged() {
                                fogStartSpin.value = Math.round(MaterialEditorQML.fogStart)
                            }
                        }
                    }
                }
                
                // Fog End (for linear fog)
                RowLayout {
                    enabled: MaterialEditorQML.fogOverride && MaterialEditorQML.fogMode === 3
                    ThemedLabel { 
                        text: "Fog End:" 
                        color: enabled ? MaterialEditorQML.textColor : MaterialEditorQML.disabledTextColor
                    }
                    ThemedSpinBox {
                        id: fogEndSpin
                        Layout.fillWidth: true
                        enabled: MaterialEditorQML.fogOverride && MaterialEditorQML.fogMode === 3
                        from: 1
                        to: 100000
                        stepSize: 1
                        value: Math.round(MaterialEditorQML.fogEnd)
                        onValueChanged: {
                            if (value !== Math.round(MaterialEditorQML.fogEnd)) {
                                MaterialEditorQML.fogEnd = value
                            }
                        }
                        Connections {
                            target: MaterialEditorQML
                            function onFogEndChanged() {
                                fogEndSpin.value = Math.round(MaterialEditorQML.fogEnd)
                            }
                        }
                    }
                }
            }
        }
    }

    // Fog Color Picker Popup
    Popup {
        id: fogColorPicker
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
                text: "Select Fog Color"
                font.bold: true
                Layout.alignment: Qt.AlignHCenter
            }
            
            // Color preview
            Rectangle {
                id: fogColorPreview
                Layout.fillWidth: true
                height: 40
                color: MaterialEditorQML.fogColor
                border.color: MaterialEditorQML.borderColor
                border.width: 1
                radius: 4
            }
            
            // RGB sliders
            GridLayout {
                columns: 2
                Layout.fillWidth: true
                
                ThemedLabel { text: "Red:" }
                Slider {
                    id: fogRedSlider
                    Layout.fillWidth: true
                    from: 0
                    to: 255
                    value: MaterialEditorQML.fogColor.r * 255
                    onValueChanged: {
                        var newColor = Qt.rgba(value/255, MaterialEditorQML.fogColor.g, MaterialEditorQML.fogColor.b, 1.0)
                        MaterialEditorQML.fogColor = newColor
                    }
                    background: Rectangle {
                        x: fogRedSlider.leftPadding
                        y: fogRedSlider.topPadding + fogRedSlider.availableHeight / 2 - height / 2
                        implicitWidth: 200
                        implicitHeight: 4
                        width: fogRedSlider.availableWidth
                        height: implicitHeight
                        radius: 2
                        color: MaterialEditorQML.borderColor
                    }
                    handle: Rectangle {
                        x: fogRedSlider.leftPadding + fogRedSlider.visualPosition * (fogRedSlider.availableWidth - width)
                        y: fogRedSlider.topPadding + fogRedSlider.availableHeight / 2 - height / 2
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
                    id: fogGreenSlider
                    Layout.fillWidth: true
                    from: 0
                    to: 255
                    value: MaterialEditorQML.fogColor.g * 255
                    onValueChanged: {
                        var newColor = Qt.rgba(MaterialEditorQML.fogColor.r, value/255, MaterialEditorQML.fogColor.b, 1.0)
                        MaterialEditorQML.fogColor = newColor
                    }
                    background: Rectangle {
                        x: fogGreenSlider.leftPadding
                        y: fogGreenSlider.topPadding + fogGreenSlider.availableHeight / 2 - height / 2
                        implicitWidth: 200
                        implicitHeight: 4
                        width: fogGreenSlider.availableWidth
                        height: implicitHeight
                        radius: 2
                        color: MaterialEditorQML.borderColor
                    }
                    handle: Rectangle {
                        x: fogGreenSlider.leftPadding + fogGreenSlider.visualPosition * (fogGreenSlider.availableWidth - width)
                        y: fogGreenSlider.topPadding + fogGreenSlider.availableHeight / 2 - height / 2
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
                    id: fogBlueSlider
                    Layout.fillWidth: true
                    from: 0
                    to: 255
                    value: MaterialEditorQML.fogColor.b * 255
                    onValueChanged: {
                        var newColor = Qt.rgba(MaterialEditorQML.fogColor.r, MaterialEditorQML.fogColor.g, value/255, 1.0)
                        MaterialEditorQML.fogColor = newColor
                    }
                    background: Rectangle {
                        x: fogBlueSlider.leftPadding
                        y: fogBlueSlider.topPadding + fogBlueSlider.availableHeight / 2 - height / 2
                        implicitWidth: 200
                        implicitHeight: 4
                        width: fogBlueSlider.availableWidth
                        height: implicitHeight
                        radius: 2
                        color: MaterialEditorQML.borderColor
                    }
                    handle: Rectangle {
                        x: fogBlueSlider.leftPadding + fogBlueSlider.visualPosition * (fogBlueSlider.availableWidth - width)
                        y: fogBlueSlider.topPadding + fogBlueSlider.availableHeight / 2 - height / 2
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
                    onClicked: fogColorPicker.close()
                }
            }
        }
    }
} 