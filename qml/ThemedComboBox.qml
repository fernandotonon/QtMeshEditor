import QtQuick
import QtQuick.Controls
import QtQuick.Window
import ThemeManager 1.0

// Themed ComboBox matching the look of the typeahead dropdowns elsewhere
// in the inspector (filter row + list with hover highlight). Pulls colors
// from ThemeManager so it works in any panel.
ComboBox {
    id: control

    delegate: ItemDelegate {
        id: itemDelegate
        width: control.width
        implicitHeight: 22
        padding: 0
        leftPadding: 6
        rightPadding: 6
        contentItem: Text {
            text: modelData
            color: ThemeManager.textColor
            font: control.font
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }
        highlighted: control && control.highlightedIndex === index
        background: Rectangle {
            color: itemDelegate.highlighted ? ThemeManager.highlightColor : "transparent"
        }
    }

    indicator: Canvas {
        id: canvas
        x: control.width - width - control.rightPadding
        y: control.topPadding + (control.availableHeight - height) / 2
        width: 9
        height: 5
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
            context.fillStyle = ThemeManager.textColor
            context.fill()
        }
    }

    contentItem: Text {
        leftPadding: 6
        rightPadding: control.indicator.width + control.spacing + 4
        text: control.displayText
        font: control.font
        color: ThemeManager.textColor
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        implicitWidth: 120
        implicitHeight: 22
        color: ThemeManager.inputColor
        border.color: ThemeManager.borderColor
        border.width: control.visualFocus ? 2 : 1
        radius: 3
    }

    // Pop directly under the input (no gap) — matches the SceneTreeNode
    // material typeahead. Default ComboBox.popup leaves ~5px gap on macOS.
    // Cap height so long lists scroll inside the popup instead of getting
    // clipped by parent bounds (the curve editor's bake combo hits this
    // with 10+ items in a small editor area).
    popup: Popup {
        // Use a native top-level window for the popup so it escapes
        // the host QQuickWidget's bounds. Long bake/reduce lists in
        // the curve editor's narrow strip would otherwise be clipped.
        // popupType: Popup.Window is Qt 6.8+ — falls back gracefully
        // when unsupported, but on Qt 6.9 (our build) it's the
        // canonical fix for QQuickWidget popup clipping.
        popupType: Popup.Window
        y: control.height
        width: control.width
        implicitHeight: Math.min(contentItem.implicitHeight + 2, 240)
        padding: 1

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.delegateModel
            currentIndex: control ? control.highlightedIndex : 0
            // Don't auto-scroll to currentIndex: that bound to
            // control.highlightedIndex keeps snapping the view back
            // to the top when the user scrolls (highlightedIndex stays
            // 0 between hovers). Visual highlight is delegate-driven.
            highlightFollowsCurrentItem: false
            ScrollIndicator.vertical: ScrollIndicator { }
        }

        background: Rectangle {
            color: ThemeManager.inputColor
            border.color: ThemeManager.borderColor
            border.width: 1
            radius: 3
        }
    }
}
