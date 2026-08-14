pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import AnimationControl 1.0
import PropertiesPanel 1.0

// ── Pose Library (#521 slice D) ──────────────────────────────────────────────
// A named pose is a frozen snapshot of every bone's TRS. Authors build
// libraries (T-pose, A-pose, neutral, "smile_l"/"smile_r"), apply them with
// one click, blend between them, and mirror a left-side expression to the
// right.
//
// Everything here routes through PoseLibrary's *Undoable entry points, so
// every operation the panel offers is a single Ctrl+Z step.
//
// Layout: a list of saved poses (thumbnail + name + per-row Apply/Mirror/
// Delete), then the authoring rows — save, blended apply, blend two, and the
// apply-with-mask bone picker.
Column {
    id: poseSection
    width: parent ? parent.width : 300
    spacing: 6

    // Inspector-themed button (AnimationControl palette) — mirrors the one in
    // NodeAnimationPanel so this group matches the rest of the inspector.
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
        Text {
            id: lblT; anchors.centerIn: parent; text: parent.label
            color: AnimationControlController.buttonTextColor; font.pixelSize: 11
        }
        MouseArea {
            id: maT; anchors.fill: parent; hoverEnabled: true
            enabled: parent.enabled; onClicked: parent.clicked()
        }
    }

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

    // A themed dropdown over the saved-pose names. Used by the blend row
    // (source A / source B) — the per-row buttons handle everything else.
    component PosePicker: Item {
        id: pickerRoot
        property string selection: ""
        property string emptyLabel: "— pick a pose —"
        height: 22
        property bool open: false

        Rectangle {
            anchors.fill: parent; radius: 3
            color: pickMouse.pressed ? Qt.darker(AnimationControlController.buttonColor, 1.2)
                 : pickMouse.containsMouse ? Qt.lighter(AnimationControlController.buttonColor, 1.1)
                 : AnimationControlController.buttonColor
            border.color: pickerRoot.open ? AnimationControlController.highlightColor
                                          : AnimationControlController.borderColor
            border.width: 1
            Row {
                anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 4; spacing: 4
                Text {
                    text: pickerRoot.selection.length > 0 ? pickerRoot.selection
                                                          : pickerRoot.emptyLabel
                    color: AnimationControlController.buttonTextColor; font.pixelSize: 11
                    elide: Text.ElideRight; anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 18
                }
                Text {
                    text: pickerRoot.open ? "▲" : "▼"
                    color: AnimationControlController.buttonTextColor; font.pixelSize: 8
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
            MouseArea {
                id: pickMouse; anchors.fill: parent; hoverEnabled: true
                enabled: poseSection.poseNames.length > 0
                onClicked: pickerRoot.open = !pickerRoot.open
            }
        }

        Popup {
            visible: pickerRoot.open
            x: 0; y: pickerRoot.height + 2
            width: pickerRoot.width
            height: Math.min(pickList.contentHeight + 4, 160)
            padding: 0
            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
            onClosed: pickerRoot.open = false
            background: Rectangle {
                color: AnimationControlController.inputColor
                border.color: AnimationControlController.borderColor
                border.width: 1; radius: 3
            }
            ListView {
                id: pickList
                anchors.fill: parent; anchors.margins: 2; clip: true
                model: poseSection.poseNames
                delegate: Rectangle {
                    width: pickList.width; height: 24
                    color: itemMouse.containsMouse ? AnimationControlController.highlightColor
                                                   : "transparent"
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left; anchors.leftMargin: 8
                        text: modelData
                        color: AnimationControlController.textColor; font.pixelSize: 11
                        elide: Text.ElideRight; width: parent.width - 12
                    }
                    MouseArea {
                        id: itemMouse; anchors.fill: parent; hoverEnabled: true
                        onClicked: {
                            pickerRoot.selection = modelData
                            pickerRoot.open = false
                        }
                    }
                }
            }
        }
    }

    // ── State ────────────────────────────────────────────────────────────────

    property var poseNames: PoseLibrary.listPosesForSelection()
    // Bumped on every library change. Each row's thumbnail watches this and
    // re-queries, so a save/mirror/blend that invalidated the cached render
    // shows the new pose. (An explicit signal rather than a binding — see the
    // Image delegate below for why the render can't live in a binding.)
    property int thumbGeneration: 0
    property bool savingPose: false
    property bool blending: false
    property bool masking: false
    // Last operation's result line, shown at the bottom of the panel.
    property string statusText: ""
    // Bone names the mask-apply targets. Kept as a plain list so the picker
    // can toggle entries without a C++ round-trip per click.
    property var maskBones: []

    function refreshPoses() {
        poseNames = PoseLibrary.listPosesForSelection()
        thumbGeneration++
    }

    Connections {
        target: PoseLibrary
        function onPosesChanged(entity) { poseSection.refreshPoses() }
        function onBlendFinished(entity, name) {
            poseSection.statusText = "Blend to '" + name + "' finished."
        }
    }
    Connections {
        target: PropertiesPanelController
        function onSelectionChanged() {
            poseSection.refreshPoses()
            poseSection.maskBones = []
            poseSection.statusText = ""
        }
    }

    // ── Saved-pose list ──────────────────────────────────────────────────────

    Text {
        width: parent.width
        visible: poseSection.poseNames.length === 0
        wrapMode: Text.WordWrap
        text: "No saved poses. Pose the rig, then press Save pose to capture it."
        color: AnimationControlController.disabledTextColor
        font.pixelSize: 10
    }

    Column {
        width: parent.width
        spacing: 3

        Repeater {
            model: poseSection.poseNames

            Rectangle {
                id: poseRow
                required property string modelData
                // Local alias so the nested controls don't have to walk
                // `parent.parent…` to reach the delegate's model data —
                // that chain silently breaks whenever the row's visual
                // nesting changes.
                readonly property string poseName: poseRow.modelData

                width: poseSection.width
                height: 40
                radius: 3
                color: AnimationControlController.inputColor
                border.color: AnimationControlController.borderColor
                border.width: 1

                Row {
                    anchors.fill: parent
                    anchors.margins: 3
                    spacing: 6

                    // Thumbnail. Rendering is lazy + cached in C++; an empty
                    // string (headless / no GL) just leaves the frame blank
                    // rather than breaking the row layout.
                    Rectangle {
                        width: 34; height: 34
                        color: AnimationControlController.buttonColor
                        border.color: AnimationControlController.borderColor
                        border.width: 1
                        anchors.verticalCenter: parent.verticalCenter
                        Image {
                            id: thumbImg
                            anchors.fill: parent
                            anchors.margins: 1
                            fillMode: Image.PreserveAspectFit
                            cache: false

                            // Rendering a thumbnail poses the entity, renders
                            // offscreen and restores — too heavy (and too
                            // side-effecting on the live skeleton) to sit in a
                            // property binding QML may re-evaluate whenever it
                            // likes. So it's an explicit one-shot: refresh on
                            // creation and whenever the library changes.
                            function refreshThumb() {
                                source = PoseLibrary.poseThumbnailForSelection(poseRow.poseName)
                            }
                            Component.onCompleted: refreshThumb()
                            Connections {
                                target: poseSection
                                function onThumbGenerationChanged() { thumbImg.refreshThumb() }
                            }
                        }
                        Text {
                            anchors.centerIn: parent
                            visible: thumbImg.source === "" || thumbImg.status !== Image.Ready
                            text: "◍"
                            color: AnimationControlController.disabledTextColor
                            font.pixelSize: 14
                        }
                    }

                    Text {
                        width: parent.width - 34 - 6 - 130
                        anchors.verticalCenter: parent.verticalCenter
                        text: poseRow.poseName
                        color: AnimationControlController.textColor
                        font.pixelSize: 11
                        elide: Text.ElideRight
                    }

                    ToolBtn {
                        anchors.verticalCenter: parent.verticalCenter
                        label: "Apply"
                        // A positive "Blend in" duration turns every Apply
                        // into a smooth transition; 0 snaps.
                        onClicked: {
                            var dur = parseFloat(blendDurInput.text)
                            var ok = (!isNaN(dur) && dur > 0)
                                ? PoseLibrary.applyPoseBlendedUndoable(poseRow.poseName, dur)
                                : PoseLibrary.applyPoseUndoable(poseRow.poseName)
                            poseSection.statusText = ok
                                ? ("Applied '" + poseRow.poseName + "'.")
                                : ("Could not apply '" + poseRow.poseName + "'.")
                        }
                    }

                    ToolBtn {
                        anchors.verticalCenter: parent.verticalCenter
                        label: "⇄"
                        onClicked: {
                            var dst = poseRow.poseName + "_mirrored"
                            poseSection.statusText =
                                PoseLibrary.mirrorPoseUndoable(poseRow.poseName, dst)
                                    ? ("Mirrored to '" + dst + "'.")
                                    : ("Could not mirror '" + poseRow.poseName + "'.")
                        }
                    }

                    ToolBtn {
                        anchors.verticalCenter: parent.verticalCenter
                        label: "✕"
                        onClicked: {
                            poseSection.statusText =
                                PoseLibrary.deletePoseUndoable(poseRow.poseName)
                                    ? ("Deleted '" + poseRow.poseName + "'.")
                                    : ("Could not delete '" + poseRow.poseName + "'.")
                        }
                    }
                }
            }
        }
    }

    // ── Save ─────────────────────────────────────────────────────────────────

    Row {
        spacing: 4; width: parent.width

        ToolBtn {
            label: "Save pose"
            onClicked: {
                poseSection.savingPose = true
                saveNameInput.text = "Pose" + (poseSection.poseNames.length + 1)
                saveNameInput.forceActiveFocus()
            }
        }
        ToolBtn {
            label: "Blend two…"
            enabled: poseSection.poseNames.length >= 2
            onClicked: poseSection.blending = !poseSection.blending
        }
        ToolBtn {
            label: "Mask…"
            enabled: poseSection.poseNames.length > 0
            onClicked: {
                poseSection.masking = !poseSection.masking
                if (poseSection.masking && poseSection.maskBones.length === 0)
                    maskBoneList.allBones = PoseLibrary.boneNamesForSelection()
            }
        }
    }

    Row {
        visible: poseSection.savingPose
        spacing: 4; width: parent.width

        ThemedInput {
            id: saveNameInput
            width: parent.width - 108
            placeholder: "Pose name"
            onAccepted: poseSection.commitSave()
        }
        ToolBtn { label: "Save"; onClicked: poseSection.commitSave() }
        ToolBtn { label: "✕"; onClicked: poseSection.savingPose = false }
    }

    function commitSave() {
        var nm = saveNameInput.text.trim()
        if (nm.length === 0) return
        statusText = PoseLibrary.savePoseUndoable(nm)
            ? ("Saved '" + nm + "'.")
            : "Could not save — select a rigged mesh first."
        savingPose = false
    }

    // ── Blend duration (applies to the per-row Apply button) ─────────────────

    Row {
        spacing: 4; width: parent.width
        Text {
            text: "Blend in:"
            color: AnimationControlController.textColor; font.pixelSize: 11
            anchors.verticalCenter: parent.verticalCenter
        }
        ThemedInput {
            id: blendDurInput
            width: 56
            text: "0"
            placeholder: "0"
            inputValidator: DoubleValidator { bottom: 0.0; notation: DoubleValidator.StandardNotation }
        }
        Text {
            text: "s (0 = snap)"
            color: AnimationControlController.disabledTextColor; font.pixelSize: 10
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    // ── Blend two poses ──────────────────────────────────────────────────────

    Column {
        visible: poseSection.blending
        width: parent.width
        spacing: 4

        Row {
            spacing: 4; width: parent.width
            PosePicker {
                id: blendA
                width: (parent.width - 8) / 2
                emptyLabel: "— pose A —"
            }
            PosePicker {
                id: blendB
                width: (parent.width - 8) / 2
                emptyLabel: "— pose B —"
            }
        }

        Row {
            spacing: 4; width: parent.width
            Text {
                text: "A"
                color: AnimationControlController.textColor; font.pixelSize: 11
                anchors.verticalCenter: parent.verticalCenter
            }
            Slider {
                id: blendWeight
                width: parent.width - 130
                from: 0.0; to: 1.0; value: 0.5
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                text: "B  " + blendWeight.value.toFixed(2)
                color: AnimationControlController.textColor; font.pixelSize: 11
                anchors.verticalCenter: parent.verticalCenter
            }
        }

        Row {
            spacing: 4; width: parent.width
            ThemedInput {
                id: blendDstInput
                width: parent.width - 108
                placeholder: "Result pose name"
                onAccepted: poseSection.commitBlend()
            }
            ToolBtn { label: "Blend"; onClicked: poseSection.commitBlend() }
            ToolBtn { label: "✕"; onClicked: poseSection.blending = false }
        }
    }

    function commitBlend() {
        var dst = blendDstInput.text.trim()
        if (dst.length === 0 || blendA.selection.length === 0
                || blendB.selection.length === 0) {
            statusText = "Pick both source poses and a result name."
            return
        }
        statusText = PoseLibrary.blendPosesUndoable(blendA.selection, blendB.selection,
                                                blendWeight.value, dst)
            ? ("Blended into '" + dst + "'.")
            : "Blend failed."
    }

    // ── Apply with mask ──────────────────────────────────────────────────────

    Column {
        visible: poseSection.masking
        width: parent.width
        spacing: 4

        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            text: "Tick the bones to affect, then Apply masked — every other bone keeps its current pose."
            color: AnimationControlController.disabledTextColor
            font.pixelSize: 10
        }

        Rectangle {
            width: parent.width
            height: 120
            radius: 2
            color: AnimationControlController.inputColor
            border.color: AnimationControlController.borderColor
            border.width: 1

            ListView {
                id: maskBoneList
                property var allBones: []
                anchors.fill: parent; anchors.margins: 2
                clip: true
                model: allBones
                delegate: Row {
                    id: boneRow
                    required property string modelData
                    readonly property string boneName: boneRow.modelData
                    spacing: 4
                    height: 18
                    CheckBox {
                        checked: poseSection.maskBones.indexOf(boneRow.boneName) >= 0
                        onToggled: {
                            var list = poseSection.maskBones.slice()
                            var i = list.indexOf(boneRow.boneName)
                            if (checked && i < 0) list.push(boneRow.boneName)
                            else if (!checked && i >= 0) list.splice(i, 1)
                            poseSection.maskBones = list
                        }
                    }
                    Text {
                        text: boneRow.boneName
                        color: AnimationControlController.textColor
                        font.pixelSize: 10
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
            }
        }

        Row {
            spacing: 4; width: parent.width
            PosePicker {
                id: maskPose
                width: parent.width - 150
                emptyLabel: "— pose to apply —"
            }
            ToolBtn {
                label: "Apply masked"
                enabled: poseSection.maskBones.length > 0
                         && maskPose.selection.length > 0
                onClicked: {
                    poseSection.statusText = PoseLibrary.applyPoseMaskedUndoable(
                        maskPose.selection, poseSection.maskBones)
                        ? ("Applied '" + maskPose.selection + "' to "
                           + poseSection.maskBones.length + " bone(s).")
                        : "Masked apply failed."
                }
            }
        }
    }

    // ── Sidecar persistence + status ─────────────────────────────────────────

    Row {
        spacing: 4; width: parent.width
        ToolBtn {
            label: "Export .poselib"
            enabled: poseSection.poseNames.length > 0
            // MainWindow owns the native QFileDialog (QML has no QWidget
            // parent to give it) and calls back into the library — same
            // pattern as HdrEnvironmentController's Browse.
            onClicked: PoseLibrary.requestExportLibrary()
        }
        ToolBtn {
            label: "Import .poselib"
            onClicked: PoseLibrary.requestImportLibrary()
        }
    }

    Text {
        width: parent.width
        visible: poseSection.statusText.length > 0
        wrapMode: Text.WordWrap
        text: poseSection.statusText
        color: AnimationControlController.disabledTextColor
        font.pixelSize: 10
    }
}
