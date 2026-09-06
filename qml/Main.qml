import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

ApplicationWindow {
    id: window
    width: 1180
    height: 760
    minimumWidth: 760
    minimumHeight: 440
    visible: true
    title: (documentController.modified ? "● " : "") + documentController.title
    color: canvasColor

    property bool commandMode: false
    property bool keyHelpVisible: false
    property bool recentVisible: false
    property bool includeDescendants: false
    property bool syncingSource: false
    property int selectedIndex: -1
    property var pendingSelection: null

    readonly property color canvasColor: documentController.theme.background
    readonly property color foreground: documentController.theme.foreground
    readonly property color accent: documentController.theme.accent
    readonly property color muted: documentController.theme.muted
    readonly property color selection: documentController.theme.selection
    readonly property color selectionForeground: documentController.theme.selectionForeground
    readonly property var selectedNode: selectedIndex >= 0 && selectedIndex < documentController.nodes.length
                                        ? documentController.nodes[selectedIndex] : null
    readonly property int cursorLine: lineForPosition(sourceArea.cursorPosition)
    readonly property int cursorColumn: {
        const previousBreak = sourceArea.text.lastIndexOf("\n", sourceArea.cursorPosition - 1)
        return sourceArea.cursorPosition - previousBreak
    }

    function currentNode() {
        const nodes = documentController.nodes
        return selectedIndex >= 0 && selectedIndex < nodes.length ? nodes[selectedIndex] : null
    }

    function lineForPosition(position) {
        let line = 1
        const limit = Math.min(Math.max(position, 0), sourceArea.text.length)
        for (let index = 0; index < limit; ++index)
            if (sourceArea.text.charCodeAt(index) === 10) ++line
        return line
    }

    function positionForLine(line) {
        if (line <= 1) return 0
        let position = 0
        for (let current = 1; current < line; ++current) {
            position = sourceArea.text.indexOf("\n", position)
            if (position < 0) return sourceArea.text.length
            ++position
        }
        return position
    }

    function nodePriority(node) {
        if (node.kind === "item") return 3
        if (node.kind === "heading") return 2
        return 1
    }

    function bestNodeAtLine(line) {
        const nodes = documentController.nodes
        let best = -1
        let bestSpan = Number.MAX_SAFE_INTEGER
        let bestPriority = -1
        for (let index = 0; index < nodes.length; ++index) {
            const node = nodes[index]
            if (line < node.startLine || line > node.endLine) continue
            const span = node.endLine - node.startLine
            const priority = nodePriority(node)
            if (span < bestSpan || (span === bestSpan && priority > bestPriority)) {
                best = index
                bestSpan = span
                bestPriority = priority
            }
        }
        return best
    }

    function selectIndex(index, reveal) {
        if (index < 0 || index >= documentController.nodes.length) {
            selectedIndex = -1
            markdownHighlighter.setStructuralRange(-1, -1)
            return
        }
        selectedIndex = index
        const node = documentController.nodes[index]
        markdownHighlighter.setStructuralRange(node.startLine, node.endLine)
        if (reveal) {
            const rectangle = sourceArea.positionToRectangle(positionForLine(node.startLine))
            const flickable = editorScroll.contentItem
            if (rectangle.y < flickable.contentY + 18 ||
                    rectangle.y > flickable.contentY + editorScroll.availableHeight - 50)
                flickable.contentY = Math.max(0, rectangle.y - editorScroll.availableHeight / 3)
        }
    }

    function selectAtPosition(position, reveal) {
        selectIndex(bestNodeAtLine(lineForPosition(position)), reveal)
    }

    function selectRelative(delta) {
        const count = documentController.nodes.length
        if (!count) return
        const next = selectedIndex < 0
                     ? (delta > 0 ? 0 : count - 1)
                     : Math.max(0, Math.min(count - 1, selectedIndex + delta))
        selectIndex(next, true)
    }

    function rememberSelection(fallbackLine) {
        const node = currentNode()
        if (!node) return
        pendingSelection = {
            "id": node.id,
            "kind": node.kind,
            "text": node.text,
            "line": fallbackLine || node.startLine
        }
    }

    function restoreSelection() {
        if (!commandMode) {
            selectIndex(-1, false)
            pendingSelection = null
            return
        }
        const nodes = documentController.nodes
        const wanted = pendingSelection
        let best = -1
        let distance = Number.MAX_SAFE_INTEGER
        if (wanted) {
            for (let index = 0; index < nodes.length; ++index) {
                const node = nodes[index]
                if (wanted.id && node.id === wanted.id) {
                    best = index
                    break
                }
                if (node.kind === wanted.kind && node.text === wanted.text) {
                    const candidateDistance = Math.abs(node.startLine - wanted.line)
                    if (candidateDistance < distance) {
                        best = index
                        distance = candidateDistance
                    }
                }
            }
        }
        if (best < 0)
            best = bestNodeAtLine(lineForPosition(sourceArea.cursorPosition))
        selectIndex(best, false)
        pendingSelection = null
    }

    function compatible(a, b) {
        if (!a || !b || a.kind !== b.kind) return false
        if (a.parent !== b.parent) return false
        return a.kind !== "heading" || a.level === b.level
    }

    function canMove(delta) {
        const selected = currentNode()
        if (!selected) return false
        const nodes = documentController.nodes
        for (let index = selectedIndex + delta; index >= 0 && index < nodes.length; index += delta)
            if (compatible(selected, nodes[index])) return true
        return false
    }

    function canShiftBranch(delta) {
        const selected = currentNode()
        if (!selected || selected.kind !== "heading") return false
        const nodes = documentController.nodes
        for (let index = 0; index < nodes.length; ++index) {
            const node = nodes[index]
            if (node.kind !== "heading" || node.startLine < selected.startLine ||
                    node.startLine > selected.endLine) continue
            if ((delta < 0 && node.level <= 1) || (delta > 0 && node.level >= 6))
                return false
        }
        return true
    }

    function canShiftLevel(delta, includeChildren) {
        const selected = currentNode()
        if (!selected) return false
        if (selected.kind === "heading")
            return includeChildren ? canShiftBranch(delta)
                                   : (delta < 0 ? selected.level > 1 : selected.level < 6)
        return selected.kind === "item"
    }

    function moveRelative(delta) {
        const selected = currentNode()
        if (!selected) return
        const nodes = documentController.nodes
        let targetIndex = selectedIndex + delta
        while (targetIndex >= 0 && targetIndex < nodes.length &&
               !compatible(selected, nodes[targetIndex]))
            targetIndex += delta
        if (targetIndex < 0 || targetIndex >= nodes.length) return
        rememberSelection(nodes[targetIndex].startLine)
        documentController.applyAction(delta < 0 ? "move_before" : "move_after",
                                       selected.identity, nodes[targetIndex].identity)
    }

    function applySelected(action) {
        const selected = currentNode()
        if (!selected) return
        rememberSelection(selected.startLine)
        documentController.applyAction(action, selected.identity)
    }

    function shiftSelected(action, includeChildren) {
        const selected = currentNode()
        if (!selected || (selected.kind !== "heading" && selected.kind !== "item")) return
        rememberSelection(selected.startLine)
        documentController.shiftLevel(action, selected.identity,
                                      selected.kind === "item" || includeChildren)
    }

    function setSelectedHeadingLevel(level) {
        const selected = currentNode()
        if (!selected || selected.kind !== "heading") return
        rememberSelection(selected.startLine)
        documentController.setHeadingLevel(selected.identity, level)
    }

    function commitSource() {
        sourceParseTimer.stop()
        documentController.updateSource(sourceArea.text)
    }

    function continueListItem() {
        if (sourceArea.selectionStart !== sourceArea.selectionEnd) return false
        const position = sourceArea.cursorPosition
        const lineStart = sourceArea.text.lastIndexOf("\n", position - 1) + 1
        let lineEnd = sourceArea.text.indexOf("\n", position)
        if (lineEnd < 0) lineEnd = sourceArea.text.length
        if (position !== lineEnd) return false

        const line = sourceArea.text.slice(lineStart, lineEnd)
        const bullet = /^(\s*)([-+*])(\s+)(?:\[([ xX])\](\s+))?(.+)$/.exec(line)
        const ordered = /^(\s*)(\d+)([.)])(\s+)(?:\[([ xX])\](\s+))?(.+)$/.exec(line)
        let prefix = ""
        if (bullet) {
            prefix = bullet[1] + bullet[2] + bullet[3]
            if (bullet[4] !== undefined) prefix += "[ ]" + bullet[5]
        } else if (ordered) {
            prefix = ordered[1] + (Number(ordered[2]) + 1) + ordered[3] + ordered[4]
            if (ordered[5] !== undefined) prefix += "[ ]" + ordered[6]
        } else {
            return false
        }

        const insertion = "\n" + prefix
        sourceArea.insert(position, insertion)
        sourceArea.cursorPosition = position + insertion.length
        return true
    }

    function enterNormal() {
        const position = sourceArea.cursorPosition
        commitSource()
        selectAtPosition(position, false)
        commandMode = true
        structureLayer.forceActiveFocus()
    }

    function enterInsert(position) {
        commandMode = false
        selectIndex(-1, false)
        sourceArea.forceActiveFocus()
        if (position !== undefined)
            sourceArea.cursorPosition = Math.max(0, Math.min(position, sourceArea.length))
    }

    function saveNow() {
        commitSource()
        if (documentController.filePath)
            documentController.save()
        else
            saveDialog.open()
    }

    function prepareFileSwitch() {
        commitSource()
        if (documentController.filePath && documentController.modified &&
                !documentController.conflict)
            return documentController.save()
        return true
    }

    function showOpenDialog() {
        if (prepareFileSwitch())
            openDialog.open()
    }

    function showRecent() {
        keyHelpVisible = false
        recentVisible = true
        recentList.currentIndex = documentController.recentFiles.length ? 0 : -1
        recentPane.forceActiveFocus()
    }

    function openRecent(index) {
        const recent = documentController.recentFiles
        if (index < 0 || index >= recent.length) return
        if (!prepareFileSwitch()) return
        if (documentController.loadFile(recent[index].url)) {
            recentVisible = false
            enterInsert(0)
        }
    }

    component FooterAction: Rectangle {
        id: actionRoot
        required property string label
        property bool active: false
        signal triggered()
        implicitWidth: actionLabel.implicitWidth + 14
        implicitHeight: 30
        color: active ? Qt.alpha(window.accent, 0.24)
                      : actionHover.hovered && enabled ? Qt.alpha(window.foreground, 0.08)
                                                      : "transparent"
        opacity: enabled ? 1 : 0.32
        Label {
            id: actionLabel
            anchors.centerIn: parent
            text: actionRoot.label
            color: actionRoot.active ? window.accent : window.foreground
            font.family: "monospace"
            font.pixelSize: 11
            font.bold: actionRoot.active
        }
        HoverHandler { id: actionHover }
        TapHandler {
            enabled: actionRoot.enabled
            onTapped: actionRoot.triggered()
        }
    }

    ScrollView {
        id: editorScroll
        anchors.fill: parent
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AsNeeded
        ScrollBar.vertical.policy: ScrollBar.AsNeeded

        TextArea {
            id: sourceArea
            objectName: "sourceArea"
            width: editorScroll.availableWidth
            height: Math.max(editorScroll.availableHeight, contentHeight + topPadding + bottomPadding)
            leftPadding: Math.max(42, (width - 1080) / 2)
            rightPadding: leftPadding
            topPadding: 30
            bottomPadding: 60
            color: window.foreground
            selectionColor: window.selection
            selectedTextColor: window.selectionForeground
            font.family: "monospace"
            font.pixelSize: 17
            wrapMode: TextEdit.Wrap
            selectByMouse: true
            persistentSelection: true
            readOnly: window.commandMode
            background: Rectangle { color: "transparent" }

            onTextChanged: {
                if (!window.syncingSource) sourceParseTimer.restart()
            }
            Keys.onPressed: function(event) {
                if (!window.commandMode &&
                        (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) &&
                        event.modifiers === Qt.NoModifier && window.continueListItem())
                    event.accepted = true
            }

            Component.onCompleted: {
                window.syncingSource = true
                text = documentController.source
                window.syncingSource = false
                markdownHighlighter.attach(textDocument)
                markdownHighlighter.setColors(window.foreground, window.muted, window.accent)
                forceActiveFocus()
            }

            FocusScope {
                id: structureLayer
                anchors.fill: parent
                visible: window.commandMode
                focus: visible

                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton
                    cursorShape: Qt.IBeamCursor
                    onPositionChanged: function(mouse) {
                        window.selectAtPosition(sourceArea.positionAt(mouse.x, mouse.y), false)
                    }
                    onClicked: function(mouse) {
                        const position = sourceArea.positionAt(mouse.x, mouse.y)
                        window.selectAtPosition(position, false)
                        window.enterInsert(position)
                    }
                }
            }
        }
    }

    Rectangle {
        id: keyHelp
        objectName: "keyHelp"
        anchors.fill: parent
        visible: window.keyHelpVisible
        z: 20
        color: Qt.alpha(window.canvasColor, 0.96)

        Column {
            anchors.centerIn: parent
            width: Math.min(560, parent.width - 80)
            spacing: 16

            Label {
                text: "keybindings"
                color: window.accent
                font.family: "monospace"
                font.pixelSize: 15
                font.bold: true
            }

            Column {
                width: parent.width
                spacing: 7

                Repeater {
                    model: [
                        { keys: "?", action: "show / hide keybindings" },
                        { keys: "Esc", action: "normal mode / close keybindings" },
                        { keys: "i  Enter", action: "insert mode" },
                        { keys: "J  K", action: "next / previous structure" },
                        { keys: "Shift+J  Shift+K", action: "move structure down / up" },
                        { keys: "Shift+H  Shift+L", action: "promote / demote selection" },
                        { keys: "Ctrl+Shift+H/L", action: "promote / demote with children" },
                        { keys: "Space", action: "toggle task" },
                        { keys: "Ctrl+N  Ctrl+O", action: "new / open" },
                        { keys: "Ctrl+Shift+O", action: "open recent" },
                        { keys: "Ctrl+S", action: "save" },
                        { keys: "Ctrl+Shift+S", action: "save as" },
                        { keys: "Ctrl+Q", action: "quit" }
                    ]

                    Row {
                        required property var modelData
                        width: parent.width
                        height: Math.max(bindingKeys.implicitHeight, bindingAction.implicitHeight)

                        Label {
                            id: bindingKeys
                            width: 190
                            text: modelData.keys
                            color: window.foreground
                            font.family: "monospace"
                            font.pixelSize: 12
                            font.bold: true
                        }
                        Label {
                            id: bindingAction
                            width: parent.width - bindingKeys.width
                            text: modelData.action
                            color: window.muted
                            font.family: "monospace"
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                        }
                    }
                }
            }

            Label {
                text: "In INSERT, Enter continues a list marker when the cursor is at the end of an item."
                width: parent.width
                color: window.muted
                font.family: "monospace"
                font.pixelSize: 11
                wrapMode: Text.Wrap
            }
        }

        TapHandler { onTapped: window.keyHelpVisible = false }
    }

    Rectangle {
        id: recentPane
        objectName: "recentPane"
        anchors.fill: parent
        visible: window.recentVisible
        focus: visible
        z: 20
        color: Qt.alpha(window.canvasColor, 0.96)

        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_Escape) {
                window.recentVisible = false
                event.accepted = true
            } else if (event.key === Qt.Key_Down || event.key === Qt.Key_J) {
                if (recentList.count)
                    recentList.currentIndex = Math.min(recentList.count - 1,
                                                       recentList.currentIndex + 1)
                event.accepted = true
            } else if (event.key === Qt.Key_Up || event.key === Qt.Key_K) {
                if (recentList.count)
                    recentList.currentIndex = Math.max(0, recentList.currentIndex - 1)
                event.accepted = true
            } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                window.openRecent(recentList.currentIndex)
                event.accepted = true
            } else if (event.key >= Qt.Key_1 && event.key <= Qt.Key_9) {
                window.openRecent(event.key - Qt.Key_1)
                event.accepted = true
            } else if (event.key === Qt.Key_0) {
                window.openRecent(9)
                event.accepted = true
            }
        }

        Column {
            anchors.centerIn: parent
            width: Math.min(720, parent.width - 80)
            spacing: 16

            Label {
                text: "recent files"
                color: window.accent
                font.family: "monospace"
                font.pixelSize: 15
                font.bold: true
            }

            ListView {
                id: recentList
                objectName: "recentList"
                width: parent.width
                height: Math.min(contentHeight, 410)
                model: documentController.recentFiles
                clip: true
                currentIndex: -1
                spacing: 2

                delegate: Rectangle {
                    id: recentRow
                    required property int index
                    required property var modelData
                    width: ListView.view.width
                    height: 42
                    color: ListView.isCurrentItem || recentHover.hovered
                           ? Qt.alpha(window.foreground, 0.08) : "transparent"

                    Row {
                        anchors.fill: parent
                        spacing: 12

                        Label {
                            width: 24
                            height: parent.height
                            verticalAlignment: Text.AlignVCenter
                            horizontalAlignment: Text.AlignRight
                            text: recentRow.index + 1
                            color: window.muted
                            font.family: "monospace"
                            font.pixelSize: 11
                        }
                        Column {
                            width: parent.width - 36
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 2
                            Label {
                                width: parent.width
                                text: recentRow.modelData.name
                                color: window.foreground
                                elide: Text.ElideMiddle
                                font.family: "monospace"
                                font.pixelSize: 12
                                font.bold: true
                            }
                            Label {
                                width: parent.width
                                text: recentRow.modelData.directory
                                color: window.muted
                                elide: Text.ElideMiddle
                                font.family: "monospace"
                                font.pixelSize: 10
                            }
                        }
                    }

                    HoverHandler { id: recentHover }
                    TapHandler {
                        onTapped: {
                            recentList.currentIndex = recentRow.index
                            window.openRecent(recentRow.index)
                        }
                    }
                }
            }

            Label {
                visible: recentList.count === 0
                width: parent.width
                text: "No recent files. Ctrl+O opens a file."
                color: window.muted
                font.family: "monospace"
                font.pixelSize: 12
            }

            Label {
                visible: recentList.count > 0
                width: parent.width
                text: "1–9 opens · J/K or arrows select · Enter opens · Esc closes"
                color: window.muted
                font.family: "monospace"
                font.pixelSize: 11
            }
        }

        TapHandler {
            acceptedButtons: Qt.RightButton
            onTapped: window.recentVisible = false
        }
    }

    footer: Rectangle {
        implicitHeight: 31
        color: Qt.darker(window.canvasColor, 1.16)
        border.color: Qt.alpha(window.foreground, 0.08)
        border.width: 1

        RowLayout {
            anchors.fill: parent
            spacing: 0

            Rectangle {
                Layout.fillHeight: true
                implicitWidth: 76
                color: window.commandMode ? window.accent : window.selection
                Label {
                    objectName: "modeLabel"
                    anchors.centerIn: parent
                    text: window.commandMode ? "NORMAL" : "INSERT"
                    color: window.commandMode ? window.canvasColor : window.selectionForeground
                    font.family: "monospace"
                    font.bold: true
                    font.pixelSize: 11
                }
                TapHandler {
                    onTapped: window.commandMode ? window.enterInsert() : window.enterNormal()
                }
            }

            FooterAction {
                label: "new"
                onTriggered: {
                    documentController.newDocument()
                    window.enterInsert(0)
                }
            }
            FooterAction { label: "open"; onTriggered: window.showOpenDialog() }
            FooterAction { label: "recent"; onTriggered: window.showRecent() }
            FooterAction { label: "save"; onTriggered: window.saveNow() }

            Rectangle {
                implicitWidth: 1
                Layout.fillHeight: true
                Layout.topMargin: 7
                Layout.bottomMargin: 7
                color: Qt.alpha(window.foreground, 0.15)
            }

            Flickable {
                id: actionStrip
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentWidth: contextActions.implicitWidth
                contentHeight: height
                clip: true
                boundsBehavior: Flickable.StopAtBounds

                Row {
                    id: contextActions
                    height: parent.height
                    spacing: 0

                    Label {
                        visible: !window.commandMode || !window.selectedNode
                        height: parent.height
                        verticalAlignment: Text.AlignVCenter
                        leftPadding: 12
                        rightPadding: 12
                        text: window.commandMode ? "no structure" : "Esc  structure"
                        color: window.muted
                        font.family: "monospace"
                        font.pixelSize: 11
                    }

                    Label {
                        visible: window.commandMode && window.selectedNode
                        height: parent.height
                        width: Math.min(implicitWidth, 170)
                        verticalAlignment: Text.AlignVCenter
                        leftPadding: 10
                        rightPadding: 8
                        text: window.selectedNode
                              ? (window.selectedNode.kind === "heading"
                                 ? "H" + window.selectedNode.level + "  " + window.selectedNode.text
                                 : window.selectedNode.kind + "  " + window.selectedNode.text)
                              : ""
                        color: window.muted
                        elide: Text.ElideRight
                        font.family: "monospace"
                        font.pixelSize: 11
                    }

                    FooterAction {
                        visible: window.commandMode && window.selectedNode
                        label: "move ↑"
                        enabled: window.canMove(-1)
                        onTriggered: window.moveRelative(-1)
                    }
                    FooterAction {
                        objectName: "moveDownAction"
                        visible: window.commandMode && window.selectedNode
                        label: "↓"
                        enabled: window.canMove(1)
                        onTriggered: window.moveRelative(1)
                    }

                    Label {
                        visible: window.commandMode && window.selectedNode &&
                                 window.selectedNode.kind === "heading"
                        height: parent.height
                        verticalAlignment: Text.AlignVCenter
                        leftPadding: 7
                        rightPadding: 2
                        text: "title"
                        color: window.muted
                        font.family: "monospace"
                        font.pixelSize: 10
                    }
                    Repeater {
                        model: window.commandMode && window.selectedNode &&
                               window.selectedNode.kind === "heading" ? 6 : 0
                        FooterAction {
                            required property int index
                            label: "H" + (index + 1)
                            active: window.selectedNode && window.selectedNode.level === index + 1
                            enabled: !active
                            onTriggered: window.setSelectedHeadingLevel(index + 1)
                        }
                    }
                    FooterAction {
                        visible: window.commandMode && window.selectedNode &&
                                 window.selectedNode.kind === "heading"
                        label: "+children"
                        active: window.includeDescendants
                        onTriggered: window.includeDescendants = !window.includeDescendants
                    }
                    FooterAction {
                        visible: window.commandMode && window.selectedNode &&
                                 (window.selectedNode.kind === "heading" ||
                                  window.selectedNode.kind === "item")
                        label: "promote"
                        enabled: window.canShiftLevel(-1, window.includeDescendants)
                        onTriggered: window.shiftSelected("promote", window.includeDescendants)
                    }
                    FooterAction {
                        visible: window.commandMode && window.selectedNode &&
                                 (window.selectedNode.kind === "heading" ||
                                  window.selectedNode.kind === "item")
                        label: "demote"
                        enabled: window.canShiftLevel(1, window.includeDescendants)
                        onTriggered: window.shiftSelected("demote", window.includeDescendants)
                    }
                    FooterAction {
                        objectName: "taskAction"
                        visible: window.commandMode && window.selectedNode &&
                                 window.selectedNode.kind === "item" && window.selectedNode.task
                        label: window.selectedNode && window.selectedNode.checked ? "uncheck" : "check"
                        onTriggered: window.applySelected("toggle_task")
                    }
                }
            }

            Label {
                Layout.maximumWidth: 210
                Layout.preferredWidth: Math.min(implicitWidth + 20, 210)
                Layout.fillHeight: true
                verticalAlignment: Text.AlignVCenter
                leftPadding: 10
                rightPadding: 10
                text: documentController.conflict ? "CONFLICT  " + documentController.status
                      : documentController.modified ? "saving…" : documentController.status
                color: documentController.conflict ? "#d98b8b" : window.muted
                elide: Text.ElideRight
                font.family: "monospace"
                font.pixelSize: 10
            }

            FooterAction {
                label: documentController.tracked ? "tracked" : "track"
                active: documentController.tracked
                onTriggered: documentController.tracked
                             ? documentController.disableTracking()
                             : documentController.enableTracking()
            }

            Rectangle {
                Layout.fillHeight: true
                implicitWidth: 66
                color: window.accent
                Label {
                    anchors.centerIn: parent
                    text: window.cursorLine + ":" + window.cursorColumn
                    color: window.canvasColor
                    font.family: "monospace"
                    font.bold: true
                    font.pixelSize: 11
                }
            }
        }
    }

    Timer {
        id: sourceParseTimer
        interval: 130
        onTriggered: {
            documentController.updateSource(sourceArea.text)
            autosaveTimer.restart()
        }
    }

    Timer {
        id: autosaveTimer
        interval: 450
        onTriggered: {
            if (documentController.filePath && !documentController.conflict)
                documentController.save()
        }
    }

    Connections {
        target: documentController
        function onSourceChanged() {
            if (sourceArea.text !== documentController.source) {
                window.syncingSource = true
                const cursor = sourceArea.cursorPosition
                sourceArea.text = documentController.source
                sourceArea.cursorPosition = Math.min(cursor, sourceArea.length)
                window.syncingSource = false
            }
            if (documentController.modified) autosaveTimer.restart()
        }
        function onDocumentChanged() { window.restoreSelection() }
        function onThemeChanged() {
            markdownHighlighter.setColors(window.foreground, window.muted, window.accent)
        }
    }

    Shortcut {
        sequence: "Escape"
        enabled: !window.commandMode && !window.recentVisible
        onActivated: window.enterNormal()
    }
    Shortcut {
        sequence: "?"
        enabled: window.commandMode && !window.recentVisible
        onActivated: window.keyHelpVisible = !window.keyHelpVisible
    }
    Shortcut {
        sequence: "Escape"
        enabled: window.keyHelpVisible
        onActivated: window.keyHelpVisible = false
    }
    Shortcut {
        sequence: "Escape"
        enabled: window.recentVisible
        onActivated: window.recentVisible = false
    }
    Shortcut { sequence: "I"; enabled: window.commandMode && !window.recentVisible; onActivated: window.enterInsert() }
    Shortcut { sequence: "Return"; enabled: window.commandMode && !window.recentVisible; onActivated: window.enterInsert() }
    Shortcut { sequence: "J"; enabled: window.commandMode && !window.recentVisible; onActivated: window.selectRelative(1) }
    Shortcut { sequence: "K"; enabled: window.commandMode && !window.recentVisible; onActivated: window.selectRelative(-1) }
    Shortcut { sequence: "Shift+J"; enabled: window.commandMode && !window.recentVisible; onActivated: window.moveRelative(1) }
    Shortcut { sequence: "Shift+K"; enabled: window.commandMode && !window.recentVisible; onActivated: window.moveRelative(-1) }
    Shortcut {
        sequence: "Shift+H"
        enabled: window.commandMode && !window.recentVisible && window.canShiftLevel(-1, false)
        onActivated: window.shiftSelected("promote", false)
    }
    Shortcut {
        sequence: "Shift+L"
        enabled: window.commandMode && !window.recentVisible && window.canShiftLevel(1, false)
        onActivated: window.shiftSelected("demote", false)
    }
    Shortcut {
        sequence: "Ctrl+Shift+H"
        enabled: window.commandMode && !window.recentVisible && window.selectedNode &&
                 window.selectedNode.kind === "heading" && window.canShiftLevel(-1, true)
        onActivated: window.shiftSelected("promote", true)
    }
    Shortcut {
        sequence: "Ctrl+Shift+L"
        enabled: window.commandMode && !window.recentVisible && window.selectedNode &&
                 window.selectedNode.kind === "heading" && window.canShiftLevel(1, true)
        onActivated: window.shiftSelected("demote", true)
    }
    Shortcut {
        sequence: "Space"
        enabled: window.commandMode && !window.recentVisible && window.selectedNode && window.selectedNode.task
        onActivated: window.applySelected("toggle_task")
    }
    Shortcut {
        sequence: "Ctrl+N"
        onActivated: {
            documentController.newDocument()
            window.enterInsert(0)
        }
    }
    Shortcut { sequence: "Ctrl+O"; onActivated: window.showOpenDialog() }
    Shortcut {
        sequence: "Ctrl+Shift+O"
        onActivated: {
            if (window.recentVisible)
                window.recentVisible = false
            else
                window.showRecent()
        }
    }
    Shortcut { sequence: "Ctrl+S"; onActivated: window.saveNow() }
    Shortcut { sequence: "Ctrl+Shift+S"; onActivated: { window.commitSource(); saveDialog.open() } }
    Shortcut { sequence: "Ctrl+Q"; onActivated: window.close() }

    FileDialog {
        id: openDialog
        title: "Open Markdown file"
        fileMode: FileDialog.OpenFile
        nameFilters: ["Markdown files (*.md *.markdown)", "Text files (*.txt)", "All files (*)"]
        onAccepted: {
            documentController.loadFile(selectedFile)
            window.enterInsert(0)
        }
    }

    FileDialog {
        id: saveDialog
        title: "Save Markdown file"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "md"
        nameFilters: ["Markdown files (*.md)"]
        onAccepted: documentController.saveAs(selectedFile)
    }
}
