import QtQuick 6.0
import QtQuick.Controls 6.0
import QtQuick.Layouts 6.0
import MaterialEditorQML 1.0

GroupBox {
    title: "Texture Properties"
    
    // Apply theme colors to GroupBox
    background: Rectangle {
        color: panelColor
        border.color: borderColor
        border.width: 1
        radius: 4
    }
    
    label: Label {
        text: parent.title
        color: textColor
        font.bold: true
        x: parent.leftPadding
        width: parent.availableWidth
    }
    
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
                        onClicked: {
                            var selectedFileName = MaterialEditorQML.openFileDialog()
                            if (selectedFileName !== "") {
                                MaterialEditorQML.setTextureName(selectedFileName)
                            }
                        }
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
                        property var availableTextures: ["-- Select from available textures --"].concat(MaterialEditorQML.getAvailableTextures())
                        model: availableTextures
                        currentIndex: 0
                        onActivated: function(index) {
                            if (index > 0) { // Skip the placeholder at index 0
                                var selectedTexture = availableTextures[index]
                                console.log("Selected texture from combo:", selectedTexture)
                                MaterialEditorQML.setTextureName(selectedTexture)
                                textureNameField.text = selectedTexture
                                currentIndex = 0 // Reset to placeholder
                            }
                        }
                        
                        // Update the list when textures change
                        Connections {
                            target: MaterialEditorQML
                            function onMaterialNameChanged() {
                                availableTexturesCombo.availableTextures = ["-- Select from available textures --"].concat(MaterialEditorQML.getAvailableTextures())
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
        
        // SD Error Display
        Rectangle {
            id: sdErrorRect
            Layout.fillWidth: true
            height: sdErrorText.implicitHeight + 20
            color: "#ffebee"
            border.color: "#ef5350"
            border.width: 1
            radius: 4
            visible: false

            Text {
                id: sdErrorText
                anchors.fill: parent
                anchors.margins: 10
                wrapMode: Text.WordWrap
                color: "#c62828"
                font.pointSize: 9
            }

            Timer {
                id: sdErrorTimer
                interval: 5000
                onTriggered: sdErrorRect.visible = false
            }

            Connections {
                target: MaterialEditorQML
                function onSdGenerationError(error) {
                    sdErrorText.text = error
                    sdErrorRect.visible = true
                    sdErrorTimer.restart()
                }
            }
        }

        // AI Texture Edit (img2img)
        GroupBox {
            title: "AI Texture Edit"
            visible: MaterialEditorQML.stableDiffusionEnabled
            Layout.fillWidth: true

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

                ThemedLabel {
                    text: "Edit the current texture with AI"
                    font.pointSize: 9
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    ThemedTextField {
                        id: sdEditPromptField
                        Layout.fillWidth: true
                        placeholderText: "Describe the change (e.g., 'change eyes to blue')..."
                        enabled: MaterialEditorQML.sdModelLoaded && !MaterialEditorQML.sdIsGenerating

                        Keys.onReturnPressed: {
                            if (text.length > 0 && MaterialEditorQML.sdModelLoaded && !MaterialEditorQML.sdIsGenerating) {
                                MaterialEditorQML.editTextureFromPrompt(text, sdStrengthSlider.value / 100.0)
                            }
                        }
                    }

                    ThemedButton {
                        text: MaterialEditorQML.sdIsGenerating ? "Stop" : "Edit"
                        enabled: MaterialEditorQML.sdModelLoaded &&
                                 (MaterialEditorQML.sdIsGenerating || sdEditPromptField.text.length > 0)
                        onClicked: {
                            if (MaterialEditorQML.sdIsGenerating) {
                                MaterialEditorQML.stopTextureGeneration()
                            } else {
                                MaterialEditorQML.editTextureFromPrompt(sdEditPromptField.text, sdStrengthSlider.value / 100.0)
                            }
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    ThemedLabel { text: "Strength:" }
                    Slider {
                        id: sdStrengthSlider
                        Layout.fillWidth: true
                        from: 10
                        to: 100
                        stepSize: 5
                        value: 50

                        background: Rectangle {
                            x: sdStrengthSlider.leftPadding
                            y: sdStrengthSlider.topPadding + sdStrengthSlider.availableHeight / 2 - height / 2
                            implicitWidth: 200
                            implicitHeight: 4
                            width: sdStrengthSlider.availableWidth
                            height: implicitHeight
                            radius: 2
                            color: MaterialEditorQML.borderColor
                        }
                        handle: Rectangle {
                            x: sdStrengthSlider.leftPadding + sdStrengthSlider.visualPosition * (sdStrengthSlider.availableWidth - width)
                            y: sdStrengthSlider.topPadding + sdStrengthSlider.availableHeight / 2 - height / 2
                            implicitWidth: 16
                            implicitHeight: 16
                            radius: 8
                            color: MaterialEditorQML.accentColor
                            border.color: MaterialEditorQML.borderColor
                        }
                    }
                    ThemedLabel {
                        text: Math.round(sdStrengthSlider.value) + "%"
                        Layout.minimumWidth: 35
                    }
                }

                ThemedLabel {
                    text: "Low = subtle changes, High = major changes"
                    font.pointSize: 8
                    opacity: 0.6
                }

                ProgressBar {
                    Layout.fillWidth: true
                    from: 0
                    to: 1
                    value: MaterialEditorQML.sdGenerationProgress
                    visible: MaterialEditorQML.sdIsGenerating
                }
            }
        }

        // AI Texture Generation
        GroupBox {
            title: "AI Texture Generation"
            visible: MaterialEditorQML.stableDiffusionEnabled
            Layout.fillWidth: true

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

                // SD Model Status
                RowLayout {
                    spacing: 8

                    Rectangle {
                        width: 10
                        height: 10
                        radius: 5
                        color: MaterialEditorQML.sdModelLoaded ? "#4caf50" : "#9e9e9e"
                    }

                    ThemedLabel {
                        text: MaterialEditorQML.sdModelLoaded ?
                              "SD Model loaded" : "No SD model loaded"
                        font.pointSize: 9
                    }
                }

                // Prompt input
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    ThemedTextField {
                        id: sdPromptField
                        Layout.fillWidth: true
                        placeholderText: "Describe the texture (e.g., 'rusty metal surface')..."
                        enabled: MaterialEditorQML.sdModelLoaded && !MaterialEditorQML.sdIsGenerating

                        Keys.onReturnPressed: {
                            if (text.length > 0 && MaterialEditorQML.sdModelLoaded && !MaterialEditorQML.sdIsGenerating) {
                                MaterialEditorQML.generateTextureFromPrompt(text, sdWidthSpin.value, sdHeightSpin.value)
                            }
                        }
                    }

                    ThemedButton {
                        text: MaterialEditorQML.sdIsGenerating ? "Stop" : "Generate"
                        enabled: MaterialEditorQML.sdModelLoaded &&
                                 (MaterialEditorQML.sdIsGenerating || sdPromptField.text.length > 0)
                        onClicked: {
                            if (MaterialEditorQML.sdIsGenerating) {
                                MaterialEditorQML.stopTextureGeneration()
                            } else {
                                MaterialEditorQML.generateTextureFromPrompt(sdPromptField.text, sdWidthSpin.value, sdHeightSpin.value)
                            }
                        }
                    }
                }

                // Size controls
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    ThemedLabel { text: "Width:" }
                    ThemedSpinBox {
                        id: sdWidthSpin
                        from: 256
                        to: 1024
                        stepSize: 128
                        value: 512
                    }

                    ThemedLabel { text: "Height:" }
                    ThemedSpinBox {
                        id: sdHeightSpin
                        from: 256
                        to: 1024
                        stepSize: 128
                        value: 512
                    }
                }

                // Progress bar
                ProgressBar {
                    Layout.fillWidth: true
                    from: 0
                    to: 1
                    value: MaterialEditorQML.sdGenerationProgress
                    visible: MaterialEditorQML.sdIsGenerating
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
        }
    }
} 