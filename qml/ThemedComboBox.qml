import QtQuick 2.15
import QtQuick.Controls 2.15

ComboBox {
    id: control
    
    delegate: ItemDelegate {
        width: control.width
        contentItem: Text {
            text: modelData
            color: MaterialEditorQML.textColor
            font: control.font
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }
        highlighted: control && control.highlightedIndex === index
        background: Rectangle {
            color: parent.highlighted ? MaterialEditorQML.highlightColor : MaterialEditorQML.buttonColor
        }
    }
    
    indicator: Canvas {
        id: canvas
        x: control.width - width - control.rightPadding
        y: control.topPadding + (control.availableHeight - height) / 2
        width: 12
        height: 8
        contextType: "2d"
        
        Connections {
            target: control
            function onPressedChanged() { canvas.requestPaint() }
        }
        
        onPaint: {
            context.reset()
            context.moveTo(0, 0)
            context.lineTo(width, 0)
            context.lineTo(width / 2, height)
            context.closePath()
            context.fillStyle = MaterialEditorQML.buttonTextColor
            context.fill()
        }
    }
    
    contentItem: Text {
        leftPadding: 10
        rightPadding: control.indicator.width + control.spacing
        text: control.displayText
        font: control.font
        color: MaterialEditorQML.buttonTextColor
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    
    background: Rectangle {
        implicitWidth: 120
        implicitHeight: 30
        color: MaterialEditorQML.buttonColor
        border.color: MaterialEditorQML.borderColor
        border.width: control.visualFocus ? 2 : 1
        radius: 3
    }
    
    popup: Popup {
        y: control.height - 1
        width: control.width
        implicitHeight: contentItem.implicitHeight
        padding: 1
        
        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.delegateModel
            currentIndex: control ? control.highlightedIndex : 0
            
            ScrollIndicator.vertical: ScrollIndicator { }
        }
        
        background: Rectangle {
            color: MaterialEditorQML.panelColor
            border.color: MaterialEditorQML.borderColor
            border.width: 1
            radius: 3
        }
    }
} 