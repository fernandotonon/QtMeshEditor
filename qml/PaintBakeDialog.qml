import QtQuick
import QtQuick.Window
import PropertiesPanel 1.0

// Paint v2 Slice I (#552) — bake painted layers to engine deliverables.
//
// Top-level Window so it has its own OS frame rather than being constrained by
// the docked PropertiesPanel, matching TextureChannelPackerDialog /
// TextureAtlasDialog. Styled with Inspector primitives (Rectangle + Text +
// MouseArea over PropertiesPanelController.* colours), not QtQuick.Controls.
//
// TexturePaintController lives in the PropertiesPanel module, so unlike the
// packer/atlas dialogs there is no MaterialEditorQML import here.
Window {
    id: dialog
    title: "Bake PBR Set"
    width: 700
    height: 560
    minimumWidth: 560
    minimumHeight: 480
    flags: Qt.Dialog
    modality: Qt.ApplicationModal
    color: PropertiesPanelController.panelColor

    property string targetId: "generic"
    property int resolution: 0            // 0 = keep each channel's own size
    property string outputDir: ""
    property string namePrefix: ""
    property bool includeHidden: false
    property bool writeSidecar: true
    property string previewDataUrl: ""
    property string statusText: ""
    property bool statusIsError: false

    function open() {
        dialog.show()
        dialog.raise()
        dialog.requestActivate()
        refreshPainted()
        refreshPreview()
    }

    property var paintedChannels: []
    function refreshPainted() {
        paintedChannels = TexturePaintController.paintedChannelIds()
    }
    function refreshPreview() {
        previewDataUrl = TexturePaintController.bakePreviewUrl(targetId, 0, 128)
    }

    onTargetIdChanged: refreshPreview()

    Column {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 10

        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: "Composites each painted channel and writes the textures a game "
                + "engine consumes. Only channels you have actually painted are "
                + "written."
            color: PropertiesPanelController.textColor
            font.pixelSize: 11
            opacity: 0.85
        }

        // ---- engine target ----
        Text {
            text: "Engine target"
            color: PropertiesPanelController.textColor
            font.pixelSize: 11
            font.bold: true
        }
        Flow {
            width: parent.width
            spacing: 4
            Repeater {
                model: TexturePaintController.bakeTargetIds()
                Rectangle {
                    required property string modelData
                    width: tgtLbl.implicitWidth + 18
                    height: 24
                    radius: 3
                    color: dialog.targetId === modelData
                         ? PropertiesPanelController.highlightColor
                         : (tgtMa.containsMouse ? PropertiesPanelController.headerColor
                                                : PropertiesPanelController.inputColor)
                    border.color: PropertiesPanelController.borderColor
                    border.width: 1
                    Text {
                        id: tgtLbl
                        anchors.centerIn: parent
                        text: TexturePaintController.bakeTargetLabel(modelData)
                        color: PropertiesPanelController.textColor
                        font.pixelSize: 10
                    }
                    MouseArea {
                        id: tgtMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: dialog.targetId = modelData
                    }
                }
            }
        }

        // ---- resolution ----
        Text {
            text: "Resolution"
            color: PropertiesPanelController.textColor
            font.pixelSize: 11
            font.bold: true
        }
        Row {
            spacing: 4
            Repeater {
                model: [ { label: "Source", v: 0 }, { label: "512", v: 512 },
                         { label: "1024", v: 1024 }, { label: "2048", v: 2048 },
                         { label: "4096", v: 4096 } ]
                Rectangle {
                    required property var modelData
                    width: 62
                    height: 24
                    radius: 3
                    color: dialog.resolution === modelData.v
                         ? PropertiesPanelController.highlightColor
                         : PropertiesPanelController.inputColor
                    border.color: PropertiesPanelController.borderColor
                    border.width: 1
                    Text {
                        anchors.centerIn: parent
                        text: modelData.label
                        color: PropertiesPanelController.textColor
                        font.pixelSize: 10
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: dialog.resolution = modelData.v
                    }
                }
            }
        }

        // ---- output dir + prefix ----
        Row {
            width: parent.width
            spacing: 6
            Rectangle {
                width: 90; height: 24; radius: 3
                color: dirMa.containsMouse ? PropertiesPanelController.headerColor
                                           : PropertiesPanelController.inputColor
                border.color: PropertiesPanelController.borderColor
                border.width: 1
                Text {
                    anchors.centerIn: parent
                    text: "Output folder…"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 9
                }
                MouseArea {
                    id: dirMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        var d = TexturePaintController.chooseBakeOutputDir()
                        if (d && d.length > 0) dialog.outputDir = d
                    }
                }
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 100
                elide: Text.ElideMiddle
                text: dialog.outputDir.length > 0 ? dialog.outputDir
                                                  : "(choose a folder)"
                color: PropertiesPanelController.textColor
                font.pixelSize: 10
                opacity: dialog.outputDir.length > 0 ? 1.0 : 0.6
            }
        }
        Row {
            spacing: 6
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "Name prefix"
                color: PropertiesPanelController.textColor
                font.pixelSize: 10
            }
            Rectangle {
                width: 180; height: 24; radius: 3
                color: PropertiesPanelController.inputColor
                border.color: PropertiesPanelController.borderColor
                border.width: 1
                TextInput {
                    id: prefixInput
                    anchors.fill: parent
                    anchors.margins: 4
                    verticalAlignment: TextInput.AlignVCenter
                    text: dialog.namePrefix
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 10
                    selectByMouse: true
                    onTextChanged: dialog.namePrefix = text
                }
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: dialog.namePrefix.length > 0
                    ? ("e.g. " + dialog.namePrefix + "_BaseColor.png")
                    : "e.g. BaseColor.png"
                color: PropertiesPanelController.textColor
                font.pixelSize: 9
                opacity: 0.7
            }
        }

        // ---- options ----
        Row {
            spacing: 14
            Row {
                spacing: 5
                Rectangle {
                    width: 14; height: 14; radius: 2
                    anchors.verticalCenter: parent.verticalCenter
                    color: dialog.includeHidden ? PropertiesPanelController.highlightColor
                                                : PropertiesPanelController.controlBgColor
                    border.color: PropertiesPanelController.borderColor
                    border.width: 1
                    Text {
                        anchors.centerIn: parent
                        text: dialog.includeHidden ? "✓" : ""
                        color: "white"; font.pixelSize: 9
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: dialog.includeHidden = !dialog.includeHidden
                    }
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Include hidden layers"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 10
                }
            }
            Row {
                spacing: 5
                Rectangle {
                    width: 14; height: 14; radius: 2
                    anchors.verticalCenter: parent.verticalCenter
                    color: dialog.writeSidecar ? PropertiesPanelController.highlightColor
                                               : PropertiesPanelController.controlBgColor
                    border.color: PropertiesPanelController.borderColor
                    border.width: 1
                    Text {
                        anchors.centerIn: parent
                        text: dialog.writeSidecar ? "✓" : ""
                        color: "white"; font.pixelSize: 9
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: dialog.writeSidecar = !dialog.writeSidecar
                    }
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Write sidecar JSON"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 10
                }
            }
        }

        // ---- what will be written ----
        Row {
            width: parent.width
            spacing: 10
            Rectangle {
                width: 132; height: 132; radius: 3
                color: PropertiesPanelController.controlBgColor
                border.color: PropertiesPanelController.borderColor
                border.width: 1
                Image {
                    anchors.centerIn: parent
                    width: 128; height: 128
                    fillMode: Image.PreserveAspectFit
                    source: dialog.previewDataUrl
                    visible: dialog.previewDataUrl.length > 0
                }
                Text {
                    anchors.centerIn: parent
                    visible: dialog.previewDataUrl.length === 0
                    text: "no preview"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 9
                    opacity: 0.6
                }
            }
            Column {
                spacing: 4
                width: parent.width - 150
                Text {
                    text: "Painted channels"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 10
                    font.bold: true
                }
                Text {
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: dialog.paintedChannels.length > 0
                        ? dialog.paintedChannels.join(", ")
                        : "Nothing painted yet — paint a channel first."
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 10
                    opacity: dialog.paintedChannels.length > 0 ? 0.9 : 0.6
                }
                // Height is deliberately absent (#547): it shares the Normal
                // session and has no standalone consumer, so there is no height
                // data to bake. Said here so its absence reads as a decision.
                Text {
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: "Height is not baked: it shares the Normal channel and "
                        + "has no separate data. Paint Normal instead."
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 9
                    opacity: 0.6
                }
            }
        }

        // ---- status ----
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            visible: dialog.statusText.length > 0
            text: dialog.statusText
            color: dialog.statusIsError ? "#e06c6c"
                                        : PropertiesPanelController.textColor
            font.pixelSize: 10
        }

        // ---- actions ----
        Row {
            spacing: 6
            Rectangle {
                width: 110; height: 26; radius: 3
                color: bakeMa.containsMouse ? PropertiesPanelController.highlightColor
                                            : PropertiesPanelController.inputColor
                border.color: PropertiesPanelController.borderColor
                border.width: 1
                opacity: dialog.outputDir.length > 0 ? 1.0 : 0.5
                Text {
                    anchors.centerIn: parent
                    text: "Bake"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 11
                }
                MouseArea {
                    id: bakeMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    enabled: dialog.outputDir.length > 0
                    onClicked: {
                        var err = TexturePaintController.bakePbrSet(
                            dialog.targetId, dialog.outputDir, dialog.resolution,
                            dialog.namePrefix, dialog.includeHidden,
                            dialog.writeSidecar)
                        dialog.statusIsError = (err && err.length > 0)
                        dialog.statusText = dialog.statusIsError
                            ? err
                            : ("Baked to " + dialog.outputDir)
                        dialog.refreshPainted()
                    }
                }
            }
            Rectangle {
                width: 80; height: 26; radius: 3
                color: closeMa.containsMouse ? PropertiesPanelController.headerColor
                                             : PropertiesPanelController.inputColor
                border.color: PropertiesPanelController.borderColor
                border.width: 1
                Text {
                    anchors.centerIn: parent
                    text: "Close"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 11
                }
                MouseArea {
                    id: closeMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: dialog.close()
                }
            }
        }
    }
}
