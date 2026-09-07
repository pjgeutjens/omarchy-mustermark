import QtQuick
import QtQuick.Controls
import QtWebEngine

Item {
    id: previewWindow
    objectName: "previewWindow"
    signal returnToNormal(int line)
    property int selectedLine: 1
    property bool selectionReady: false

    readonly property color foreground: documentController.theme.foreground
    readonly property color accent: documentController.theme.accent
    readonly property color muted: documentController.theme.muted
    property string pendingAppendSection: ""
    property string pendingAppendKind: ""
    property bool appendPending: false

    function activatePreview() {
        forceActiveFocus()
    }

    function withoutDraft(callback) {
        previewView.runJavaScript("typeof activeDraft !== 'undefined' && !!activeDraft",
            function(draft) { if (!draft) callback() })
    }

    function selectLine(line) {
        selectedLine = line
        previewView.forceActiveFocus()
        previewView.runJavaScript("typeof selectHostLine === 'function' && selectHostLine(" + line + ")",
            function(selected) { selectionReady = Boolean(selected); if (!selected && previewWindow.visible) selectionTimer.restart() })
    }

    function reloadPreview() {
        previewView.reload()
    }

    function requestAppend(section, kind) {
        pendingAppendSection = section
        pendingAppendKind = kind
        appendPending = true
        activatePreview()
        previewView.forceActiveFocus()
        runPendingAppend()
    }

    function runPendingAppend() {
        if (!appendPending) return
        if (previewView.loading) {
            appendRetryTimer.restart()
            return
        }
        const section = JSON.stringify(pendingAppendSection)
        const kind = JSON.stringify(pendingAppendKind)
        previewView.runJavaScript(
            "typeof beginExternalAppend === 'function' ? beginExternalAppend(" + section + "," + kind + ") : null",
            function(started) {
                if (started === true) {
                    appendPending = false
                    appendFocusTimer.restart()
                } else if (started === null || started === undefined) {
                    appendRetryTimer.restart()
                } else {
                    appendPending = false
                }
            })
    }

    Timer {
        id: selectionTimer
        interval: 100
        onTriggered: previewWindow.selectLine(previewWindow.selectedLine)
    }

    Timer {
        id: appendRetryTimer
        interval: 100
        repeat: false
        onTriggered: previewWindow.runPendingAppend()
    }

    Timer {
        id: appendFocusTimer
        interval: 250
        repeat: false
        onTriggered: {
            previewView.forceActiveFocus()
            previewView.runJavaScript(
                "document.querySelector('.mm-inline-input')?.focus()")
        }
    }

    function handleEscape() {
        previewView.runJavaScript(
            "typeof cancelTransientUi === 'function' && cancelTransientUi()",
            function(handled) {
                if (!handled)
                    previewView.runJavaScript("stateNode(activeItemRef)?.startLine || 0",
                        function(line) { previewWindow.returnToNormal(line || previewWindow.selectedLine) })
            })
    }

    WebEngineView {
        id: previewView
        objectName: "previewView"
        anchors.fill: parent
        url: previewUrl
        backgroundColor: documentController.theme.background
        settings.javascriptEnabled: true
        settings.javascriptCanAccessClipboard: true
        settings.javascriptCanPaste: true
        settings.localContentCanAccessRemoteUrls: false
        onLoadingChanged: function(loadRequest) {
            if (loadRequest.status === WebEngineView.LoadSucceededStatus)
                { previewWindow.selectLine(previewWindow.selectedLine); previewWindow.runPendingAppend() }
        }
    }

    Shortcut {
        sequence: "Ctrl+R"
        enabled: previewWindow.visible
        onActivated: previewWindow.reloadPreview()
    }



}
