import QtQuick 2.15
import QtQuick.Controls 2.15

Button {
    background: Rectangle {
        color: parent.enabled ? (parent.hovered ? Qt.lighter(MaterialEditorQML.buttonColor, 1.2) : MaterialEditorQML.buttonColor) : Qt.darker(MaterialEditorQML.buttonColor, 1.5)
        border.color: MaterialEditorQML.borderColor
        border.width: 1
        radius: 3
        
        Rectangle {
            anchors.fill: parent
            anchors.margins: 1
            color: "transparent"
            border.color: parent.enabled && parent.parent.pressed ? Qt.lighter(MaterialEditorQML.borderColor, 1.5) : "transparent"
            border.width: 1
            radius: 2
        }
    }
    
    contentItem: Text {
        text: parent.text
        font: parent.font
        color: parent.enabled ? MaterialEditorQML.buttonTextColor : Qt.darker(MaterialEditorQML.buttonTextColor, 2.0)
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
} 