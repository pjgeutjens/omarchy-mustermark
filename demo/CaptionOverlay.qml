import QtQuick
import QtQuick.Controls

Item {
    id: overlay
    anchors.fill: parent
    z: 1000
    property string caption: "Mustermark · a guided tour"
    property string detail: "Start your recording, then press F8. F10 stops the demo."
    property string code: ""
    property var webView: null
    property var jsResult: null
    property bool jsDone: false
    function evaluate(script) {
        jsDone = false
        webView.runJavaScript(script, function(result) { jsResult = result; jsDone = true })
    }
    Rectangle {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 30
        width: Math.min(580, parent.width * 0.45)
        height: codeText.implicitHeight + 36
        visible: overlay.code !== ""
        color: "#ed172331"
        radius: 12
        border.color: "#81b9b0"
        Text {
            id: codeText
            anchors.fill: parent
            anchors.margins: 18
            text: overlay.code
            textFormat: Text.PlainText
            color: "#d9eee8"
            font.family: "monospace"
            font.pixelSize: 15
            wrapMode: Text.WrapAnywhere
        }
    }
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 62
        width: Math.min(parent.width - 100, 1080)
        height: narration.implicitHeight + 36
        color: "#ee111923"
        radius: 12
        border.color: "#6c9692"
        Column {
            id: narration
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.margins: 24
            spacing: 8
            Text {
                width: parent.width
                text: overlay.caption
                textFormat: Text.PlainText
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                color: "#f4f3ec"
                font.pixelSize: 23
                font.bold: true
            }
            Text {
                width: parent.width
                text: overlay.detail
                textFormat: Text.PlainText
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                color: "#bdd7d3"
                font.pixelSize: 16
            }
        }
    }
}
