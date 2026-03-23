import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import PropertiesPanel 1.0
import AnimationTimeline 1.0

Rectangle {
    id: root
    color: PropertiesPanelController.panelColor
    visible: AnimationTimelineController.hasAnimations
    implicitHeight: 120

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 6
        spacing: 4

        // Animation selector + playback controls
        RowLayout {
            spacing: 8
            Layout.fillWidth: true

            ComboBox {
                id: animCombo
                model: AnimationTimelineController.animationNames
                currentIndex: {
                    var names = AnimationTimelineController.animationNames
                    return names.indexOf(AnimationTimelineController.currentAnimation)
                }
                onActivated: AnimationTimelineController.currentAnimation = currentText

                Layout.fillWidth: true
                Layout.maximumWidth: 200

                background: Rectangle {
                    color: PropertiesPanelController.inputColor
                    border.color: PropertiesPanelController.borderColor
                    border.width: 1
                    radius: 3
                    implicitHeight: 26
                }
                contentItem: Text {
                    text: animCombo.displayText
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 11
                    verticalAlignment: Text.AlignVCenter
                    leftPadding: 6
                }
            }

            Button {
                text: AnimationTimelineController.playing ? "\u275A\u275A" : "\u25B6"
                onClicked: {
                    if (AnimationTimelineController.playing)
                        AnimationTimelineController.pause()
                    else
                        AnimationTimelineController.play()
                }
                implicitWidth: 32
                implicitHeight: 26

                background: Rectangle {
                    color: parent.pressed ? Qt.darker(PropertiesPanelController.headerColor, 1.2)
                                          : PropertiesPanelController.headerColor
                    border.color: PropertiesPanelController.borderColor
                    radius: 3
                }
                contentItem: Text {
                    text: parent.text
                    color: PropertiesPanelController.textColor
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.pixelSize: 12
                }
            }

            Button {
                text: "\u25A0"
                onClicked: AnimationTimelineController.stop()
                implicitWidth: 32
                implicitHeight: 26

                background: Rectangle {
                    color: parent.pressed ? Qt.darker(PropertiesPanelController.headerColor, 1.2)
                                          : PropertiesPanelController.headerColor
                    border.color: PropertiesPanelController.borderColor
                    radius: 3
                }
                contentItem: Text {
                    text: parent.text
                    color: PropertiesPanelController.textColor
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.pixelSize: 12
                }
            }

            Text {
                text: {
                    var pos = AnimationTimelineController.progress.toFixed(2)
                    var dur = AnimationTimelineController.duration.toFixed(2)
                    return pos + " / " + dur + "s"
                }
                color: PropertiesPanelController.textColor
                font.pixelSize: 11
            }
        }

        // Timeline scrubber
        Slider {
            id: scrubber
            Layout.fillWidth: true
            from: 0
            to: Math.max(AnimationTimelineController.duration, 0.01)
            value: AnimationTimelineController.progress
            onMoved: AnimationTimelineController.progress = value

            background: Rectangle {
                x: scrubber.leftPadding
                y: scrubber.topPadding + scrubber.availableHeight / 2 - height / 2
                width: scrubber.availableWidth
                height: 4
                radius: 2
                color: PropertiesPanelController.borderColor

                Rectangle {
                    width: scrubber.visualPosition * parent.width
                    height: parent.height
                    color: PropertiesPanelController.highlightColor
                    radius: 2
                }
            }

            handle: Rectangle {
                x: scrubber.leftPadding + scrubber.visualPosition * (scrubber.availableWidth - width)
                y: scrubber.topPadding + scrubber.availableHeight / 2 - height / 2
                width: 12
                height: 12
                radius: 6
                color: scrubber.pressed ? Qt.lighter(PropertiesPanelController.highlightColor, 1.2)
                                        : PropertiesPanelController.highlightColor
                border.color: PropertiesPanelController.borderColor
            }
        }
    }
}
