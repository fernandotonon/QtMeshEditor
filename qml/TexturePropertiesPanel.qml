import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MaterialEditorQML 1.0

GroupBox {
    id: root
    title: "Texture Properties"

    // Issue #403: dispatch texture generation — mesh-conditioned
    // when the "use selected mesh" checkbox is ticked, plain
    // txt2img otherwise. Shared by the prompt field's Enter key and
    // the Generate button.
    function runTextureGeneration() {
        if (sdPromptField.text.trim().length === 0) return
        // hasSelectedMesh is exposed as a Q_PROPERTY, not a callable — read it
        // as a property. Calling it as MaterialEditorQML.hasSelectedMesh()
        // throws "not a function" and silently aborted the whole handler, which
        // is why mesh-conditioned generation never started.
        var useMesh = useMeshCheck.checked && MaterialEditorQML.hasSelectedMesh
        var w = sdWidthSpin.value
        var h = sdHeightSpin.value
        if (useMesh && multiViewCheck.checked) {
            // Front+back projection bake onto the mesh's UV0 (slice 2).
            MaterialEditorQML.generateMeshTextureMultiView(
                sdPromptField.text, w, h, meshStrengthSlider.value, ["front", "back"])
        } else if (useMesh) {
            MaterialEditorQML.generateMeshTextureFromPrompt(
                sdPromptField.text, w, h, meshStrengthSlider.value)
        } else {
            MaterialEditorQML.generateTextureFromPrompt(sdPromptField.text, w, h)
        }
    }

    // Slice I: flat Inspector-style header.
    topPadding: 22
    leftPadding: 6
    rightPadding: 6
    bottomPadding: 6
    background: Rectangle {
        color: "transparent"
        Rectangle {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 1
            color: borderColor
        }
    }
    label: ThemedLabel {
        text: parent.title
        font.bold: true
        topPadding: 4
    }
    
    Component.onCompleted: {
        console.log("TexturePropertiesPanel: loaded successfully")
    }

    // Slice I: align local colors with MaterialEditorQML so the
    // GroupBox surfaces match the outer Material Editor pane.
    readonly property color backgroundColor: MaterialEditorQML.backgroundColor
    readonly property color panelColor: MaterialEditorQML.panelColor
    readonly property color textColor: MaterialEditorQML.textColor
    readonly property color borderColor: MaterialEditorQML.borderColor
    readonly property color highlightColor: MaterialEditorQML.highlightColor
    readonly property color buttonColor: MaterialEditorQML.buttonColor
    readonly property color buttonTextColor: MaterialEditorQML.buttonTextColor
    readonly property color disabledTextColor: MaterialEditorQML.disabledTextColor
    
    ColumnLayout {
        anchors.fill: parent
        spacing: 15
        
        // Texture Selection
        GroupBox {
            title: "Texture Selection"
            Layout.fillWidth: true
            // Slice I: flat Inspector-style header.
            topPadding: 22
            leftPadding: 6
            rightPadding: 6
            bottomPadding: 6
            background: Rectangle {
                color: "transparent"
                Rectangle {
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    height: 1
                    color: borderColor
                }
            }
            label: ThemedLabel {
                text: parent.title
                font.bold: true
                topPadding: 4
            }

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
                            var selectedPath = MaterialEditorQML.openFileDialog()
                            if (selectedPath !== "") {
                                MaterialEditorQML.loadTextureFile(selectedPath)
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
            // Slice I: flat Inspector-style header.
            topPadding: 22
            leftPadding: 6
            rightPadding: 6
            bottomPadding: 6
            background: Rectangle {
                color: "transparent"
                Rectangle {
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    height: 1
                    color: borderColor
                }
            }
            label: ThemedLabel {
                text: parent.title
                font.bold: true
                topPadding: 4
            }
            
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
                    // Driven imperatively via reloadPreview() — NOT a binding.
                    // A `source:` binding on getTexturePreviewPath() re-evaluates
                    // the getter twice per change (once for the !=="" test, once
                    // for the value); combined with reloadPreview() that re-ran
                    // the GPU readback + preview-PNG rewrite several times per
                    // switch and crashed under rapid switching. The getter is now
                    // memoized C++-side, and we call it once here.
                    source: ""

                    // Cache-bust counter for forcing image reload
                    property int cacheBuster: 0

                    Component.onCompleted: reloadPreview()

                    function reloadPreview() {
                        var path = MaterialEditorQML.getTexturePreviewPath()
                        if (path !== "") {
                            cacheBuster++
                            texturePreview.visible = true
                            texturePreview.source = path + "?v=" + cacheBuster
                        } else {
                            // No resolvable preview — clearing source leaves
                            // status at Image.Null (no statusChanged for Error),
                            // so explicitly fall back to the placeholder here.
                            texturePreview.source = ""
                            texturePreview.visible = false
                            placeholderText.visible = true
                        }
                    }

                    // Update source when texture name changes
                    Connections {
                        target: MaterialEditorQML
                        function onTextureNameChanged() {
                            texturePreview.reloadPreview()
                        }
                        function onSdTextureGenerated(filePath) {
                            // Force reload after SD generation (same filename, new content)
                            texturePreview.reloadPreview()
                        }
                    }

                    onStatusChanged: {
                        if (status === Image.Null || status === Image.Error) {
                            // Null (empty source) or a failed load → placeholder.
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

        // Export the currently-previewed texture to disk (issue #403
        // escape hatch — lets the user save a generated texture and
        // apply it via other tools even when in-app application of
        // the texture to certain materials isn't working yet).
        RowLayout {
            Layout.fillWidth: true
            // #404: synthesize normal/roughness/height from the current diffuse.
            // Only shown on an ONNX build; disabled until a real texture is set.
            ThemedButton {
                id: pbrSynthBtn
                text: "Generate PBR maps from diffuse"
                visible: MaterialEditorQML.aiPbrAvailable()
                enabled: MaterialEditorQML.textureName !== ""
                    && MaterialEditorQML.textureName !== "*Select a texture*"
                onClicked: {
                    pbrStatus.text = "Synthesizing PBR maps…"
                    MaterialEditorQML.generatePbrFromDiffuse()
                }
            }
            ThemedLabel {
                id: pbrStatus
                Layout.fillWidth: true
                visible: MaterialEditorQML.aiPbrAvailable()
                text: ""
                elide: Text.ElideRight
            }
            Item { Layout.fillWidth: true; visible: !MaterialEditorQML.aiPbrAvailable() }
            ThemedButton {
                text: "Save Texture As…"
                // Bind to textureName (a NOTIFY property) so the
                // enabled state tracks selection changes — a plain
                // getTexturePreviewPath() call would only evaluate
                // once and never update. Enabled whenever a real
                // texture is set.
                enabled: MaterialEditorQML.textureName !== ""
                    && MaterialEditorQML.textureName !== "*Select a texture*"
                onClicked: {
                    const dest = MaterialEditorQML.chooseTextureExportPath()
                    if (dest && dest.length > 0)
                        MaterialEditorQML.exportCurrentTexture(dest)
                }
            }

            Connections {
                target: MaterialEditorQML
                function onPbrSynthCompleted(result) {
                    pbrStatus.text = result.fromCache ? "PBR maps ready (cached)."
                                                      : "PBR maps generated."
                }
                function onPbrSynthError(err) { pbrStatus.text = "PBR: " + err }
            }
        }

        // Texture Coordinates Group
        GroupBox {
            title: "Texture Coordinates"
            // Slice I: flat Inspector-style header.
            topPadding: 22
            leftPadding: 6
            rightPadding: 6
            bottomPadding: 6
            background: Rectangle {
                color: "transparent"
                Rectangle {
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    height: 1
                    color: MaterialEditorQML.borderColor
                }
            }
            label: ThemedLabel {
                text: parent.title
                font.bold: true
                topPadding: 4
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
            // Slice I: flat Inspector-style header.
            topPadding: 22
            leftPadding: 6
            rightPadding: 6
            bottomPadding: 6
            background: Rectangle {
                color: "transparent"
                Rectangle {
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    height: 1
                    color: MaterialEditorQML.borderColor
                }
            }
            label: ThemedLabel {
                text: parent.title
                font.bold: true
                topPadding: 4
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
            // Slice I: flat Inspector-style header.
            topPadding: 22
            leftPadding: 6
            rightPadding: 6
            bottomPadding: 6
            background: Rectangle {
                color: "transparent"
                Rectangle {
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    height: 1
                    color: MaterialEditorQML.borderColor
                }
            }
            label: ThemedLabel {
                text: parent.title
                font.bold: true
                topPadding: 4
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
            // Slice I: flat Inspector-style header.
            topPadding: 22
            leftPadding: 6
            rightPadding: 6
            bottomPadding: 6
            background: Rectangle {
                color: "transparent"
                Rectangle {
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    height: 1
                    color: MaterialEditorQML.borderColor
                }
            }
            label: ThemedLabel {
                text: parent.title
                font.bold: true
                topPadding: 4
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
            // Slice I: flat Inspector-style header.
            topPadding: 22
            leftPadding: 6
            rightPadding: 6
            bottomPadding: 6
            background: Rectangle {
                color: "transparent"
                Rectangle {
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    height: 1
                    color: MaterialEditorQML.borderColor
                }
            }
            label: ThemedLabel {
                text: parent.title
                font.bold: true
                topPadding: 4
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
                // Non-fatal notice (e.g. degraded mesh-conditioning). Shown in
                // the same banner, but generation continues — so we do NOT
                // touch the generating/progress state here.
                function onSdGenerationNotice(message) {
                    sdErrorText.text = message
                    sdErrorRect.visible = true
                    sdErrorTimer.restart()
                }
            }
        }

        // AI Texture Generation
        GroupBox {
            title: "AI Texture Generation"
            visible: MaterialEditorQML.stableDiffusionEnabled
            Layout.fillWidth: true

            // Slice I: flat Inspector-style header.
            topPadding: 22
            leftPadding: 6
            rightPadding: 6
            bottomPadding: 6
            background: Rectangle {
                color: "transparent"
                Rectangle {
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    height: 1
                    color: MaterialEditorQML.borderColor
                }
            }
            label: ThemedLabel {
                text: parent.title
                font.bold: true
                topPadding: 4
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
                                root.runTextureGeneration()
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
                                root.runTextureGeneration()
                            }
                        }
                    }
                }

                // Issue #403: mesh-aware generation toggle. When
                // checked, the selected mesh's depth map conditions
                // generation (ControlNet) so the texture follows its
                // shape. Disabled when no mesh is selected.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    ThemedCheckBox {
                        id: useMeshCheck
                        text: "Use selected mesh (depth-conditioned)"
                        enabled: MaterialEditorQML.sdModelLoaded
                            && !MaterialEditorQML.sdIsGenerating
                            && MaterialEditorQML.hasSelectedMesh
                        checked: false
                        // Drop the checked state only when the mesh selection
                        // itself is gone (so a stale check can't drive mesh
                        // generation). Do NOT key this off `enabled`: the box is
                        // also disabled while generating, and clearing it then
                        // would uncheck the user's choice the moment generation
                        // starts.
                        Connections {
                            target: MaterialEditorQML
                            function onHasSelectedMeshChanged() {
                                if (!MaterialEditorQML.hasSelectedMesh)
                                    useMeshCheck.checked = false
                            }
                        }
                    }
                }

                // Slice 2: multi-view bake. When on (and mesh conditioning is
                // enabled), generate front + back images and projection-bake
                // them onto the mesh's UV0 atlas instead of a single planar
                // view. Only meaningful with "use selected mesh" checked.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    visible: useMeshCheck.checked && useMeshCheck.enabled
                    ThemedCheckBox {
                        id: multiViewCheck
                        text: "Bake front + back (projects onto UV map)"
                        enabled: !MaterialEditorQML.sdIsGenerating
                        checked: false
                    }
                }

                // ControlNet status + strength — only relevant when
                // mesh conditioning is on.
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4
                    visible: useMeshCheck.checked && useMeshCheck.enabled

                    ThemedLabel {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        font.pointSize: 8
                        text: MaterialEditorQML.discoveredControlNetDepthPath() !== ""
                            ? "✓ ControlNet depth model found — generation will follow the mesh shape."
                            : "⚠ No ControlNet depth model in the models folder. It'll generate without shape conditioning. Download \"ControlNet Depth (SD 1.5)\" in AI Settings."
                        color: MaterialEditorQML.discoveredControlNetDepthPath() !== ""
                            ? "#4caf50" : "#e0a030"
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        ThemedLabel { text: "Shape strength:"; font.pointSize: 9 }
                        Slider {
                            id: meshStrengthSlider
                            Layout.fillWidth: true
                            from: 0; to: 1; value: 0.9
                        }
                        ThemedLabel {
                            text: (Math.round(meshStrengthSlider.value * 100) / 100).toFixed(2)
                            font.pointSize: 9
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
            // Slice I: flat Inspector-style header.
            topPadding: 22
            leftPadding: 6
            rightPadding: 6
            bottomPadding: 6
            background: Rectangle {
                color: "transparent"
                Rectangle {
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    height: 1
                    color: borderColor
                }
            }
            label: ThemedLabel {
                text: parent.title
                font.bold: true
                topPadding: 4
            }

            ColumnLayout {
                anchors.fill: parent
                spacing: 5

                ThemedLabel {
                    text: "Texture: " + (MaterialEditorQML.textureName || "None")
                    font.pointSize: 10
                }

                ThemedLabel {
                    text: texturePreview.source != "" && texturePreview.status === Image.Ready ?
                          "Size: " + texturePreview.sourceSize.width + " x " + texturePreview.sourceSize.height :
                          "Size: Unknown"
                    font.pointSize: 10
                }

                ThemedLabel {
                    text: "Animation: " +
                          (MaterialEditorQML.scrollAnimUSpeed != 0.0 || MaterialEditorQML.scrollAnimVSpeed != 0.0 ?
                           "Enabled (" + MaterialEditorQML.scrollAnimUSpeed.toFixed(2) + ", " + MaterialEditorQML.scrollAnimVSpeed.toFixed(2) + ")" :
                           "Disabled")
                    font.pointSize: 10
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