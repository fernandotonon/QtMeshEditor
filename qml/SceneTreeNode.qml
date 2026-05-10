import QtQuick
import QtQuick.Controls
import PropertiesPanel 1.0

Column {
    id: treeNode

    property var nodeIndex
    property var treeModel
    property int indentLevel: 0

    property bool expanded: false  // All nodes start collapsed to avoid loading full subtree
    property int childCount: treeModel ? treeModel.rowCount(nodeIndex) : 0
    property bool hasChildren: childCount > 0
    property string nodeName: treeModel ? (treeModel.data(nodeIndex) || "") : ""
    property bool selected: false
    // Only Node-type items are draggable (not entities/submeshes)
    property bool isNodeType: treeModel ? (treeModel.data(nodeIndex, 259) === "Node" || treeModel.data(nodeIndex, 259) === "Group") : false

    width: parent ? parent.width : 200

    Component.onCompleted: refreshSelected()

    function refreshSelected() {
        if (treeModel && nodeIndex)
            selected = treeModel.isSelected(nodeIndex.row, treeModel.parent(nodeIndex))
    }

    function typeLabel() {
        return treeModel ? (treeModel.data(nodeIndex, 259) || "") : ""
    }

    function impactBadgeText() {
        switch (typeLabel()) {
        case "Node":
        case "Group":
            return "PLACEMENT"
        case "Mesh":
            return "MESH DATA"
        case "Submesh":
            return "SUBMESH"
        default:
            return ""
        }
    }

    function impactBadgeColor() {
        switch (typeLabel()) {
        case "Node":
        case "Group":
            return "#6ca0dc"
        case "Mesh":
            return "#55b65a"
        case "Submesh":
            return "#c9b64f"
        default:
            return PropertiesPanelController.borderColor
        }
    }

    Connections {
        target: treeModel
        function onSelectionUpdated() { treeNode.refreshSelected() }
    }

    // Row for this node
    Rectangle {
        id: nodeRow
        width: treeNode.width
        height: 22
        color: treeNode.selected
               ? PropertiesPanelController.highlightColor
               : (rowMouse.containsMouse ? Qt.lighter(PropertiesPanelController.panelColor, 1.15)
                                         : "transparent")

        // Full-row mouse area for selection
        MouseArea {
            id: rowMouse
            anchors.fill: parent
            hoverEnabled: true

            onClicked: function(mouse) {
                if (treeModel) {
                    var multiSelect = (mouse.modifiers & Qt.ControlModifier) ||
                                      (mouse.modifiers & Qt.ShiftModifier)
                    treeModel.selectItem(nodeIndex.row, treeModel.parent(nodeIndex), multiSelect)
                }
            }

        }

        Row {
            id: treeRowContent
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.leftMargin: 4 + indentLevel * 16
            spacing: 4
            z: 10  // Above drop areas

            // Expand/collapse chevron button
            Item {
                width: 14
                height: 22

                Text {
                    visible: treeNode.hasChildren
                    anchors.centerIn: parent
                    text: treeNode.expanded ? "\u25BC" : "\u25B6"
                    color: treeNode.selected ? "white" : PropertiesPanelController.textColor
                    font.pixelSize: 8
                }

                MouseArea {
                    anchors.fill: parent
                    visible: treeNode.hasChildren
                    z: 10  // Above rowMouse
                    onClicked: treeNode.expanded = !treeNode.expanded
                }
            }

            // Icon based on type
            Text {
                text: {
                    if (!treeModel) return ""
                    var t = treeModel.data(nodeIndex, 259)
                    switch(t) {
                    case "Node":    return "\u25A0"
                    case "Mesh":    return "\u25C6"
                    case "Submesh": return "\u25CB"
                    default:        return "\u25A1"
                    }
                }
                color: {
                    if (treeNode.selected) return "white"
                    if (!treeModel) return PropertiesPanelController.textColor
                    var t = treeModel.data(nodeIndex, 259)
                    switch(t) {
                    case "Node":    return "#6ca0dc"
                    case "Mesh":    return "#6cdc6c"
                    case "Submesh": return "#dcdc6c"
                    default:        return PropertiesPanelController.textColor
                    }
                }
                font.pixelSize: 10
                anchors.verticalCenter: parent.verticalCenter
            }

            // Name + type label
            Text {
                id: nameLabel
                text: {
                    if (!treeModel) return ""
                    var n = treeNode.nodeName
                    var t = treeModel.data(nodeIndex, 259)
                    return n + (t ? " (" + t + ")" : "")
                }
                color: treeNode.selected ? "white" : PropertiesPanelController.textColor
                font.pixelSize: 11
                elide: Text.ElideRight
                width: Math.max(40, treeNode.width - treeRowContent.anchors.leftMargin
                                - 14 - 10 - 4
                                - (treeNode.impactBadgeText() !== "" ? badgeText.implicitWidth + 12 : 0)
                                - (matSelector.visible ? matSelector.width + 4 : 0)
                                - (treeNode.isNodeType ? 26 : 0)
                                - 8)
                anchors.verticalCenter: parent.verticalCenter
            }

            Rectangle {
                visible: treeNode.impactBadgeText() !== ""
                width: visible ? badgeText.implicitWidth + 8 : 0
                height: 16
                radius: 3
                color: treeNode.selected ? Qt.rgba(1, 1, 1, 0.18) : Qt.rgba(0, 0, 0, 0)
                border.color: treeNode.selected ? "white" : treeNode.impactBadgeColor()
                border.width: 1
                anchors.verticalCenter: parent.verticalCenter

                Text {
                    id: badgeText
                    anchors.centerIn: parent
                    text: treeNode.impactBadgeText()
                    color: treeNode.selected ? "white" : treeNode.impactBadgeColor()
                    font.pixelSize: 8
                    font.bold: true
                }
            }

            // Material selector (typeahead combo for submeshes)
            Item {
                id: matSelector
                visible: treeModel ? treeModel.data(nodeIndex, 259) === "Submesh" : false
                width: visible ? Math.max(90, matLabel.implicitWidth + 20) : 0
                height: 18
                anchors.verticalCenter: parent.verticalCenter

                property string currentMat: treeModel ? (treeModel.materialName(nodeIndex.row, treeModel.parent(nodeIndex)) || "") : ""
                property bool dropdownOpen: false

                // Display label (click to open dropdown)
                Text {
                    id: matLabel
                    anchors.verticalCenter: parent.verticalCenter
                    text: "[" + matSelector.currentMat + "]"
                    color: treeNode.selected ? Qt.lighter(PropertiesPanelController.highlightColor, 1.5)
                                             : PropertiesPanelController.highlightColor
                    font.pixelSize: 10
                    font.italic: true

                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: -2
                        z: 10
                        onClicked: {
                            matSelector.dropdownOpen = !matSelector.dropdownOpen
                            if (matSelector.dropdownOpen) {
                                matFilter.text = ""
                                matFilter.forceActiveFocus()
                            }
                        }
                    }
                }
            }

            Item {
                width: treeNode.isNodeType ? 22 : 0
                height: 22
                visible: treeNode.isNodeType
                anchors.verticalCenter: parent.verticalCenter

                Text {
                    anchors.centerIn: parent
                    text: "\uD83D\uDDD1"
                    font.pixelSize: 12
                    color: treeNode.selected ? "white" : PropertiesPanelController.textColor
                }
                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    z: 20
                    onClicked: PropertiesPanelController.deleteSceneTreeNode(treeNode.nodeName)
                }
            }
        }
    }

    // Material typeahead dropdown (outside Row to avoid clipping)
    Popup {
        id: matDropdown
        visible: matSelector.dropdownOpen
        x: 4 + indentLevel * 16
        y: 22
        width: Math.min(treeNode.width - x, 220)
        height: Math.min(matFilteredList.contentHeight + 30, 160)
        padding: 0
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
        onClosed: matSelector.dropdownOpen = false

        background: Rectangle {
            color: PropertiesPanelController.inputColor
            border.color: PropertiesPanelController.borderColor
            border.width: 1
            radius: 3
        }

        Column {
            anchors.fill: parent
            spacing: 0

            // Filter input
            Rectangle {
                width: parent.width
                height: 24
                color: PropertiesPanelController.panelColor
                border.color: PropertiesPanelController.borderColor
                border.width: 1
                radius: 3

                TextInput {
                    id: matFilter
                    anchors.fill: parent
                    anchors.margins: 4
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 11
                    clip: true
                    verticalAlignment: TextInput.AlignVCenter

                    property var allMaterials: treeModel ? treeModel.availableMaterials() : []
                    property var filtered: {
                        var query = text.toLowerCase()
                        if (query.length === 0) return allMaterials
                        var result = []
                        for (var i = 0; i < allMaterials.length; i++) {
                            if (allMaterials[i].toLowerCase().indexOf(query) >= 0)
                                result.push(allMaterials[i])
                        }
                        return result
                    }

                    Keys.onEscapePressed: matSelector.dropdownOpen = false
                    Keys.onReturnPressed: {
                        if (filtered.length > 0) {
                            treeModel.setMaterial(nodeIndex.row, treeModel.parent(nodeIndex), filtered[0])
                            matSelector.currentMat = filtered[0]
                            matSelector.dropdownOpen = false
                        }
                    }
                }

                Text {
                    anchors.fill: parent
                    anchors.margins: 4
                    text: "Type to filter..."
                    color: PropertiesPanelController.borderColor
                    font.pixelSize: 11
                    font.italic: true
                    visible: matFilter.text.length === 0 && !matFilter.activeFocus
                    verticalAlignment: Text.AlignVCenter
                }
            }

            // Material list
            ListView {
                id: matFilteredList
                width: parent.width
                height: parent.height - 24
                model: matFilter.filtered
                clip: true

                delegate: Rectangle {
                    width: matFilteredList.width
                    height: 20
                    color: matDelegateMouse.containsMouse
                           ? PropertiesPanelController.highlightColor
                           : "transparent"

                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 6
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData
                        color: matDelegateMouse.containsMouse
                               ? "white"
                               : PropertiesPanelController.textColor
                        font.pixelSize: 10
                        elide: Text.ElideRight
                    }

                    MouseArea {
                        id: matDelegateMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            treeModel.setMaterial(nodeIndex.row, treeModel.parent(nodeIndex), modelData)
                            matSelector.currentMat = modelData
                            matSelector.dropdownOpen = false
                        }
                    }
                }
            }
        }
    }

    // Children (using Loader to break recursion)
    Loader {
        active: treeNode.expanded && treeNode.hasChildren
        visible: active
        width: treeNode.width

        sourceComponent: Component {
            Column {
                width: treeNode.width

                Repeater {
                    model: treeNode.childCount

                    Loader {
                        width: treeNode.width
                        source: "qrc:/PropertiesPanel/SceneTreeNode.qml"
                        asynchronous: false
                        onLoaded: {
                            item.nodeIndex = treeModel.index(index, 0, treeNode.nodeIndex)
                            item.treeModel = treeNode.treeModel
                            item.indentLevel = treeNode.indentLevel + 1
                            item.width = Qt.binding(function() { return treeNode.width })
                            if (item.refreshSelected)
                                item.refreshSelected()
                        }
                    }
                }
            }
        }
    }
}
