import QtQuick
import QtQuick.Controls
import QtQuick.Window

Rectangle {
    id: selector
    property string mode: "source"
    property real textScale: 1
    property color textColor
    property color accentColor
    signal modeChosen(string mode)

    implicitWidth: buttons.width + 4
    implicitHeight: buttons.height + 4
    radius: 5
    color: Qt.rgba(textColor.r, textColor.g, textColor.b, 0.06)
    border.color: Qt.rgba(textColor.r, textColor.g, textColor.b, 0.2)

    component ModeButton: ToolButton {
        id: button
        property string view
        property string title
        width: Math.round(32 * selector.textScale)
        height: Math.round(28 * selector.textScale)
        checkable: true
        autoExclusive: true
        checked: selector.mode === view
        Accessible.name: title
        ToolTip.visible: hovered
        ToolTip.delay: 500
        ToolTip.text: title
        onClicked: selector.modeChosen(view)
        background: Rectangle {
            radius: 3
            color: button.checked || button.hovered || button.activeFocus
                ? Qt.rgba(selector.accentColor.r, selector.accentColor.g,
                          selector.accentColor.b, button.checked ? 0.22 : 0.1) : "transparent"
            border.color: button.activeFocus ? selector.accentColor : "transparent"
        }
        contentItem: Item {
            Canvas {
                id: icon
                readonly property real dpr: Screen.devicePixelRatio
                readonly property color ink: button.checked ? selector.accentColor : selector.textColor
                width: 18 * selector.textScale * dpr
                height: 18 * selector.textScale * dpr
                x: (parent.width - width / dpr) / 2
                y: (parent.height - height / dpr) / 2
                transformOrigin: Item.TopLeft
                scale: 1 / dpr
                onInkChanged: requestPaint()
                onWidthChanged: requestPaint()
                onPaint: {
                    var c = getContext("2d");
                    var factor = selector.textScale * dpr;
                    c.setTransform(factor, 0, 0, factor, 0, 0);
                    c.clearRect(0, 0, 18, 18);
                    c.strokeStyle = ink;
                    c.lineWidth = 1.4;
                    c.lineCap = "round";
                    c.lineJoin = "round";
                    c.beginPath();
                    if (button.view === "source") {
                        c.moveTo(5, 5); c.lineTo(1, 9); c.lineTo(5, 13);
                        c.moveTo(13, 5); c.lineTo(17, 9); c.lineTo(13, 13);
                        c.moveTo(11, 3); c.lineTo(7, 15);
                    } else if (button.view === "split") {
                        c.rect(1.5, 3, 15, 12);
                        c.moveTo(9, 3); c.lineTo(9, 15);
                    } else {
                        c.moveTo(1, 9);
                        c.bezierCurveTo(5, 2, 13, 2, 17, 9);
                        c.bezierCurveTo(13, 16, 5, 16, 1, 9);
                        c.moveTo(11.5, 9); c.arc(9, 9, 2.5, 0, Math.PI * 2);
                    }
                    c.stroke();
                }
            }
        }
    }

    Row {
        id: buttons
        anchors.centerIn: parent
        ModeButton { objectName: "sourceModeButton"; view: "source"; title: "Source" }
        ModeButton { objectName: "splitModeButton"; view: "split"; title: "Split view" }
        ModeButton { objectName: "previewModeButton"; view: "preview"; title: "Preview" }
    }
}
