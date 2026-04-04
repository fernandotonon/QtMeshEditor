import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import PropertiesPanel 1.0
import AnimationControl 1.0

Rectangle {
    id: root
    color: PropertiesPanelController.panelColor

    ScrollView {
        anchors.fill: parent
        clip: true

        Column {
            width: root.width
            spacing: 0

            // ---- Scene Outliner ----
            CollapsibleSection {
                title: "Scene"
                expanded: true

                Component.onCompleted: content = sceneOutlinerComponent
            }

            // ---- Transform ----
            CollapsibleSection {
                title: "Transform"
                sectionVisible: PropertiesPanelController.hasSelection

                Component.onCompleted: content = transformComponent
            }

            // ---- Primitive Parameters ----
            CollapsibleSection {
                title: "Primitive: " + PropertiesPanelController.primitiveType
                sectionVisible: PropertiesPanelController.hasPrimitive

                Component.onCompleted: content = primitiveComponent
            }

            // ---- Animations ----
            CollapsibleSection {
                title: "Animations"
                sectionVisible: PropertiesPanelController.hasAnimations

                Component.onCompleted: content = animationComponent
            }

            // ---- Animation Control (keyframe editor) ----
            CollapsibleSection {
                title: "Animation Control"
                sectionVisible: AnimationControlController.hasAnimation
                expanded: false

                Component.onCompleted: content = animControlComponent
            }

            // ---- LOD Generation ----
            CollapsibleSection {
                title: "LOD Generation"
                sectionVisible: MeshLodController.hasSelection
                expanded: false

                Component.onCompleted: content = lodComponent
            }

            // ---- Mesh Validation ----
            CollapsibleSection {
                title: "Mesh Validation"
                sectionVisible: MeshValidator.hasSelection
                expanded: false

                Component.onCompleted: content = validationComponent
            }
        }
    }

    // ---- Scene Outliner Content ----
    Component {
        id: sceneOutlinerComponent

        Column {
            id: outlinerColumn
            width: parent ? parent.width : 200

            property var treeModel: PropertiesPanelController.sceneTreeModel
            property int nodeCount: treeModel ? treeModel.rowCount() : 0
            property bool delegatesActive: true

            Repeater {
                model: outlinerColumn.nodeCount

                Loader {
                    active: outlinerColumn.delegatesActive
                    width: outlinerColumn.width
                    source: "qrc:/PropertiesPanel/SceneTreeNode.qml"
                    onLoaded: {
                        item.nodeIndex = outlinerColumn.treeModel.index(index, 0)
                        item.treeModel = outlinerColumn.treeModel
                        item.indentLevel = 0
                        item.width = Qt.binding(function() { return outlinerColumn.width })
                        if (item.refreshSelected)
                            item.refreshSelected()
                    }
                }
            }

            Connections {
                target: outlinerColumn.treeModel
                function onModelReset() {
                    outlinerColumn.delegatesActive = false
                    outlinerColumn.nodeCount = outlinerColumn.treeModel
                        ? outlinerColumn.treeModel.rowCount() : 0
                    Qt.callLater(function() { outlinerColumn.delegatesActive = true })
                }
            }
        }
    }

    // ---- Transform Content ----
    Component {
        id: transformComponent

        Column {
            width: parent ? parent.width : 200
            padding: 8
            spacing: 6

            // Position
            Text {
                text: "Position"
                color: PropertiesPanelController.textColor
                font.pixelSize: 11
                font.bold: true
            }
            Row {
                spacing: 4
                width: parent.width - 16

                TransformField { label: "X"; value: PropertiesPanelController.posX; color: "#c04040"
                    onNewValue: function(val) { PropertiesPanelController.posX = val } }
                TransformField { label: "Y"; value: PropertiesPanelController.posY; color: "#40c040"
                    onNewValue: function(val) { PropertiesPanelController.posY = val } }
                TransformField { label: "Z"; value: PropertiesPanelController.posZ; color: "#4040c0"
                    onNewValue: function(val) { PropertiesPanelController.posZ = val } }
            }

            // Rotation
            Text {
                text: "Rotation"
                color: PropertiesPanelController.textColor
                font.pixelSize: 11
                font.bold: true
            }
            Row {
                spacing: 4
                width: parent.width - 16

                TransformField { label: "X"; value: PropertiesPanelController.rotX; color: "#c04040"
                    onNewValue: function(val) { PropertiesPanelController.rotX = val } }
                TransformField { label: "Y"; value: PropertiesPanelController.rotY; color: "#40c040"
                    onNewValue: function(val) { PropertiesPanelController.rotY = val } }
                TransformField { label: "Z"; value: PropertiesPanelController.rotZ; color: "#4040c0"
                    onNewValue: function(val) { PropertiesPanelController.rotZ = val } }
            }

            // Scale
            Text {
                text: "Scale"
                color: PropertiesPanelController.textColor
                font.pixelSize: 11
                font.bold: true
            }
            Row {
                spacing: 4
                width: parent.width - 16

                TransformField { label: "X"; value: PropertiesPanelController.scaleX; color: "#c04040"
                    onNewValue: function(val) { PropertiesPanelController.scaleX = val } }
                TransformField { label: "Y"; value: PropertiesPanelController.scaleY; color: "#40c040"
                    onNewValue: function(val) { PropertiesPanelController.scaleY = val } }
                TransformField { label: "Z"; value: PropertiesPanelController.scaleZ; color: "#4040c0"
                    onNewValue: function(val) { PropertiesPanelController.scaleZ = val } }
            }
        }
    }

    // ---- Primitive Content ----
    Component {
        id: primitiveComponent

        Column {
            width: parent ? parent.width : 200
            padding: 8
            spacing: 6

            property var rawCfg: PropertiesPanelController.primFieldConfig || {}
            property bool cfgShowSizeX: rawCfg.showSizeX || false
            property bool cfgShowSizeY: rawCfg.showSizeY || false
            property bool cfgShowSizeZ: rawCfg.showSizeZ || false
            property bool cfgShowRadius: rawCfg.showRadius || false
            property bool cfgShowRadius2: rawCfg.showRadius2 || false
            property bool cfgShowHeight: rawCfg.showHeight || false
            property bool cfgShowSegX: rawCfg.showSegX || false
            property bool cfgShowSegY: rawCfg.showSegY || false
            property bool cfgShowSegZ: rawCfg.showSegZ || false
            property bool cfgShowUV: rawCfg.showUV || false
            property var cfg: rawCfg

            // Size
            Text {
                visible: cfgShowSizeX || cfgShowSizeY || cfgShowSizeZ
                text: "Size"
                color: PropertiesPanelController.textColor
                font.pixelSize: 11; font.bold: true
            }
            Row {
                visible: cfgShowSizeX || cfgShowSizeY || cfgShowSizeZ
                spacing: 4; width: parent.width - 16

                TransformField { visible: cfgShowSizeX; label: "X"; value: PropertiesPanelController.primSizeX; color: "#c04040"; step: 0.1
                    onNewValue: function(val) { PropertiesPanelController.primSizeX = val } }
                TransformField { visible: cfgShowSizeY; label: "Y"; value: PropertiesPanelController.primSizeY; color: "#40c040"; step: 0.1
                    onNewValue: function(val) { PropertiesPanelController.primSizeY = val } }
                TransformField { visible: cfgShowSizeZ; label: "Z"; value: PropertiesPanelController.primSizeZ; color: "#4040c0"; step: 0.1
                    onNewValue: function(val) { PropertiesPanelController.primSizeZ = val } }
            }

            // Radius / Radius2 / Height
            Text {
                visible: cfgShowRadius || cfgShowRadius2 || cfgShowHeight
                text: {
                    var parts = []
                    if (cfg.showRadius) parts.push(cfg.radiusLabel || "Radius")
                    if (cfg.showRadius2) parts.push(cfg.radius2Label || "Radius2")
                    if (cfg.showHeight) parts.push("Height")
                    return parts.join(" / ")
                }
                color: PropertiesPanelController.textColor
                font.pixelSize: 11; font.bold: true
            }
            Row {
                visible: cfgShowRadius || cfgShowRadius2 || cfgShowHeight
                spacing: 4; width: parent.width - 16

                TransformField { visible: cfgShowRadius; label: (cfg.radiusLabel || "R").charAt(0); value: PropertiesPanelController.primRadius; color: "#c08040"; step: 0.1
                    onNewValue: function(val) { PropertiesPanelController.primRadius = val } }
                TransformField { visible: cfgShowRadius2; label: (cfg.radius2Label || "R2").charAt(0); value: PropertiesPanelController.primRadius2; color: "#c0a040"; step: 0.1
                    onNewValue: function(val) { PropertiesPanelController.primRadius2 = val } }
                TransformField { visible: cfgShowHeight; label: "H"; value: PropertiesPanelController.primHeight; color: "#40c0c0"; step: 0.1
                    onNewValue: function(val) { PropertiesPanelController.primHeight = val } }
            }

            // Segments
            Text {
                visible: cfgShowSegX || cfgShowSegY || cfgShowSegZ
                text: "Segments"
                color: PropertiesPanelController.textColor
                font.pixelSize: 11; font.bold: true
            }
            Row {
                visible: cfgShowSegX || cfgShowSegY || cfgShowSegZ
                spacing: 4; width: parent.width - 16

                TransformField { visible: cfgShowSegX; label: (cfg.segXLabel || "X").charAt(0); value: PropertiesPanelController.primSegX; color: "#808080"; step: 1
                    onNewValue: function(val) { PropertiesPanelController.primSegX = Math.max(1, Math.round(val)) } }
                TransformField { visible: cfgShowSegY; label: (cfg.segYLabel || "Y").charAt(0); value: PropertiesPanelController.primSegY; color: "#808080"; step: 1
                    onNewValue: function(val) { PropertiesPanelController.primSegY = Math.max(1, Math.round(val)) } }
                TransformField { visible: cfgShowSegZ; label: (cfg.segZLabel || "Z").charAt(0); value: PropertiesPanelController.primSegZ; color: "#808080"; step: 1
                    onNewValue: function(val) { PropertiesPanelController.primSegZ = Math.max(1, Math.round(val)) } }
            }

            // UV Tiling
            Text {
                visible: cfgShowUV
                text: "UV Tiling"
                color: PropertiesPanelController.textColor
                font.pixelSize: 11; font.bold: true
            }
            Row {
                visible: cfgShowUV
                spacing: 4; width: parent.width - 16

                TransformField { label: "U"; value: PropertiesPanelController.primUTile; color: "#c040c0"; step: 0.1
                    onNewValue: function(val) { PropertiesPanelController.primUTile = val } }
                TransformField { label: "V"; value: PropertiesPanelController.primVTile; color: "#c040c0"; step: 0.1
                    onNewValue: function(val) { PropertiesPanelController.primVTile = val } }
            }
        }
    }

    // ---- LOD Generation Content ----
    Component {
        id: lodComponent

        Column {
            id: lodContent
            width: parent ? parent.width : 200
            padding: 8
            spacing: 6

            property int lodCount: 2
            // Tracks the live reduction values from the sliders so Generate can read them
            property var reductionValues: [0.25, 0.5, 0.75, 1.0]

            // LOD level summary table
            Column {
                id: lodInfoColumn
                width: parent.width - 16
                spacing: 2
                visible: MeshLodController.currentLodLevels > 0
                property var lodInfoModel: MeshLodController.lodLevelInfo()
                Connections {
                    target: MeshLodController
                    function onLodChanged() {
                        lodInfoColumn.lodInfoModel = MeshLodController.lodLevelInfo()
                        lodPreviewSlider.value = 0
                    }
                    function onGenerationSucceeded() { lodInfoColumn.lodInfoModel = MeshLodController.lodLevelInfo() }
                }

                Text {
                    text: MeshLodController.currentLodLevels + " LOD level(s) generated:"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 11; font.bold: true
                }

                Repeater {
                    model: lodInfoColumn.lodInfoModel
                    Row {
                        spacing: 8
                        Text {
                            text: modelData.label + ":"
                            color: PropertiesPanelController.textColor; font.pixelSize: 10; width: 46
                        }
                        Text {
                            text: modelData.triangles.toLocaleString() + " triangles"
                            color: Qt.lighter(PropertiesPanelController.textColor, 0.8); font.pixelSize: 10
                        }
                    }
                }

                // Preview LOD slider
                Row {
                    spacing: 6; width: parent.width
                    Text {
                        text: "Preview:"
                        color: PropertiesPanelController.textColor; font.pixelSize: 10
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Slider {
                        id: lodPreviewSlider
                        from: 0; to: Math.max(0, MeshLodController.currentLodLevels)
                        value: 0; stepSize: 1
                        width: parent.width - 90
                        anchors.verticalCenter: parent.verticalCenter
                        onValueChanged: MeshLodController.previewLod(value)
                    }
                    Text {
                        property var info: (lodPreviewSlider.value < lodInfoColumn.lodInfoModel.length)
                            ? lodInfoColumn.lodInfoModel[Math.round(lodPreviewSlider.value)] : null
                        text: info ? (info.label + "\n" + info.triangles + " tri") : ""
                        color: PropertiesPanelController.textColor; font.pixelSize: 9; width: 52
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
            }
            Text {
                text: "No LOD levels — click Generate or Auto below."
                visible: MeshLodController.currentLodLevels === 0
                color: Qt.lighter(PropertiesPanelController.textColor, 0.6)
                font.pixelSize: 10; font.italic: true
            }

            // LOD count row
            Row {
                spacing: 6
                Text {
                    text: "Levels:"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 11
                    anchors.verticalCenter: parent.verticalCenter
                }
                Row {
                    spacing: 2
                    Repeater {
                        model: 4
                        Rectangle {
                            width: 22; height: 22; radius: 3
                            color: (index + 1) === lodCountSelector.value
                                   ? PropertiesPanelController.highlightColor
                                   : PropertiesPanelController.headerColor
                            border.color: PropertiesPanelController.borderColor; border.width: 1
                            Text {
                                anchors.centerIn: parent
                                text: index + 1
                                color: PropertiesPanelController.textColor; font.pixelSize: 11
                            }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: lodCountSelector.value = index + 1
                            }
                        }
                    }
                    // hidden SpinBox just to track value
                    SpinBox { id: lodCountSelector; visible: false; value: 2; from: 1; to: 4 }
                }
            }

            // Reduction sliders per level
            Column {
                width: parent.width - 16
                spacing: 4

                Repeater {
                    model: lodCountSelector.value

                    Row {
                        spacing: 6
                        width: parent.width

                        Text {
                            text: "LOD " + (index + 1) + ":"
                            color: PropertiesPanelController.textColor; font.pixelSize: 11
                            width: 42; anchors.verticalCenter: parent.verticalCenter
                        }
                        Slider {
                            id: reductionSlider
                            from: 0.1; to: 0.95
                            value: 0.25 * (index + 1)
                            stepSize: 0.05
                            width: parent.width - 90
                            anchors.verticalCenter: parent.verticalCenter
                            onValueChanged: {
                                var arr = lodContent.reductionValues.slice()
                                arr[index] = value
                                lodContent.reductionValues = arr
                            }
                        }
                        Text {
                            text: Math.round(reductionSlider.value * 100) + "%"
                            color: PropertiesPanelController.textColor; font.pixelSize: 10; width: 36
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }
                }
            }

            // Action buttons
            Row {
                spacing: 6
                width: parent.width - 16

                Rectangle {
                    id: generateBtn
                    height: 26; width: (parent.width - 6) * 0.5; radius: 3
                    color: genMouse.pressed ? Qt.darker(PropertiesPanelController.highlightColor, 1.2)
                         : genMouse.containsMouse ? Qt.lighter(PropertiesPanelController.highlightColor, 1.1)
                         : PropertiesPanelController.highlightColor
                    Text { anchors.centerIn: parent; text: "Generate"; color: "white"; font.pixelSize: 11 }
                    MouseArea {
                        id: genMouse; anchors.fill: parent; hoverEnabled: true
                        onClicked: {
                            var reductions = lodContent.reductionValues.slice(0, lodCountSelector.value)
                            MeshLodController.generateLods(lodCountSelector.value, reductions)
                        }
                    }
                }

                Rectangle {
                    id: autoBtn
                    height: 26; width: (parent.width - 6) * 0.25; radius: 3
                    color: autoMouse.pressed ? Qt.darker(PropertiesPanelController.headerColor, 1.3)
                         : autoMouse.containsMouse ? Qt.lighter(PropertiesPanelController.headerColor, 1.2)
                         : PropertiesPanelController.headerColor
                    border.color: PropertiesPanelController.borderColor; border.width: 1
                    Text { anchors.centerIn: parent; text: "Auto"; color: PropertiesPanelController.textColor; font.pixelSize: 11 }
                    MouseArea {
                        id: autoMouse; anchors.fill: parent; hoverEnabled: true
                        onClicked: MeshLodController.generateAutoLods()
                    }
                }

                Rectangle {
                    id: removeBtn
                    height: 26; width: (parent.width - 6) * 0.25; radius: 3
                    color: removeMouse.pressed ? Qt.darker(PropertiesPanelController.headerColor, 1.3)
                         : removeMouse.containsMouse ? Qt.lighter(PropertiesPanelController.headerColor, 1.2)
                         : PropertiesPanelController.headerColor
                    border.color: PropertiesPanelController.borderColor; border.width: 1
                    Text { anchors.centerIn: parent; text: "Remove"; color: PropertiesPanelController.textColor; font.pixelSize: 11 }
                    MouseArea {
                        id: removeMouse; anchors.fill: parent; hoverEnabled: true
                        onClicked: MeshLodController.removeLods()
                    }
                }
            }

            // Persistence note
            Text {
                width: parent.width - 16
                wrapMode: Text.Wrap
                font.pixelSize: 10
                font.italic: true
                color: Qt.lighter(PropertiesPanelController.textColor, 0.7)
                text: "LOD levels persist only when exporting as Ogre .mesh.\nFor other engines, use Export LODs below."
            }

            // Export LODs row
            Row {
                spacing: 6
                width: parent.width - 16

                ComboBox {
                    id: exportFormatCombo
                    width: 90; height: 26
                    model: ["gltf", "glb", "fbx", "obj", "mesh"]
                    background: Rectangle {
                        color: PropertiesPanelController.headerColor
                        border.color: PropertiesPanelController.borderColor; border.width: 1; radius: 3
                    }
                    contentItem: Text {
                        leftPadding: 6
                        text: exportFormatCombo.displayText
                        color: PropertiesPanelController.textColor; font.pixelSize: 11
                        verticalAlignment: Text.AlignVCenter
                    }
                }

                Rectangle {
                    height: 26; width: parent.width - exportFormatCombo.width - 6; radius: 3
                    color: exportMouse.pressed ? Qt.darker(PropertiesPanelController.headerColor, 1.3)
                         : exportMouse.containsMouse ? Qt.lighter(PropertiesPanelController.headerColor, 1.2)
                         : PropertiesPanelController.headerColor
                    border.color: PropertiesPanelController.borderColor; border.width: 1
                    Text { anchors.centerIn: parent; text: "Export LODs…"; color: PropertiesPanelController.textColor; font.pixelSize: 11 }
                    MouseArea {
                        id: exportMouse; anchors.fill: parent; hoverEnabled: true
                        onClicked: MeshLodController.exportLods(exportFormatCombo.currentText)
                    }
                }
            }

            // Feedback
            Text {
                id: lodFeedback
                width: parent.width - 16
                wrapMode: Text.Wrap
                font.pixelSize: 10
                color: "#60c060"
                text: ""

                Connections {
                    target: MeshLodController
                    function onGenerationSucceeded(levels) {
                        lodFeedback.color = "#60c060"
                        lodFeedback.text = levels < 0
                            ? "Auto LOD applied."
                            : levels + " LOD level(s) generated."
                    }
                    function onExportSucceeded(count, directory) {
                        lodFeedback.color = "#60c060"
                        lodFeedback.text = count + " LOD file(s) saved to " + directory
                    }
                    function onError(msg) {
                        lodFeedback.color = "#c06060"
                        lodFeedback.text = msg
                    }
                    function onLodChanged() {
                        lodFeedback.text = ""
                    }
                }
            }
        }
    }

    // ---- Mesh Validation Content ----
    Component {
        id: validationComponent

        Column {
            width: parent ? parent.width : 200
            padding: 8
            spacing: 6

            // Validate button
            Rectangle {
                width: parent.width - 16; height: 28; radius: 3
                color: validateMouse.pressed ? Qt.darker(PropertiesPanelController.highlightColor, 1.2)
                     : validateMouse.containsMouse ? Qt.lighter(PropertiesPanelController.highlightColor, 1.1)
                     : PropertiesPanelController.highlightColor
                Text { anchors.centerIn: parent; text: "Run Validation"; color: "white"; font.pixelSize: 12 }
                MouseArea {
                    id: validateMouse; anchors.fill: parent; hoverEnabled: true
                    onClicked: MeshValidator.validate()
                }
            }

            // Issues list
            Column {
                width: parent.width - 16
                spacing: 3
                visible: MeshValidator.validated

                Repeater {
                    model: MeshValidator.issues

                    Row {
                        spacing: 6; width: parent.width

                        Text {
                            text: modelData.type === "error" ? "\u2718"
                                : modelData.type === "warning" ? "\u26A0"
                                : "\u2714"
                            color: modelData.type === "error" ? "#e05050"
                                 : modelData.type === "warning" ? "#e0a030"
                                 : "#60c060"
                            font.pixelSize: 13
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Text {
                            text: modelData.description
                            color: PropertiesPanelController.textColor; font.pixelSize: 10
                            wrapMode: Text.Wrap; width: parent.width - 24
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }
                }
            }

            // Fix All button
            Rectangle {
                width: parent.width - 16; height: 28; radius: 3
                visible: MeshValidator.hasFixableIssues
                color: fixMouse.pressed ? Qt.darker("#c04040", 1.2)
                     : fixMouse.containsMouse ? Qt.lighter("#c04040", 1.2)
                     : "#c04040"
                Text { anchors.centerIn: parent; text: "Fix All (re-import with cleanup)"; color: "white"; font.pixelSize: 11 }
                MouseArea {
                    id: fixMouse; anchors.fill: parent; hoverEnabled: true
                    onClicked: MeshValidator.fixAll()
                }
            }

            // Fix feedback
            Text {
                id: fixFeedback
                width: parent.width - 16; wrapMode: Text.Wrap
                font.pixelSize: 10; color: "#60c060"; text: ""

                Connections {
                    target: MeshValidator
                    function onFixApplied(msg) {
                        fixFeedback.color = "#60c060"
                        fixFeedback.text = msg
                    }
                    function onError(msg) {
                        fixFeedback.color = "#c06060"
                        fixFeedback.text = msg
                    }
                }
            }
        }
    }

    // ---- Animation Control Content (keyframe editor) ----
    Component {
        id: animControlComponent

        Loader {
            width: parent ? parent.width : 300
            source: "qrc:/AnimationControl/AnimationControlPanel.qml"
        }
    }

    // ---- Animation Content ----
    Component {
        id: animationComponent

        Column {
            width: parent ? parent.width : 200
            padding: 8
            spacing: 4

            property var entityGroups: PropertiesPanelController.animationData()

            function refreshAnimData() {
                entityGroups = PropertiesPanelController.animationData()
            }

            Connections {
                target: PropertiesPanelController
                function onSelectionChanged() { refreshAnimData() }
                function onAnimationStateChanged() { refreshAnimData() }
            }

            // Play/Pause button
            Row {
                spacing: 8
                width: parent.width - 16

                Rectangle {
                    width: 28; height: 28; radius: 3
                    color: playMouse.pressed ? Qt.darker(PropertiesPanelController.headerColor, 1.2)
                         : playMouse.containsMouse ? Qt.lighter(PropertiesPanelController.headerColor, 1.2)
                         : PropertiesPanelController.headerColor
                    border.color: PropertiesPanelController.borderColor

                    Text {
                        anchors.centerIn: parent
                        text: PropertiesPanelController.playing ? "\u275A\u275A" : "\u25B6"
                        color: PropertiesPanelController.textColor; font.pixelSize: 14
                    }
                    MouseArea {
                        id: playMouse; anchors.fill: parent; hoverEnabled: true
                        onClicked: PropertiesPanelController.playing = !PropertiesPanelController.playing
                    }
                }
            }

            // Per-entity groups
            Repeater {
                model: entityGroups

                Column {
                    width: parent.width - 16
                    spacing: 2

                    property var grp: modelData
                    property bool groupExpanded: true

                    // Entity header with chevron
                    Rectangle {
                        width: parent.width; height: 22
                        color: entHeaderMouse.containsMouse
                               ? Qt.lighter(PropertiesPanelController.headerColor, 1.1)
                               : PropertiesPanelController.headerColor
                        border.color: PropertiesPanelController.borderColor; border.width: 1

                        Row {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left; anchors.leftMargin: 6; spacing: 4

                            Text {
                                text: groupExpanded ? "\u25BC" : "\u25B6"
                                color: PropertiesPanelController.textColor; font.pixelSize: 8
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Text {
                                text: grp.entity + " (" + grp.animations.length + " anim)"
                                color: PropertiesPanelController.textColor; font.pixelSize: 11; font.bold: true
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }

                        MouseArea {
                            id: entHeaderMouse; anchors.fill: parent; hoverEnabled: true
                            onClicked: groupExpanded = !groupExpanded
                        }
                    }

                    // Expanded content
                    Column {
                        visible: groupExpanded
                        width: parent.width
                        spacing: 2
                        leftPadding: 8

                        // Animation rows
                        Repeater {
                            model: grp.animations

                            Rectangle {
                                width: parent.width - 8; height: 22; color: "transparent"

                                Row {
                                    anchors.fill: parent; spacing: 6

                                    // Enable checkbox
                                    Rectangle {
                                        width: 14; height: 14; anchors.verticalCenter: parent.verticalCenter
                                        border.color: PropertiesPanelController.borderColor; border.width: 1; radius: 2
                                        color: modelData.enabled ? PropertiesPanelController.highlightColor : "transparent"
                                        Text { anchors.centerIn: parent; text: modelData.enabled ? "\u2713" : ""; color: "white"; font.pixelSize: 10 }
                                        MouseArea { anchors.fill: parent; onClicked: PropertiesPanelController.toggleAnimationEnabled(grp.entity, modelData.name, !modelData.enabled) }
                                    }

                                    // Loop checkbox
                                    Rectangle {
                                        width: 14; height: 14; anchors.verticalCenter: parent.verticalCenter
                                        border.color: PropertiesPanelController.borderColor; border.width: 1; radius: 7
                                        color: modelData.loop ? PropertiesPanelController.highlightColor : "transparent"
                                        Text { anchors.centerIn: parent; text: modelData.loop ? "\u21BB" : ""; color: "white"; font.pixelSize: 8 }
                                        MouseArea { anchors.fill: parent; onClicked: PropertiesPanelController.toggleAnimationLoop(grp.entity, modelData.name, !modelData.loop) }
                                    }

                                    // Name (double-click to rename)
                                    Text {
                                        id: animText
                                        visible: !animEdit.visible
                                        text: modelData.name + " (" + modelData.length.toFixed(2) + "s)"
                                        color: PropertiesPanelController.textColor; font.pixelSize: 11
                                        elide: Text.ElideRight; anchors.verticalCenter: parent.verticalCenter
                                        MouseArea {
                                            anchors.fill: parent
                                            onDoubleClicked: { animEdit.text = modelData.name; animEdit.visible = true; animEdit.forceActiveFocus(); animEdit.selectAll() }
                                        }
                                    }
                                    TextInput {
                                        id: animEdit; visible: false; width: 100
                                        color: PropertiesPanelController.textColor; font.pixelSize: 11
                                        anchors.verticalCenter: parent.verticalCenter; selectByMouse: true
                                        Rectangle { anchors.fill: parent; anchors.margins: -2; z: -1; color: PropertiesPanelController.inputColor; border.color: PropertiesPanelController.highlightColor; border.width: 1; radius: 2 }
                                        onEditingFinished: { if (text.length > 0 && text !== modelData.name) PropertiesPanelController.renameAnimation(grp.entity, modelData.name, text); visible = false }
                                        Keys.onEscapePressed: visible = false
                                    }
                                }
                            }
                        }

                        // Skeleton/Weights row (if has skeleton)
                        Row {
                            visible: grp.hasSkeleton
                            spacing: 8; topPadding: 4

                            Rectangle {
                                width: 14; height: 14; anchors.verticalCenter: parent.verticalCenter
                                border.color: PropertiesPanelController.borderColor; border.width: 1; radius: 2
                                color: grp.showSkeleton ? PropertiesPanelController.highlightColor : "transparent"
                                Text { anchors.centerIn: parent; text: grp.showSkeleton ? "\u2713" : ""; color: "white"; font.pixelSize: 10 }
                                MouseArea { anchors.fill: parent; onClicked: PropertiesPanelController.toggleSkeletonDebug(grp.entity, !grp.showSkeleton) }
                            }
                            Text { text: "Skeleton"; color: PropertiesPanelController.textColor; font.pixelSize: 10; anchors.verticalCenter: parent.verticalCenter }

                            Rectangle {
                                width: 14; height: 14; anchors.verticalCenter: parent.verticalCenter
                                border.color: PropertiesPanelController.borderColor; border.width: 1; radius: 2
                                color: grp.showWeights ? PropertiesPanelController.highlightColor : "transparent"
                                Text { anchors.centerIn: parent; text: grp.showWeights ? "\u2713" : ""; color: "white"; font.pixelSize: 10 }
                                MouseArea { anchors.fill: parent; onClicked: PropertiesPanelController.toggleBoneWeights(grp.entity, !grp.showWeights) }
                            }
                            Text { text: "Weights"; color: PropertiesPanelController.textColor; font.pixelSize: 10; anchors.verticalCenter: parent.verticalCenter }
                        }
                    }
                }
            }
        }
    }
}
