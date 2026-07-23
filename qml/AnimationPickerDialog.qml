import QtQuick
import QtQuick.Controls
import QtQuick.Window
import PropertiesPanel 1.0
import AnimationControl 1.0

// #838: Mixamo-style animation PICKER. Lists every clip in the template
// motion library with a human-readable name (e.g. "Walk (Tired Character)")
// so the user SELECTS a specific clip and applies it to the selected rig,
// instead of the free-text prompt's quality-weighted random pick. A search
// box filters by name/action. Applying calls generateMotion(variantIndex).
// The free-text prompt in the panel stays for the AI-model path only.
Window {
    id: dialog
    title: "Animation Library"
    width: 460
    height: 620
    minimumWidth: 380
    minimumHeight: 420
    flags: Qt.Dialog
    color: PropertiesPanelController.panelColor

    // emitted after a successful apply so the panel can refresh its anim list
    signal applied(string animation, string entity)

    property var allClips: []          // full [{index,action,name,source,quality,frames}]
    property string filterText: ""
    property int busyIndex: -1         // row currently generating (for UI feedback)

    function refresh() {
        allClips = AnimationControlController.listMotionClips()
        rebuildModel()
    }
    function rebuildModel() {
        var f = filterText.trim().toLowerCase()
        listModel.clear()
        for (var i = 0; i < allClips.length; ++i) {
            var c = allClips[i]
            if (f === "" || c.name.toLowerCase().indexOf(f) !== -1
                         || c.action.toLowerCase().indexOf(f) !== -1) {
                listModel.append(c)
            }
        }
    }
    function open() { dialog.show(); dialog.raise(); dialog.requestActivate(); refresh() }

    ListModel { id: listModel }

    Rectangle {
        anchors.fill: parent
        color: PropertiesPanelController.panelColor

        Column {
            anchors.fill: parent
            anchors.margins: 12
            spacing: 8

            Text {
                text: "Pick an animation to apply to the selected rig"
                color: PropertiesPanelController.textColor
                font.pixelSize: 12; font.bold: true
            }
            Text {
                text: dialog.allClips.length + " animations · click Apply to retarget onto your mesh"
                color: PropertiesPanelController.textColor; opacity: 0.6
                font.pixelSize: 10
            }

            // search box
            Rectangle {
                width: parent.width; height: 26; radius: 3
                color: PropertiesPanelController.inputColor
                border.color: searchIn.activeFocus ? PropertiesPanelController.highlightColor
                                                    : PropertiesPanelController.borderColor
                TextInput {
                    id: searchIn
                    anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8
                    verticalAlignment: TextInput.AlignVCenter
                    color: PropertiesPanelController.textColor; font.pixelSize: 11
                    clip: true; selectByMouse: true; activeFocusOnPress: true
                    onTextChanged: { dialog.filterText = text; dialog.rebuildModel() }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: !searchIn.text
                        text: "Search… (walk, run, zombie, dance)"
                        color: PropertiesPanelController.textColor; opacity: 0.4
                        font.pixelSize: 11
                    }
                }
            }

            // the list
            Rectangle {
                width: parent.width
                height: parent.height - y - applyRow.height - 24
                color: PropertiesPanelController.inputColor
                border.color: PropertiesPanelController.borderColor; radius: 3
                clip: true
                ListView {
                    id: lv
                    anchors.fill: parent; anchors.margins: 2
                    model: listModel
                    clip: true
                    ScrollBar.vertical: ScrollBar {}
                    delegate: Rectangle {
                        width: lv.width; height: 40
                        color: rowMa.containsMouse
                               ? Qt.lighter(PropertiesPanelController.panelColor, 1.3)
                               : (index % 2 ? PropertiesPanelController.panelColor
                                            : "transparent")
                        Row {
                            anchors.fill: parent
                            anchors.leftMargin: 8; anchors.rightMargin: 6
                            spacing: 8
                            Column {
                                width: parent.width - applyBtn.width - 14
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 1
                                Text {
                                    text: model.name
                                    color: PropertiesPanelController.textColor
                                    font.pixelSize: 11; elide: Text.ElideRight
                                    width: parent.width
                                }
                                Text {
                                    text: "q " + model.quality.toFixed(2) + " · " + model.frames + "f"
                                    color: PropertiesPanelController.textColor; opacity: 0.5
                                    font.pixelSize: 9
                                }
                            }
                            Rectangle {
                                id: applyBtn
                                width: 58; height: 24; radius: 3
                                anchors.verticalCenter: parent.verticalCenter
                                property bool busy: dialog.busyIndex === model.index
                                opacity: busy ? 0.5 : 1.0
                                color: applyMa.pressed ? Qt.darker(PropertiesPanelController.highlightColor, 1.2)
                                     : applyMa.containsMouse ? Qt.lighter(PropertiesPanelController.highlightColor, 1.1)
                                     : PropertiesPanelController.highlightColor
                                Text { anchors.centerIn: parent
                                       text: applyBtn.busy ? "…" : "Apply"
                                       color: "white"; font.pixelSize: 10 }
                                MouseArea {
                                    id: applyMa; anchors.fill: parent; hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: dialog.applyClip(model.index, model.name)
                                }
                            }
                        }
                        MouseArea {
                            id: rowMa; anchors.fill: parent; hoverEnabled: true
                            acceptedButtons: Qt.NoButton   // hover only; Apply btn handles clicks
                        }
                    }
                }
            }

            Row {
                id: applyRow
                width: parent.width; spacing: 8
                Text {
                    id: pickStatus
                    width: parent.width - closeBtn.width - 8
                    anchors.verticalCenter: parent.verticalCenter
                    color: pickStatus.isError ? "#e08080" : PropertiesPanelController.textColor
                    property bool isError: false
                    font.pixelSize: 10; wrapMode: Text.WordWrap
                    text: ""
                }
                Rectangle {
                    id: closeBtn
                    width: 64; height: 26; radius: 3
                    color: closeMa.containsMouse ? Qt.lighter(PropertiesPanelController.headerColor, 1.2)
                                                 : PropertiesPanelController.headerColor
                    border.color: PropertiesPanelController.borderColor
                    Text { anchors.centerIn: parent; text: "Close"
                           color: PropertiesPanelController.textColor; font.pixelSize: 11 }
                    MouseArea { id: closeMa; anchors.fill: parent; hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor; onClicked: dialog.close() }
                }
            }
        }
    }

    function applyClip(idx, name) {
        if (dialog.busyIndex >= 0) return
        dialog.busyIndex = idx
        pickStatus.isError = false
        pickStatus.text = "Applying " + name + "…"
        // variantIndex forces this exact clip (template path, no random pick).
        var r = AnimationControlController.generateMotion("", 0.0, false, 0.0, true, idx)
        dialog.busyIndex = -1
        if (r && r.ok) {
            pickStatus.text = "Applied: " + name
            dialog.applied(r.animation || "", r.entity || "")
        } else {
            pickStatus.isError = true
            pickStatus.text = (r && r.error) ? r.error : "Failed to apply."
        }
    }
}
