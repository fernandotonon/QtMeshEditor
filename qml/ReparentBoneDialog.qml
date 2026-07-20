import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import PropertiesPanel 1.0

Window {
    id: dialog
    title: "Reparent Bone"
    width: 400
    height: 360
    minimumWidth: 360
    minimumHeight: 300
    flags: Qt.Dialog
    modality: Qt.ApplicationModal
    color: PropertiesPanelController.panelColor

    property string boneName: ""
    property string currentParent: ""
    property var parentCandidates: []
    property string selectedParent: ""

    signal reparentRequested(string newParentName, bool keepWorld)
    signal cancelled()

    function openForBone(name, candidates, currentParentName) {
        dialog.boneName = name || ""
        dialog.currentParent = currentParentName || ""
        dialog.parentCandidates = candidates || []
        dialog.selectedParent = dialog.parentCandidates.length > 0
            ? dialog.parentCandidates[0] : ""
        parentFilter.text = ""
        parentPicker.dropdownOpen = true
        keepWorldCheck.checked = true
        dialog.show()
        dialog.raise()
        dialog.requestActivate()
    }

    function submit() {
        if (dialog.selectedParent.length === 0)
            return
        dialog.reparentRequested(dialog.selectedParent, keepWorldCheck.checked)
        dialog.close()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            font.pixelSize: 12
            color: PropertiesPanelController.textColor
            text: dialog.boneName.length > 0
                ? "Set a new parent for \"" + dialog.boneName + "\":"
                : "Set a new parent for the selected bone:"
        }

        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            font.pixelSize: 11
            color: PropertiesPanelController.textColor
            opacity: 0.85
            text: dialog.currentParent.length > 0
                ? "Current parent: " + dialog.currentParent
                : "Current parent: (root)"
        }

        Text {
            text: "New parent"
            color: PropertiesPanelController.textColor
            font.pixelSize: 11
            opacity: 0.8
        }

        // Searchable bone picker (same pattern as Inspector bone dropdown)
        Item {
            id: parentPicker
            Layout.fillWidth: true
            Layout.preferredHeight: 24
            Layout.fillHeight: dropdownOpen
            property bool dropdownOpen: true

            ColumnLayout {
                anchors.fill: parent
                spacing: 4

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 24
                    radius: 3
                    color: PropertiesPanelController.inputColor
                    border.color: parentPicker.dropdownOpen
                        ? PropertiesPanelController.highlightColor
                        : PropertiesPanelController.borderColor
                    border.width: 1

                    Row {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 4
                        spacing: 4
                        Text {
                            text: dialog.selectedParent.length > 0
                                ? dialog.selectedParent : "(none)"
                            color: PropertiesPanelController.textColor
                            font.pixelSize: 11
                            elide: Text.ElideRight
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - 18
                        }
                        Text {
                            text: parentPicker.dropdownOpen ? "\u25B2" : "\u25BC"
                            color: PropertiesPanelController.textColor
                            font.pixelSize: 8
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            parentPicker.dropdownOpen = !parentPicker.dropdownOpen
                            if (parentPicker.dropdownOpen) {
                                parentFilter.text = ""
                                parentFilter.forceActiveFocus()
                            }
                        }
                    }
                }

                Rectangle {
                    visible: parentPicker.dropdownOpen
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: 160
                    color: PropertiesPanelController.inputColor
                    border.color: PropertiesPanelController.borderColor
                    border.width: 1
                    radius: 3

                    Column {
                        anchors.fill: parent
                        anchors.margins: 1
                        spacing: 0

                        Rectangle {
                            width: parent.width
                            height: 26
                            color: PropertiesPanelController.panelColor
                            border.color: PropertiesPanelController.borderColor
                            border.width: 1
                            radius: 3

                            TextInput {
                                id: parentFilter
                                anchors.fill: parent
                                anchors.margins: 4
                                color: PropertiesPanelController.textColor
                                font.pixelSize: 11
                                clip: true
                                verticalAlignment: TextInput.AlignVCenter

                                property var filtered: {
                                    var q = text.toLowerCase()
                                    var bones = dialog.parentCandidates
                                    if (!bones || bones.length === 0) return []
                                    if (q.length === 0) return bones
                                    var r = []
                                    for (var i = 0; i < bones.length; i++)
                                        if (bones[i].toLowerCase().indexOf(q) >= 0)
                                            r.push(bones[i])
                                    return r
                                }

                                Keys.onEscapePressed: parentPicker.dropdownOpen = false
                                Keys.onReturnPressed: {
                                    if (filtered.length > 0) {
                                        dialog.selectedParent = filtered[0]
                                        parentPicker.dropdownOpen = false
                                    }
                                }
                            }

                            Text {
                                anchors.fill: parent
                                anchors.margins: 4
                                text: "Type to filter bones…"
                                font.pixelSize: 11
                                font.italic: true
                                color: PropertiesPanelController.borderColor
                                visible: parentFilter.text.length === 0 && !parentFilter.activeFocus
                                verticalAlignment: Text.AlignVCenter
                            }
                        }

                        ListView {
                            id: parentListView
                            width: parent.width
                            height: parent.height - 26
                            clip: true
                            model: parentFilter.filtered
                            ScrollBar.vertical: ScrollBar {
                                policy: parentListView.contentHeight > parentListView.height
                                    ? ScrollBar.AlwaysOn : ScrollBar.AsNeeded
                            }

                            delegate: Rectangle {
                                width: parentListView.width
                                height: 22
                                color: {
                                    if (parentDelegateMouse.containsMouse)
                                        return PropertiesPanelController.highlightColor
                                    if (modelData === dialog.selectedParent)
                                        return Qt.rgba(
                                            PropertiesPanelController.highlightColor.r,
                                            PropertiesPanelController.highlightColor.g,
                                            PropertiesPanelController.highlightColor.b, 0.35)
                                    return PropertiesPanelController.inputColor
                                }

                                Text {
                                    anchors.left: parent.left
                                    anchors.leftMargin: 8
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: modelData
                                    color: parentDelegateMouse.containsMouse
                                        ? "white" : PropertiesPanelController.textColor
                                    font.pixelSize: 11
                                    elide: Text.ElideRight
                                    width: parent.width - 12
                                }
                                MouseArea {
                                    id: parentDelegateMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        dialog.selectedParent = modelData
                                        parentPicker.dropdownOpen = false
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        CheckBox {
            id: keepWorldCheck
            checked: true
            text: "Keep world transform"
            font.pixelSize: 11
            spacing: 6
            indicator: Rectangle {
                x: keepWorldCheck.leftPadding
                y: keepWorldCheck.height / 2 - height / 2
                implicitWidth: 16
                implicitHeight: 16
                radius: 2
                color: keepWorldCheck.checked
                    ? PropertiesPanelController.highlightColor
                    : PropertiesPanelController.inputColor
                border.color: keepWorldCheck.activeFocus
                    ? PropertiesPanelController.highlightColor
                    : PropertiesPanelController.borderColor
                border.width: keepWorldCheck.activeFocus ? 2 : 1
                Text {
                    anchors.centerIn: parent
                    visible: keepWorldCheck.checked
                    text: "✓"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 12
                    font.bold: true
                }
            }
            contentItem: Text {
                text: keepWorldCheck.text
                color: PropertiesPanelController.textColor
                font.pixelSize: 11
                leftPadding: keepWorldCheck.indicator.width + keepWorldCheck.spacing
                verticalAlignment: Text.AlignVCenter
            }
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: 8
            Rectangle {
                width: cancelLabel.implicitWidth + 20
                height: 28
                radius: 3
                color: PropertiesPanelController.headerColor
                border.color: PropertiesPanelController.borderColor
                Text {
                    id: cancelLabel
                    anchors.centerIn: parent
                    text: "Cancel"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 12
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: { dialog.cancelled(); dialog.close() }
                }
            }
            Rectangle {
                width: okLabel.implicitWidth + 20
                height: 28
                radius: 3
                opacity: dialog.selectedParent.length > 0 ? 1.0 : 0.45
                color: PropertiesPanelController.highlightColor
                Text {
                    id: okLabel
                    anchors.centerIn: parent
                    text: "Reparent"
                    color: "white"
                    font.pixelSize: 12
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: dialog.selectedParent.length > 0
                    cursorShape: Qt.PointingHandCursor
                    onClicked: dialog.submit()
                }
            }
        }
    }
}
