import QtQuick
import QtQuick.Controls
import PropertiesPanel 1.0

Row {
    id: root
    property string label: "X"
    property real value: 0
    property color color: "#c04040"
    property real step: 0.1
    property int decimals: step >= 1 ? 0 : 3
    property int labelWidth: 16
    property int inputWidth: -1
    signal newValue(real val)

    spacing: 2
    width: (parent ? parent.width : 180) / 3 - 3

    Rectangle {
        width: root.labelWidth
        height: 22
        color: root.color
        radius: 2

        Text {
            anchors.centerIn: parent
            text: root.label
            color: "white"
            font.pixelSize: 10
            font.bold: true
        }
    }

    Rectangle {
        id: inputBg
        width: root.inputWidth >= 0
            ? root.inputWidth
            : Math.max(0, parent.width - root.labelWidth - root.spacing)
        height: 22
        color: PropertiesPanelController.inputColor
        border.color: input.activeFocus ? root.color : PropertiesPanelController.borderColor
        border.width: 1
        radius: 2

        TextInput {
            id: input
            anchors.left: parent.left
            anchors.right: arrows.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.margins: 2
            text: root.value.toFixed(root.decimals)
            color: PropertiesPanelController.textColor
            font.pixelSize: 11
            verticalAlignment: TextInput.AlignVCenter
            selectByMouse: true
            clip: true
            validator: DoubleValidator { decimals: root.decimals > 0 ? root.decimals + 1 : 0 }

            onEditingFinished: {
                var v = parseFloat(text)
                if (!isNaN(v))
                    root.newValue(v)
            }

            Keys.onUpPressed: {
                var v = parseFloat(text)
                if (!isNaN(v)) {
                    v += root.step
                    text = v.toFixed(root.decimals)
                    root.newValue(v)
                }
            }
            Keys.onDownPressed: {
                var v = parseFloat(text)
                if (!isNaN(v)) {
                    v -= root.step
                    text = v.toFixed(root.decimals)
                    root.newValue(v)
                }
            }
        }

        // Up/Down arrow buttons
        Column {
            id: arrows
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 14

            Rectangle {
                width: parent.width
                height: parent.height / 2
                color: upMouse.pressed ? Qt.darker(PropertiesPanelController.panelColor, 1.2)
                     : upMouse.containsMouse ? Qt.lighter(PropertiesPanelController.panelColor, 1.2)
                     : PropertiesPanelController.panelColor
                border.color: PropertiesPanelController.borderColor
                border.width: 1

                Text {
                    anchors.centerIn: parent
                    text: "\u25B2"
                    font.pixelSize: 6
                    color: PropertiesPanelController.textColor
                }

                MouseArea {
                    id: upMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: {
                        var v = parseFloat(input.text)
                        if (!isNaN(v)) {
                            v += root.step
                            input.text = v.toFixed(root.decimals)
                            root.newValue(v)
                        }
                    }
                }
            }

            Rectangle {
                width: parent.width
                height: parent.height / 2
                color: downMouse.pressed ? Qt.darker(PropertiesPanelController.panelColor, 1.2)
                     : downMouse.containsMouse ? Qt.lighter(PropertiesPanelController.panelColor, 1.2)
                     : PropertiesPanelController.panelColor
                border.color: PropertiesPanelController.borderColor
                border.width: 1

                Text {
                    anchors.centerIn: parent
                    text: "\u25BC"
                    font.pixelSize: 6
                    color: PropertiesPanelController.textColor
                }

                MouseArea {
                    id: downMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: {
                        var v = parseFloat(input.text)
                        if (!isNaN(v)) {
                            v -= root.step
                            input.text = v.toFixed(root.decimals)
                            root.newValue(v)
                        }
                    }
                }
            }
        }
    }
}
