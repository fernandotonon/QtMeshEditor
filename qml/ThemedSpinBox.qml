import QtQuick 2.15
import QtQuick.Controls 2.15

SpinBox {
    contentItem: TextInput {
        z: 2
        text: parent.textFromValue(parent.value, parent.locale)
        font: parent.font
        color: MaterialEditorQML.textColor
        selectionColor: Qt.rgba(0.4, 0.4, 0.6, 1.0)
        selectedTextColor: MaterialEditorQML.textColor
        horizontalAlignment: Qt.AlignHCenter
        verticalAlignment: Qt.AlignVCenter
        readOnly: !parent.editable
        validator: parent.validator
        inputMethodHints: Qt.ImhFormattedNumbersOnly
    }
    
    up.indicator: Rectangle {
        x: parent.mirrored ? 0 : parent.width - width
        height: parent.height / 2
        implicitWidth: 25
        implicitHeight: 15
        color: parent.up.pressed ? Qt.darker(MaterialEditorQML.buttonColor, 1.2) : (parent.up.hovered ? Qt.lighter(MaterialEditorQML.buttonColor, 1.2) : MaterialEditorQML.buttonColor)
        border.color: MaterialEditorQML.borderColor
        border.width: 1
        
        Text {
            text: "+"
            font.pixelSize: parent.height * 0.6
            color: MaterialEditorQML.buttonTextColor
            anchors.centerIn: parent
        }
    }
    
    down.indicator: Rectangle {
        x: parent.mirrored ? 0 : parent.width - width
        y: parent.height / 2
        height: parent.height / 2
        implicitWidth: 25
        implicitHeight: 15
        color: parent.down.pressed ? Qt.darker(MaterialEditorQML.buttonColor, 1.2) : (parent.down.hovered ? Qt.lighter(MaterialEditorQML.buttonColor, 1.2) : MaterialEditorQML.buttonColor)
        border.color: MaterialEditorQML.borderColor
        border.width: 1
        
        Text {
            text: "-"
            font.pixelSize: parent.height * 0.6
            color: MaterialEditorQML.buttonTextColor
            anchors.centerIn: parent
        }
    }
    
    background: Rectangle {
        implicitWidth: 100
        implicitHeight: 30
        color: MaterialEditorQML.panelColor
        border.color: MaterialEditorQML.borderColor
        border.width: 1
        radius: 3
    }
} 