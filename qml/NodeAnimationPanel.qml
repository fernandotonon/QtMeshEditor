import QtQuick
import QtQuick.Controls
import AnimationControl 1.0
import PropertiesPanel 1.0

// ── Node Transform Animation (#517 slice C) ──────────────────────────────────
// Author transform (TRS) clips on non-skinned SceneNodes — animated props,
// doors, spinning machinery, camera moves. Node clips are owned by the
// SceneManager and animate the SceneNode whose name == the entity name.
//
// Edit-session model: New starts an EDIT session (node stays free for the
// gizmo, clip hidden from the animation list). Key captures the node's current
// transform at the playhead. Done editing commits the clip to the animation
// list where the MAIN transport (Play) drives it like any other clip. Edit
// re-opens an existing clip. Extracted into its own inspector group so the
// Animation Control section isn't overloaded.
Column {
    id: nodeAnimSection
    width: parent ? parent.width : 300
    spacing: 6

    // Inspector-themed button (AnimationControl palette), mirrors the panel's
    // ToolBtn so this standalone group matches the rest of the inspector.
    component ToolBtn: Rectangle {
        property string label: ""
        property bool   enabled: true
        signal clicked()
        width: Math.max(28, lblT.implicitWidth + 10); height: 22; radius: 3
        color: maT.pressed ? Qt.darker(AnimationControlController.buttonColor, 1.3)
             : maT.containsMouse ? Qt.lighter(AnimationControlController.buttonColor, 1.15)
             : AnimationControlController.buttonColor
        border.color: AnimationControlController.borderColor; border.width: 1
        opacity: enabled ? 1.0 : 0.4
        Text { id: lblT; anchors.centerIn: parent; text: parent.label; color: AnimationControlController.buttonTextColor; font.pixelSize: 11 }
        MouseArea { id: maT; anchors.fill: parent; hoverEnabled: true; enabled: parent.enabled; onClicked: parent.clicked() }
    }

    // Inspector-themed text input.
    component ThemedInput: Rectangle {
        id: tiRoot
        property alias text: tiIn.text
        property string placeholder: ""
        property var inputValidator: null
        signal accepted()
        height: 22; radius: 2
        color: AnimationControlController.inputColor
        border.color: tiIn.activeFocus ? AnimationControlController.highlightColor
                                        : AnimationControlController.borderColor
        border.width: 1
        TextInput {
            id: tiIn
            anchors.fill: parent; anchors.margins: 4
            color: AnimationControlController.textColor
            font.pixelSize: 11
            verticalAlignment: TextInput.AlignVCenter
            selectByMouse: true; clip: true
            validator: tiRoot.inputValidator
            onAccepted: tiRoot.accepted()
        }
        Text {
            anchors.left: parent.left; anchors.leftMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            text: tiRoot.placeholder
            color: AnimationControlController.disabledTextColor
            font.pixelSize: 11
            visible: tiIn.text.length === 0 && !tiIn.activeFocus
        }
    }

    property var clipNames: NodeAnimationManager.listClips()
    function refreshClips() { clipNames = NodeAnimationManager.listClips() }
    property bool creatingClip: false

    function commitNewClip() {
        var nm = newNameInput.text.trim()
        var len = parseFloat(newLenInput.text)
        if (nm.length > 0 && !isNaN(len) && len > 0) {
            if (NodeAnimationManager.createClipUndoable(nm, len)) {
                NodeAnimationManager.beginEdit(nm)
                AnimationControlController.animationLength = len
            }
        }
        nodeAnimSection.creatingClip = false
    }

    Connections {
        target: NodeAnimationManager
        function onClipsChanged() {
            nodeAnimSection.refreshClips()
            if (NodeAnimationManager.activeClip.length > 0 &&
                    nodeAnimSection.clipNames.indexOf(NodeAnimationManager.activeClip) < 0)
                NodeAnimationManager.activeClip = ""
            // If the clip being EDITED disappeared (e.g. deleted via the Del
            // button while it was also the edit target after commitNewClip), end
            // the edit session too — otherwise the authoring row stays visible and
            // "Key selected node" targets a clip the SceneManager no longer has. (#517)
            if (NodeAnimationManager.editingClip.length > 0 &&
                    nodeAnimSection.clipNames.indexOf(NodeAnimationManager.editingClip) < 0)
                NodeAnimationManager.endEdit()
            AnimationControlController.updateAnimationTree()
        }
    }

    // Clip picker (themed dropdown) + New / Delete.
    Row {
        spacing: 4; width: parent.width

        Item {
            id: nodeClipSelector
            width: parent.width - 92; height: 22
            property bool open: false

            Rectangle {
                anchors.fill: parent; radius: 3
                color: nodeClipMouse.pressed ? Qt.darker(AnimationControlController.buttonColor, 1.2)
                     : nodeClipMouse.containsMouse ? Qt.lighter(AnimationControlController.buttonColor, 1.1)
                     : AnimationControlController.buttonColor
                border.color: nodeClipSelector.open ? AnimationControlController.highlightColor
                                                    : AnimationControlController.borderColor
                border.width: 1
                Row {
                    anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 4; spacing: 4
                    Text {
                        text: NodeAnimationManager.activeClip.length > 0
                              ? NodeAnimationManager.activeClip : "— no node clip —"
                        color: AnimationControlController.buttonTextColor; font.pixelSize: 11
                        elide: Text.ElideRight; anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 18
                    }
                    Text {
                        text: nodeClipSelector.open ? "▲" : "▼"
                        color: AnimationControlController.buttonTextColor; font.pixelSize: 8
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
                MouseArea {
                    id: nodeClipMouse; anchors.fill: parent; hoverEnabled: true
                    enabled: nodeAnimSection.clipNames.length > 0
                    onClicked: nodeClipSelector.open = !nodeClipSelector.open
                }
            }

            Popup {
                id: nodeClipPopup
                visible: nodeClipSelector.open
                x: 0; y: nodeClipSelector.height + 2
                width: nodeClipSelector.width
                height: Math.min(nodeClipListView.contentHeight + 4, 160)
                padding: 0
                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
                onClosed: nodeClipSelector.open = false
                background: Rectangle {
                    color: AnimationControlController.inputColor
                    border.color: AnimationControlController.borderColor; border.width: 1; radius: 3
                }
                ListView {
                    id: nodeClipListView
                    anchors.fill: parent; anchors.margins: 2; clip: true
                    model: nodeAnimSection.clipNames
                    delegate: Rectangle {
                        width: nodeClipListView.width; height: 24
                        color: clipItemMouse.containsMouse ? AnimationControlController.highlightColor
                                                           : "transparent"
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left; anchors.leftMargin: 8
                            text: modelData
                            color: AnimationControlController.textColor; font.pixelSize: 11
                            elide: Text.ElideRight; width: parent.width - 12
                        }
                        MouseArea {
                            id: clipItemMouse; anchors.fill: parent; hoverEnabled: true
                            onClicked: {
                                NodeAnimationManager.activeClip = modelData
                                var len = NodeAnimationManager.clipLength(modelData)
                                if (len > 0) AnimationControlController.animationLength = len
                                nodeClipSelector.open = false
                            }
                        }
                    }
                }
            }
        }

        ToolBtn {
            label: "New"
            onClicked: {
                nodeAnimSection.creatingClip = true
                newNameInput.text = "NodeClip"
                newLenInput.text = "5.0"
                newNameInput.forceActiveFocus()
            }
        }

        ToolBtn {
            label: "Del"
            enabled: NodeAnimationManager.activeClip.length > 0
            onClicked: {
                if (NodeAnimationManager.deleteClipUndoable(NodeAnimationManager.activeClip))
                    NodeAnimationManager.activeClip = ""
            }
        }
    }

    // Inline "new clip" editor row.
    Row {
        visible: nodeAnimSection.creatingClip
        spacing: 4; width: parent.width

        ThemedInput {
            id: newNameInput
            width: parent.width - 168
            placeholder: "Clip name"
            onAccepted: nodeAnimSection.commitNewClip()
        }
        ThemedInput {
            id: newLenInput
            width: 56
            placeholder: "5.0"
            inputValidator: DoubleValidator { bottom: 0.01; notation: DoubleValidator.StandardNotation }
            onAccepted: nodeAnimSection.commitNewClip()
        }
        Text { text: "s"; color: AnimationControlController.textColor; font.pixelSize: 11; anchors.verticalCenter: parent.verticalCenter }
        ToolBtn { label: "Create"; onClicked: nodeAnimSection.commitNewClip() }
        ToolBtn { label: "✕"; onClicked: nodeAnimSection.creatingClip = false }
    }

    // Authoring row — shown during an EDIT session.
    Row {
        spacing: 6; width: parent.width
        visible: NodeAnimationManager.editingClip.length > 0

        ToolBtn {
            label: "Key selected node"
            enabled: PropertiesPanelController.selectionName.length > 0
            onClicked: {
                var t = AnimationControlController.sliderValue / 1000.0
                NodeAnimationManager.keyNodeCurrentTransform(
                    NodeAnimationManager.editingClip,
                    PropertiesPanelController.selectionName, t)
            }
        }

        ToolBtn {
            label: "Done editing"
            onClicked: {
                NodeAnimationManager.endEdit()
                AnimationControlController.updateAnimationTree()
            }
        }
    }

    // Edit an existing (committed) clip again.
    Row {
        spacing: 6; width: parent.width
        visible: NodeAnimationManager.activeClip.length > 0
                 && NodeAnimationManager.editingClip.length === 0
        ToolBtn {
            label: "Edit '" + NodeAnimationManager.activeClip + "'"
            onClicked: NodeAnimationManager.beginEdit(NodeAnimationManager.activeClip)
        }
    }

    Text {
        width: parent.width
        visible: NodeAnimationManager.editingClip.length > 0
        wrapMode: Text.WordWrap
        text: PropertiesPanelController.selectionName.length > 0
              ? "Editing '" + NodeAnimationManager.editingClip + "': move '"
                + PropertiesPanelController.selectionName
                + "', set the playhead, then Key. Press Done editing to play it from the list."
              : "Select the scene node to key its transform."
        color: AnimationControlController.disabledTextColor
        font.pixelSize: 10
    }
}
