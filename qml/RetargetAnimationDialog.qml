import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import MaterialEditorQML 1.0
import PropertiesPanel 1.0

// #523: Retarget Animation wizard — source clip → target rig through a
// reviewable bone map, with a side-by-side preview before applying.
Window {
    id: dialog
    title: "Retarget Animation"
    width: 620
    height: 720
    minimumWidth: 520
    minimumHeight: 560
    flags: Qt.Dialog
    color: PropertiesPanelController.panelColor

    property bool onlyUnmapped: false
    readonly property int labelColWidth: 92
    readonly property var targetLabels: RetargetController.targetBones.map(
        function(b) { return b === "" ? "— none —" : b })

    function open() {
        RetargetController.noteOpened()
        dialog.show()
        dialog.raise()
        dialog.requestActivate()
    }
    onClosing: RetargetController.stopPreview()

    component InspectorButton: Rectangle {
        id: btn
        property string label: ""
        property bool buttonEnabled: true
        property bool accent: false
        signal clicked()
        implicitWidth: btnLabel.implicitWidth + 18
        Layout.preferredWidth: Math.max(84, implicitWidth)
        implicitHeight: 26
        radius: 3
        activeFocusOnTab: buttonEnabled
        Keys.onSpacePressed: if (buttonEnabled) btn.clicked()
        Keys.onReturnPressed: if (buttonEnabled) btn.clicked()
        color: (btnMa.containsMouse && buttonEnabled) || accent
            ? PropertiesPanelController.highlightColor
            : PropertiesPanelController.headerColor
        border.color: btn.activeFocus ? PropertiesPanelController.highlightColor
                                      : PropertiesPanelController.borderColor
        border.width: 1
        opacity: buttonEnabled ? 1.0 : 0.45
        Accessible.role: Accessible.Button
        Accessible.name: label
        Text {
            id: btnLabel
            anchors.centerIn: parent
            text: btn.label
            color: PropertiesPanelController.textColor
            font.pixelSize: 11
        }
        MouseArea {
            id: btnMa
            anchors.fill: parent
            hoverEnabled: true
            enabled: btn.buttonEnabled
            cursorShape: btn.buttonEnabled ? Qt.PointingHandCursor : Qt.ForbiddenCursor
            onClicked: btn.clicked()
        }
    }

    component InspectorLabel: Text {
        color: PropertiesPanelController.textColor
        font.pixelSize: 11
    }

    component StepHeader: Text {
        color: PropertiesPanelController.textColor
        font.pixelSize: 12
        font.bold: true
        Layout.topMargin: 6
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 6

        // ---- 1. Source ----
        StepHeader { text: "1. Source clip" }
        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            InspectorLabel { text: "Entity:"; Layout.preferredWidth: dialog.labelColWidth }
            ThemedComboBox {
                Layout.fillWidth: true
                Layout.preferredHeight: 24
                font.pixelSize: 11
                model: RetargetController.skeletalEntities
                currentIndex: model.indexOf(RetargetController.sourceEntity)
                onActivated: function(i) { RetargetController.sourceEntity = model[i] }
                enabled: !RetargetController.previewing
            }
            InspectorButton {
                label: "Import file…"
                buttonEnabled: !RetargetController.previewing
                onClicked: RetargetController.importSourceFile()
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            InspectorLabel { text: "Clip:"; Layout.preferredWidth: dialog.labelColWidth }
            ThemedComboBox {
                Layout.fillWidth: true
                Layout.preferredHeight: 24
                font.pixelSize: 11
                model: RetargetController.sourceAnimations
                currentIndex: model.indexOf(RetargetController.sourceAnimation)
                onActivated: function(i) { RetargetController.sourceAnimation = model[i] }
            }
        }

        // ---- 2. Target ----
        StepHeader { text: "2. Target skeleton" }
        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            InspectorLabel { text: "Entity:"; Layout.preferredWidth: dialog.labelColWidth }
            ThemedComboBox {
                Layout.fillWidth: true
                Layout.preferredHeight: 24
                font.pixelSize: 11
                model: RetargetController.skeletalEntities
                currentIndex: model.indexOf(RetargetController.targetEntity)
                onActivated: function(i) { RetargetController.targetEntity = model[i] }
                enabled: !RetargetController.previewing
            }
        }

        // ---- 3. Bone map ----
        StepHeader { text: "3. Bone map (" + RetargetController.mappedCount + " mapped)" }
        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            InspectorButton {
                label: "Auto-map"
                buttonEnabled: RetargetController.sourceEntity !== "" && RetargetController.targetEntity !== ""
                onClicked: RetargetController.autoMap()
            }
            ThemedComboBox {
                id: bundledCombo
                Layout.preferredWidth: 170
                Layout.preferredHeight: 24
                font.pixelSize: 11
                model: RetargetController.bundledMaps
            }
            InspectorButton {
                label: "Use bundled"
                onClicked: RetargetController.loadBundledMap(bundledCombo.currentText)
            }
            Item { Layout.fillWidth: true }
            InspectorButton { label: "Load…"; onClicked: RetargetController.loadBoneMapFile() }
            InspectorButton {
                label: "Save…"
                buttonEnabled: RetargetController.mappedCount > 0
                onClicked: RetargetController.saveBoneMapFile()
            }
        }
        ThemedCheckBox {
            text: "Show unmapped source bones only"
            checked: dialog.onlyUnmapped
            onToggled: dialog.onlyUnmapped = checked
            contentItem: Text {
                leftPadding: 22
                text: parent.text
                color: PropertiesPanelController.textColor
                font.pixelSize: 11
                verticalAlignment: Text.AlignVCenter
            }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 160
            color: PropertiesPanelController.inputColor
            border.color: PropertiesPanelController.borderColor
            border.width: 1
            radius: 2
            ListView {
                id: mapList
                anchors.fill: parent
                anchors.margins: 4
                clip: true
                spacing: 1
                model: RetargetController.mappingRows
                ScrollBar.vertical: ScrollBar { }
                delegate: Item {
                    width: mapList.width - 12
                    readonly property bool shown: !dialog.onlyUnmapped || modelData.target === ""
                    height: shown ? 24 : 0
                    visible: shown
                    RowLayout {
                        anchors.fill: parent
                        spacing: 6
                        Text {
                            Layout.preferredWidth: parent.width * 0.48
                            leftPadding: 4 + Math.min(modelData.depth, 12) * 8
                            text: modelData.source
                            elide: Text.ElideRight
                            color: modelData.target === "" ? PropertiesPanelController.borderColor
                                                           : PropertiesPanelController.textColor
                            font.pixelSize: 11
                        }
                        Text { text: "→"; color: PropertiesPanelController.textColor; font.pixelSize: 11 }
                        ThemedComboBox {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 22
                            font.pixelSize: 11
                            model: dialog.targetLabels
                            currentIndex: Math.max(0, RetargetController.targetBones.indexOf(modelData.target))
                            onActivated: function(i) {
                                RetargetController.setPairTarget(modelData.source,
                                                                 RetargetController.targetBones[i])
                            }
                        }
                    }
                }
                Text {
                    anchors.centerIn: parent
                    visible: mapList.count === 0
                    text: "Pick a source and a target — the bones are auto-mapped."
                    color: PropertiesPanelController.borderColor
                    font.pixelSize: 11
                }
            }
        }

        // ---- 4. Options ----
        StepHeader { text: "4. Options" }
        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            InspectorLabel { text: "Translation:"; Layout.preferredWidth: dialog.labelColWidth }
            ThemedComboBox {
                Layout.preferredWidth: 220
                Layout.preferredHeight: 24
                font.pixelSize: 11
                readonly property var ids: ["none", "root", "all"]
                model: ["Rotation only (keep bone lengths)", "Root motion (scaled)", "All bones (faces / fingers)"]
                currentIndex: ids.indexOf(RetargetController.translationMode)
                onActivated: function(i) { RetargetController.translationMode = ids[i] }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            InspectorLabel { text: "Source rest:"; Layout.preferredWidth: dialog.labelColWidth }
            ThemedComboBox {
                Layout.preferredWidth: 220
                Layout.preferredHeight: 24
                font.pixelSize: 11
                readonly property var ids: ["bind", "first-frame"]
                model: ["Bind pose", "First frame of the clip"]
                currentIndex: ids.indexOf(RetargetController.sourceRest)
                onActivated: function(i) { RetargetController.sourceRest = ids[i] }
            }
            ThemedCheckBox {
                text: "Match rest directions"
                checked: RetargetController.alignDirections
                onToggled: RetargetController.alignDirections = checked
                contentItem: Text {
                    leftPadding: 22
                    text: parent.text
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 11
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            InspectorLabel { text: "New clip:"; Layout.preferredWidth: dialog.labelColWidth }
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 24
                radius: 2
                color: PropertiesPanelController.inputColor
                border.color: nameField.activeFocus ? PropertiesPanelController.highlightColor
                                                    : PropertiesPanelController.borderColor
                TextInput {
                    id: nameField
                    anchors.fill: parent
                    anchors.margins: 5
                    text: RetargetController.newAnimationName
                    onTextEdited: RetargetController.newAnimationName = text
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 11
                    selectByMouse: true
                    clip: true
                    Text {
                        anchors.fill: parent
                        visible: nameField.text.length === 0
                        text: (RetargetController.sourceAnimation || "clip") + "_retargeted"
                        color: PropertiesPanelController.borderColor
                        font.pixelSize: 11
                    }
                }
            }
        }

        // ---- 5. Preview / apply ----
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 6
            spacing: 6
            InspectorButton {
                label: RetargetController.previewing ? "Stop preview" : "Preview side by side"
                accent: RetargetController.previewing
                buttonEnabled: RetargetController.mappedCount > 0 && RetargetController.sourceAnimation !== ""
                onClicked: RetargetController.previewing ? RetargetController.stopPreview()
                                                         : RetargetController.startPreview()
            }
            Item { Layout.fillWidth: true }
            InspectorButton { label: "Close"; onClicked: dialog.close() }
            InspectorButton {
                label: "Apply"
                buttonEnabled: RetargetController.mappedCount > 0 && RetargetController.sourceAnimation !== ""
                               && RetargetController.targetEntity !== ""
                onClicked: RetargetController.apply()
            }
        }
        Text {
            Layout.fillWidth: true
            visible: RetargetController.status !== ""
            text: (RetargetController.lastOk ? "✓ " : "✗ ") + RetargetController.status
            color: RetargetController.lastOk ? "#60c060" : "#e06060"
            font.pixelSize: 11
            wrapMode: Text.Wrap
        }
    }
}
