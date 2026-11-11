import QtQuick

Rectangle {
    id: badge

    property alias text: caption.text
    property alias font: caption.font
    property alias textColor: caption.color
    property int horizontalPadding: 6
    property int verticalPadding: 2
    property int minimumHeight: 0
    property bool showDot: false
    property color dotColor: "#94a3b8"
    property bool pulseDot: false

    implicitWidth: caption.implicitWidth + horizontalPadding * 2 + (showDot ? dot.width + 6 : 0)
    implicitHeight: Math.max(minimumHeight, caption.implicitHeight + verticalPadding * 2)
    radius: height / 2
    clip: true

    Rectangle {
        id: dot
        visible: badge.showDot
        width: 6
        height: 6
        radius: 3
        color: badge.dotColor
        x: badge.horizontalPadding
        anchors.verticalCenter: parent.verticalCenter

        SequentialAnimation on opacity {
            running: badge.showDot && badge.pulseDot
            loops: Animation.Infinite
            NumberAnimation { to: 0.3; duration: 600 }
            NumberAnimation { to: 1.0; duration: 600 }
            onStopped: dot.opacity = 1
        }
    }

    Text {
        id: caption
        color: "#ffffff"
        font.pixelSize: 10
        font.bold: true
        textFormat: Text.PlainText
        elide: Text.ElideRight
        horizontalAlignment: Text.AlignHCenter
        x: badge.horizontalPadding + (badge.showDot ? dot.width + 6 : 0)
        width: Math.max(0, badge.width - x - badge.horizontalPadding)
        anchors.verticalCenter: parent.verticalCenter
    }
}
