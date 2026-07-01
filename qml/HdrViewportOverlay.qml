import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import HdrEnvironment 1.0
import ThemeManager 1.0

Rectangle {
    id: root
    width: 196
    height: 118
    radius: 4
    color: ThemeManager.panelColor
    border.color: ThemeManager.borderColor
    border.width: 1
    opacity: 0.94

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 6
        spacing: 4

        Text {
            text: "HDR"
            color: ThemeManager.textColor
            font.pixelSize: 10
            font.bold: true
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 4

            Text {
                text: "Tonemap"
                color: ThemeManager.textColor
                font.pixelSize: 9
                Layout.preferredWidth: 52
            }
            ComboBox {
                id: tonemapCombo
                Layout.fillWidth: true
                implicitHeight: 22
                model: ["Reinhard", "ACES", "AgX"]
                currentIndex: HdrEnvironmentController.tonemapOperator
                enabled: HdrEnvironmentController.hasEnvironment
                onActivated: HdrEnvironmentController.tonemapOperator = currentIndex
                Connections {
                    target: HdrEnvironmentController
                    function onTonemapChanged() {
                        tonemapCombo.currentIndex = HdrEnvironmentController.tonemapOperator
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 4

            Text {
                text: "Exposure"
                color: ThemeManager.textColor
                font.pixelSize: 9
                Layout.preferredWidth: 52
            }
            Slider {
                id: exposureSlider
                Layout.fillWidth: true
                from: -4
                to: 4
                stepSize: 0.1
                value: HdrEnvironmentController.exposureEv
                enabled: HdrEnvironmentController.hasEnvironment
                onMoved: HdrEnvironmentController.exposureEv = value
                Connections {
                    target: HdrEnvironmentController
                    function onTonemapChanged() {
                        exposureSlider.value = HdrEnvironmentController.exposureEv
                    }
                }
            }
            Text {
                text: exposureSlider.value.toFixed(1) + " EV"
                color: ThemeManager.textColor
                font.pixelSize: 9
                Layout.preferredWidth: 36
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            CheckBox {
                id: overrideCheck
                text: "Override"
                enabled: HdrEnvironmentController.hasEnvironment
                checked: HdrEnvironmentController.activeTonemapOverride
                onToggled: HdrEnvironmentController.activeTonemapOverride = checked
                Connections {
                    target: HdrEnvironmentController
                    function onViewportOverridesChanged() {
                        overrideCheck.checked = HdrEnvironmentController.activeTonemapOverride
                    }
                }
            }

            Button {
                text: "Reset"
                implicitHeight: 22
                enabled: HdrEnvironmentController.hasEnvironment
                onClicked: HdrEnvironmentController.resetTonemap()
            }
        }
    }
}
