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

    property var allClips: []          // full [{libIndex,action,name,source,quality,frames,category}]
                                       // (role is libIndex, NOT index — a role named
                                       // "index" shadows the delegate's row index)
    property string filterText: ""
    property int busyIndex: -1         // row currently generating (for UI feedback)
    // Character-category filter. The library mixes human and undead takes
    // under the same action names, so browsing "walk" otherwise interleaves
    // ordinary strides with zombie shambles. "" = show everything.
    property string categoryFilter: ""
    readonly property var categoryOptions: ["All", "Human", "Undead", "Creature"]

    function refresh() {
        allClips = AnimationControlController.listMotionClips()
        rebuildModel()
    }
    function rebuildModel() {
        var f = filterText.trim().toLowerCase()
        var cat = categoryFilter
        listModel.clear()
        for (var i = 0; i < allClips.length; ++i) {
            var c = allClips[i]
            if (cat !== "" && c.category !== cat) continue
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
                text: listModel.count + " of " + dialog.allClips.length
                      + " animations · click Apply to retarget"
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

            // #838: lower the body on crouch/pickup/sit/crawl/death clips
            // (descent-only). ON by default; uncheck to keep the root at
            // standing height. No effect on locomotion clips.
            Row {
                spacing: 14
                Row {
                    spacing: 6
                    Rectangle {
                        id: pickDescentChk
                        property bool checked: true
                        width: 14; height: 14; radius: 2
                        anchors.verticalCenter: parent.verticalCenter
                        color: checked ? PropertiesPanelController.highlightColor
                                       : PropertiesPanelController.inputColor
                        border.color: PropertiesPanelController.borderColor
                        Text { anchors.centerIn: parent; visible: parent.checked
                               text: "✓"; color: "white"; font.pixelSize: 10 }
                        MouseArea { anchors.fill: parent
                                    onClicked: pickDescentChk.checked = !pickDescentChk.checked }
                    }
                    Text {
                        text: "Lower body (crouch/pickup)"
                        color: PropertiesPanelController.textColor; font.pixelSize: 10
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
                // Character category. The corpus mixes human and undead takes
                // under the SAME action names, so an unfiltered "walk" list
                // interleaves ordinary strides with zombie shambles.
                Row {
                    spacing: 6
                    Text {
                        text: "Category"
                        color: PropertiesPanelController.textColor; font.pixelSize: 10
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Repeater {
                        model: dialog.categoryOptions
                        Rectangle {
                            readonly property string catValue:
                                modelData === "All" ? "" : modelData.toLowerCase()
                            readonly property bool active:
                                dialog.categoryFilter === catValue
                            height: 18; radius: 3
                            width: catLabel.implicitWidth + 12
                            anchors.verticalCenter: parent.verticalCenter
                            color: active ? PropertiesPanelController.highlightColor
                                          : PropertiesPanelController.inputColor
                            border.color: PropertiesPanelController.borderColor
                            Text {
                                id: catLabel
                                anchors.centerIn: parent
                                text: modelData
                                font.pixelSize: 10
                                color: active ? "white"
                                              : PropertiesPanelController.textColor
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: { dialog.categoryFilter = catValue
                                             dialog.rebuildModel() }
                            }
                        }
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
                                width: parent.width - applyBtn.width - 22
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 1
                                Text {
                                    text: model.name
                                    color: PropertiesPanelController.textColor
                                    font.pixelSize: 11; elide: Text.ElideRight
                                    width: parent.width
                                }
                                Text {
                                    // `applicable` is only set for creature
                                    // clips once a creature rig is selected;
                                    // undefined means "unknown", which must
                                    // read as normal rather than as a warning.
                                    text: "q " + model.quality.toFixed(2) + " · " + model.frames + "f"
                                          + (model.applicable === false
                                             ? " · needs a " + model.skeleton + " rig" : "")
                                    color: PropertiesPanelController.textColor
                                    opacity: model.applicable === false ? 0.75 : 0.5
                                    font.pixelSize: 9
                                }
                            }
                            Rectangle {
                                id: applyBtn
                                width: 58; height: 24; radius: 3
                                anchors.verticalCenter: parent.verticalCenter
                                property bool busy: dialog.busyIndex === model.libIndex
                                // Dim, but do NOT disable: the plan check is
                                // a heuristic on bone names, so a user who
                                // knows better should still be able to try —
                                // the retarget refuses safely with a clear
                                // message if it really cannot map.
                                property bool mismatched: model.applicable === false
                                opacity: busy ? 0.5 : (mismatched ? 0.45 : 1.0)
                                color: applyMa.pressed ? Qt.darker(PropertiesPanelController.highlightColor, 1.2)
                                     : applyMa.containsMouse ? Qt.lighter(PropertiesPanelController.highlightColor, 1.1)
                                     : PropertiesPanelController.highlightColor
                                Text { anchors.centerIn: parent
                                       text: applyBtn.busy ? "…" : "Apply"
                                       color: "white"; font.pixelSize: 10 }
                                MouseArea {
                                    id: applyMa; anchors.fill: parent; hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: dialog.applyClip(model.libIndex, model.name)
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
        // generateMotion runs synchronously on the GUI thread — defer it one
        // event-loop turn so the busy status above actually paints first.
        Qt.callLater(function() {
            // variantIndex forces this exact clip (template path, no random
            // pick). The picker's own descent checkbox (#838) is the last arg.
            var r = AnimationControlController.generateMotion("", 0.0, false, 0.0, true, idx,
                                                              pickDescentChk.checked)
            dialog.busyIndex = -1
            if (r && r.ok) {
                pickStatus.text = "Applied: " + name
                dialog.applied(r.animation || "", r.entity || "")
            } else {
                pickStatus.isError = true
                pickStatus.text = (r && r.error) ? r.error : "Failed to apply."
            }
        })
    }
}
