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

    property bool visualMode: false
    property int visualSelectionLine: 1
    property var pendingVisualAppend: null
    property bool commandMode: false
    property bool keyHelpVisible: false
    property bool recentVisible: false
    property bool attachmentsVisible: false
    property int attachmentIndex: 0
    property bool includeDescendants: false
    property bool syncingSource: false
    property int selectedIndex: -1
    property var pendingSelection: null
    property var provisionalInsert: null

    readonly property color canvasColor: documentController.theme.background
    readonly property color foreground: documentController.theme.foreground
    readonly property color accent: documentController.theme.accent
    readonly property color muted: documentController.theme.muted
    readonly property color selection: documentController.theme.selection
    readonly property color selectionForeground: documentController.theme.selectionForeground
    readonly property bool structuralShortcutsEnabled: window.commandMode && !window.visualMode &&
                                                       !window.recentVisible &&
                                                       !window.attachmentsVisible &&
                                                       !window.keyHelpVisible
    readonly property var selectedNode: selectedIndex >= 0 && selectedIndex < documentController.nodes.length
                                        ? documentController.nodes[selectedIndex] : null
    readonly property string firstKeyHelpBinding: keyHelpBindings(selectedNode)[0].keys
    readonly property int cursorLine: lineForPosition(sourceArea.cursorPosition)
    readonly property int cursorColumn: {
        const previousBreak = sourceArea.text.lastIndexOf("\n", sourceArea.cursorPosition - 1)
        return sourceArea.cursorPosition - previousBreak
    }

    function currentNode() {
        const nodes = documentController.nodes
        return selectedIndex >= 0 && selectedIndex < nodes.length ? nodes[selectedIndex] : null
    }

    function keyHelpBindings(node) {
        const relevant = []
        const remaining = [
            { keys: "?", action: "show / hide keybindings" },
            { keys: "Esc", action: "normal mode / close keybindings" },
            { keys: "i  Enter", action: "insert mode" },
            { keys: "o  O", action: "add matching structure below / above" },
            { keys: "J  K", action: "next / previous structure" },
            { keys: "Left", action: "select parent structure" },
            { keys: "Shift+J  Shift+K", action: "move structure down / up" },
            { keys: "Shift+H  Shift+L", action: "promote / demote selection" },
            { keys: "Ctrl+Shift+H/L", action: "promote / demote with children" },
            { keys: "Space", action: "toggle task" },
            { keys: "d  Delete", action: "delete selected structure" },
            { keys: "a  Ctrl+Enter", action: "append item to current list" },
            { keys: "A  Ctrl+Shift+Enter", action: "prepend item to current list" },
            { keys: "P  Ctrl+V", action: "attach clipboard image to item" },
            { keys: "Ctrl+Shift+V", action: "paste clipboard text" },
            { keys: "Ctrl+Shift+A", action: "open images for selected item" },
            { keys: "Shift+V", action: "rendered Visual mode" },
            { keys: "Ctrl+Z", action: "undo latest Editor change" },
            { keys: "Ctrl+N  Ctrl+O", action: "new / open" },
            { keys: "Ctrl+Shift+O", action: "open recent" },
            { keys: "Ctrl+S", action: "save" },
            { keys: "Ctrl+Shift+S", action: "save as" },
            { keys: "Ctrl+Q", action: "quit" }
        ]
        function prioritize(keys) {
            for (let index = 0; index < remaining.length; ++index) {
                if (remaining[index].keys !== keys) continue
                const entry = remaining.splice(index, 1)[0]
                entry.relevant = true
                relevant.push(entry)
                return
            }
        }

        if (documentController.canUndo)
            prioritize("Ctrl+Z")
        if (node) {
            if (node.kind === "item" && node.task)
                prioritize("Space")
            if (node.kind === "item" || node.kind === "list") {
                prioritize("a  Ctrl+Enter")
                prioritize("A  Ctrl+Shift+Enter")
            }
            prioritize("d  Delete")
            if (node.kind === "item") {
                prioritize("P  Ctrl+V")
                if (node.attachments && node.attachments.length > 0)
                    prioritize("Ctrl+Shift+A")
            }
            if (node.kind === "heading")
                prioritize("Ctrl+Shift+H/L")
            if (node.kind === "heading" || node.kind === "item")
                prioritize("Shift+H  Shift+L")
            prioritize("o  O")
            if (node.parent)
                prioritize("Left")
            if (canMove(-1) || canMove(1))
                prioritize("Shift+J  Shift+K")
        }
        prioritize("J  K")
        return relevant.concat(remaining)
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

    function selectParent() {
        const selected = currentNode()
        if (!selected || !selected.parent) return
        const nodes = documentController.nodes
        for (let index = 0; index < nodes.length; ++index) {
            if (nodes[index].identity === selected.parent) {
                selectIndex(index, true)
                return
            }
        }
    }

    function rememberSelection(fallbackLine) {
        const node = currentNode()
        if (!node) return
        pendingSelection = {
            "identity": node.identity,
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
                if ((wanted.identity && node.identity === wanted.identity) ||
                        (wanted.id && node.id === wanted.id)) {
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
        if (!a || !b) return false
        if (a.parent !== b.parent) return false
        const aBlock = a.kind === "list" || a.kind === "block"
        const bBlock = b.kind === "list" || b.kind === "block"
        if (aBlock || bBlock) return aBlock && bBlock
        if (a.kind !== b.kind) return false
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

    function maximumSelectedHeadingLevel() {
        const selected = currentNode()
        if (!selected || selected.kind !== "heading") return 6
        let maximum = 6
        const nodes = documentController.nodes
        for (let index = 0; index < nodes.length; ++index) {
            const node = nodes[index]
            if (node.kind !== "heading" || node.startLine <= selected.startLine ||
                    node.startLine > selected.endLine) continue
            maximum = Math.min(maximum, node.level - 1)
        }
        return maximum
    }

    function canSetSelectedHeadingLevel(level) {
        const selected = currentNode()
        return selected && selected.kind === "heading" &&
               level >= 1 && level <= maximumSelectedHeadingLevel()
    }

    function canShiftLevel(delta, includeChildren) {
        const selected = currentNode()
        if (!selected) return false
        if (selected.kind === "heading")
            return includeChildren ? canShiftBranch(delta)
                                   : (delta < 0 ? selected.level > 1
                                                : selected.level < maximumSelectedHeadingLevel())
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

    function deleteSelectedStructure() {
        const selected = currentNode()
        if (!selected) return
        rememberSelection(selected.startLine)
        documentController.applyAction("delete", selected.identity)
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
        if (!selected || selected.kind !== "heading" ||
                !canSetSelectedHeadingLevel(level)) return
        rememberSelection(selected.startLine)
        documentController.setHeadingLevel(selected.identity, level)
    }

    function undoSelectedChange() {
        const selected = currentNode()
        const wanted = selected ? {
            "identity": selected.identity,
            "id": selected.id,
            "kind": selected.kind,
            "text": selected.text,
            "line": selected.startLine
        } : null
        pendingSelection = wanted
        if (documentController.undoDocumentChange() && wanted) {
            pendingSelection = wanted
            restoreSelection()
        }
    }

    function pasteClipboardImage() {
        const resumeSourceEdit = !commandMode
        const position = sourceArea.cursorPosition
        commitSource()
        if (resumeSourceEdit)
            documentController.endSourceEdit()
        if (!commandMode)
            selectAtPosition(position, false)
        const selected = currentNode()
        if (selected && selected.kind === "item") {
            rememberSelection(selected.startLine)
            documentController.pasteImage(selected.identity)
        }
        if (resumeSourceEdit)
            documentController.beginSourceEdit()
    }

    function pasteClipboardText() {
        if (commandMode)
            enterInsert(sourceArea.cursorPosition)
        sourceArea.paste()
    }

    function showAttachments() {
        const selected = currentNode()
        if (!selected || selected.kind !== "item" || !selected.attachments ||
                selected.attachments.length === 0) return
        attachmentIndex = 0
        attachmentsVisible = true
        attachmentPane.forceActiveFocus()
    }

    function closeAttachments() {
        attachmentsVisible = false
        structureLayer.forceActiveFocus()
    }

    function showPreview() {
        if (visualMode) { if (visualLoader.item) visualLoader.item.forceActiveFocus(); return }
        if (!documentController.filePath) { saveDialog.open(); return }
        const node = currentNode()
        visualSelectionLine = node ? node.startLine : 1
        commitSource()
        documentController.endSourceEdit()
        documentController.enableTracking()
        commandMode = true
        visualMode = true
        visualLoader.active = true
        if (visualLoader.item) visualLoader.item.selectLine(visualSelectionLine)
    }

    function acceptOpenRequest(path, visual, append, section, kind) {
        const open = function() {
            if (path !== documentController.filePath) {
                if (documentController.modified || documentController.conflict) return
                if (!documentController.loadFile("file://" + path.split("/").map(encodeURIComponent).join("/"))) return
                visualMode = false
                visualLoader.active = false
                enterOpenedFile()
            }
            if (append) requestAppend(section, kind)
            else if (visual) showPreview()
        }
        if (path !== documentController.filePath && visualLoader.item)
            visualLoader.item.withoutDraft(open)
        else open()
    }

    function requestAppend(section, kind) {
        showPreview()
        if (visualLoader.item) visualLoader.item.requestAppend(section, kind)
        else pendingVisualAppend = {section: section, kind: kind}
    }

    function leaveVisual(line) {
        visualMode = false
        commandMode = true
        selectAtPosition(positionForLine(line), true)
        structureLayer.forceActiveFocus()
    }

    function removeSelectedAttachment() {
        const selected = currentNode()
        if (!selected || !selected.attachments || selected.attachments.length === 0) return
        const index = Math.max(0, Math.min(attachmentIndex, selected.attachments.length - 1))
        rememberSelection(selected.startLine)
        documentController.removeAttachment(selected.identity, selected.attachments[index].path)
        const updated = currentNode()
        if (!updated || !updated.attachments || updated.attachments.length === 0)
            closeAttachments()
        else
            attachmentIndex = Math.min(index, updated.attachments.length - 1)
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

    function lineText(line) {
        const start = positionForLine(line)
        let end = sourceArea.text.indexOf("\n", start)
        if (end < 0) end = sourceArea.text.length
        return sourceArea.text.slice(start, end)
    }

    function itemPrefix(node) {
        const line = lineText(node.startLine)
        const match = /^(\s*)([-+*]|\d+[.)])(\s+)(?:\[([ xX])\](\s+))?/.exec(line)
        if (!match) return node.ordered ? "1. " : "- "
        let prefix = match[1] + match[2] + match[3]
        if (match[4] !== undefined) prefix += "[ ]" + match[5]
        return prefix
    }

    function listPrefix(node, atEnd) {
        const nodes = documentController.nodes
        let firstItem = null
        let itemCount = 0
        for (let index = 0; index < nodes.length; ++index) {
            if (nodes[index].kind !== "item" || nodes[index].parent !== node.identity)
                continue
            if (firstItem === null)
                firstItem = nodes[index]
            ++itemCount
        }
        if (firstItem !== null) {
            const prefix = itemPrefix(firstItem)
            if (!node.ordered || !atEnd)
                return prefix
            const ordered = /^(\s*)(\d+)([.)])(\s+)(.*)$/.exec(prefix)
            if (ordered)
                return ordered[1] + (Number(ordered[2]) + itemCount) + ordered[3] +
                       ordered[4] + ordered[5]
            return prefix
        }
        return node.ordered ? "1. " : "- "
    }

    function blankSeparator(position) {
        let newlines = 0
        for (let index = position - 1; index >= 0 && sourceArea.text[index] === "\n"; --index)
            ++newlines
        return "\n".repeat(Math.max(0, 2 - newlines))
    }

    function currentListIndex() {
        const selected = currentNode()
        if (!selected) return -1
        if (selected.kind === "list") return selectedIndex
        if (selected.kind !== "item" || !selected.parent) return -1
        const nodes = documentController.nodes
        for (let index = 0; index < nodes.length; ++index)
            if (nodes[index].kind === "list" && nodes[index].identity === selected.parent)
                return index
        return -1
    }

    function canAddToCurrentList() {
        return currentListIndex() >= 0
    }

    function listEndInsertionPosition(list) {
        const nodes = documentController.nodes
        let lastItem = null
        for (let index = 0; index < nodes.length; ++index) {
            const node = nodes[index]
            if (node.kind !== "item" || node.parent !== list.identity) continue
            if (lastItem === null || node.endLine > lastItem.endLine)
                lastItem = node
        }
        return positionForLine((lastItem ? lastItem.endLine : list.endLine) + 1)
    }

    function normalizeOrderedList(identity, originalLine) {
        const nodes = documentController.nodes
        let list = null
        for (let index = 0; index < nodes.length; ++index) {
            const candidate = nodes[index]
            if (candidate.kind !== "list" || !candidate.ordered) continue
            if (candidate.identity === identity) {
                list = candidate
                break
            }
            if (list === null && candidate.startLine === originalLine)
                list = candidate
        }
        if (list === null) return false
        const items = []
        for (let index = 0; index < nodes.length; ++index)
            if (nodes[index].kind === "item" && nodes[index].parent === list.identity)
                items.push(nodes[index])
        if (items.length === 0) return false
        const first = /^(\s*)(\d+)([.)])/.exec(lineText(items[0].startLine))
        if (!first) return false
        const start = Number(first[2])
        let changed = false
        for (let index = items.length - 1; index >= 0; --index) {
            const match = /^(\s*)(\d+)([.)])/.exec(lineText(items[index].startLine))
            if (!match) continue
            const expected = String(start + index)
            if (match[2] === expected) continue
            const numberStart = positionForLine(items[index].startLine) + match[1].length
            sourceArea.remove(numberStart, numberStart + match[2].length)
            sourceArea.insert(numberStart, expected)
            changed = true
        }
        return changed
    }

    function openListBoundary(atEnd) {
        const original = currentNode()
        const listIndex = currentListIndex()
        if (!original || listIndex < 0) return
        const list = documentController.nodes[listIndex]
        const originalSelection = {
            "identity": original.identity,
            "id": original.id,
            "kind": original.kind,
            "text": original.text,
            "line": original.startLine
        }
        const prefix = listPrefix(list, atEnd)
        const newline = sourceArea.text.indexOf("\r\n") >= 0 ? "\r\n" : "\n"
        let position = atEnd ? listEndInsertionPosition(list) : positionForLine(list.startLine)
        let leading = ""
        if (atEnd && position > 0 && sourceArea.text[position - 1] !== "\n")
            leading = newline
        const insertion = leading + prefix + newline
        provisionalInsert = {
            "originalText": sourceArea.text,
            "originalSelection": originalSelection,
            "continuation": " ".repeat(prefix.length),
            "orderedListIdentity": list.ordered ? list.identity : "",
            "orderedListLine": list.startLine
        }
        enterInsert(position)
        sourceArea.insert(position, insertion)
        sourceArea.cursorPosition = position + leading.length + prefix.length
    }

    function openSibling(below, restoreNode) {
        const selected = currentNode()
        if (!selected || provisionalInsert !== null) return

        const original = restoreNode || selected
        const originalSelection = {
            "identity": original.identity,
            "id": original.id,
            "kind": original.kind,
            "text": original.text,
            "line": original.startLine
        }
        let position = positionForLine(below ? selected.endLine + 1 : selected.startLine)
        let prefix = ""
        let insertion = ""
        let continuation = ""
        let orderedListIdentity = ""
        let orderedListLine = -1
        if (selected.kind === "heading") {
            prefix = "#".repeat(selected.level) + " "
            const leading = blankSeparator(position)
            insertion = leading + prefix + "\n\n"
            position += leading.length
        } else if (selected.kind === "item") {
            prefix = itemPrefix(selected)
            const nodes = documentController.nodes
            for (let index = 0; index < nodes.length; ++index) {
                const candidate = nodes[index]
                if (candidate.kind === "list" && candidate.identity === selected.parent &&
                        candidate.ordered) {
                    orderedListIdentity = candidate.identity
                    orderedListLine = candidate.startLine
                    break
                }
            }
            const leading = position > 0 && sourceArea.text[position - 1] !== "\n" ? "\n" : ""
            insertion = leading + prefix + "\n"
            position += leading.length
            continuation = " ".repeat(prefix.length)
        } else if (selected.kind === "list") {
            prefix = listPrefix(selected)
            const leading = blankSeparator(position)
            insertion = leading + prefix + "\n\n"
            position += leading.length
            continuation = " ".repeat(prefix.length)
        } else {
            const leading = blankSeparator(position)
            insertion = leading + "\n\n"
            position += leading.length
        }

        provisionalInsert = {
            "originalText": sourceArea.text,
            "originalSelection": originalSelection,
            "continuation": continuation,
            "orderedListIdentity": orderedListIdentity,
            "orderedListLine": orderedListLine
        }
        sourceArea.insert(position, insertion)
        enterInsert(position + prefix.length)
    }

    function finishProvisional(accept) {
        const provisional = provisionalInsert
        if (provisional === null) return
        provisionalInsert = null
        sourceParseTimer.stop()
        autosaveTimer.stop()
        if (accept) {
            commitSource()
            if (provisional.orderedListIdentity &&
                    normalizeOrderedList(provisional.orderedListIdentity,
                                         provisional.orderedListLine))
                commitSource()
            enterNormal()
            if (documentController.modified) autosaveTimer.restart()
            return
        }

        window.syncingSource = true
        sourceArea.text = provisional.originalText
        window.syncingSource = false
        documentController.updateSource(provisional.originalText)
        documentController.endSourceEdit()
        commandMode = true
        const wanted = provisional.originalSelection
        const nodes = documentController.nodes
        let restored = -1
        for (let index = 0; index < nodes.length; ++index) {
            if (nodes[index].identity === wanted.identity ||
                    (wanted.id && nodes[index].id === wanted.id)) {
                restored = index
                break
            }
        }
        if (restored < 0) {
            for (let index = 0; index < nodes.length; ++index) {
                if (nodes[index].kind === wanted.kind && nodes[index].text === wanted.text) {
                    restored = index
                    break
                }
            }
        }
        selectIndex(restored, true)
        structureLayer.forceActiveFocus()
    }

    function insertProvisionalNewline() {
        if (provisionalInsert === null) return
        const position = sourceArea.cursorPosition
        const insertion = "\n" + provisionalInsert.continuation
        sourceArea.insert(position, insertion)
        sourceArea.cursorPosition = position + insertion.length
    }

    function enterNormal() {
        if (visualMode) { visualLoader.item.handleEscape(); return }
        const position = sourceArea.cursorPosition
        commitSource()
        documentController.endSourceEdit()
        selectAtPosition(position, false)
        commandMode = true
        structureLayer.forceActiveFocus()
    }

    function contentPosition(node) {
        if (!node) return sourceArea.cursorPosition
        const start = positionForLine(node.startLine)
        const newline = sourceArea.text.indexOf("\n", start)
        const line = sourceArea.text.substring(start, newline < 0 ? sourceArea.length : newline)
        let marker = null
        if (node.kind === "heading")
            marker = line.match(/^\s{0,3}#{1,6}[ \t]+/)
        else if (node.kind === "item" || node.kind === "list")
            marker = line.match(/^[ \t]*(?:[-+*]|\d+[.)])[ \t]+(?:\[[ xX]\][ \t]+)?/)
        return start + (marker ? marker[0].length : 0)
    }

    function enterInsert(position) {
        if (visualMode) { visualLoader.item.handleEscape(); return }
        const selected = currentNode()
        const targetPosition = position === undefined ? contentPosition(selected) : position
        documentController.beginSourceEdit()
        commandMode = false
        selectIndex(-1, false)
        sourceArea.forceActiveFocus()
        sourceArea.cursorPosition = Math.max(0, Math.min(targetPosition, sourceArea.length))
    }

    function enterOpenedFile() {
        documentController.endSourceEdit()
        commandMode = true
        sourceArea.cursorPosition = 0
        selectIndex(documentController.nodes.length > 0 ? 0 : -1, false)
        structureLayer.forceActiveFocus()
    }

    function saveNow() {
        commitSource()
        if (documentController.filePath)
            documentController.save()
        else
            saveDialog.open()
    }

    function prepareFileSwitch() {
        if (visualMode) { visualLoader.item.handleEscape(); return false }
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
            enterOpenedFile()
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
            font.pixelSize: 10
            font.bold: actionRoot.active
        }
        HoverHandler { id: actionHover }
        TapHandler {
            enabled: actionRoot.enabled
            onTapped: actionRoot.triggered()
        }
    }

    Loader {
        id: visualLoader
        anchors.fill: parent
        active: false
        visible: window.visualMode
        source: "Preview.qml"
        onLoaded: {
            item.selectLine(window.visualSelectionLine)
            if (window.pendingVisualAppend) {
                item.requestAppend(window.pendingVisualAppend.section, window.pendingVisualAppend.kind)
                window.pendingVisualAppend = null
            }
        }
    }
    Connections {
        target: visualLoader.item
        function onReturnToNormal(line) { window.leaveVisual(line) }
    }

    ScrollView {
        id: editorScroll
        visible: !window.visualMode
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
                if (window.provisionalInsert !== null &&
                        (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) &&
                        event.modifiers === Qt.NoModifier) {
                    window.finishProvisional(true)
                    event.accepted = true
                } else if (window.provisionalInsert !== null &&
                           (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) &&
                           event.modifiers === Qt.ShiftModifier) {
                    window.insertProvisionalNewline()
                    event.accepted = true
                } else if (!window.commandMode && event.key === Qt.Key_V &&
                        event.modifiers === Qt.ControlModifier) {
                    window.pasteClipboardImage()
                    event.accepted = true
                } else if (!window.commandMode && event.key === Qt.Key_V &&
                           event.modifiers === (Qt.ControlModifier | Qt.ShiftModifier)) {
                    window.pasteClipboardText()
                    event.accepted = true
                } else if (!window.commandMode &&
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
                if (typeof initialVisualMode !== "undefined" && initialVisualMode) Qt.callLater(window.showPreview)
                if (documentController.filePath)
                    window.enterOpenedFile()
                else {
                    documentController.beginSourceEdit()
                    forceActiveFocus()
                }
            }

            FocusScope {
                id: structureLayer
                objectName: "structureLayer"
                anchors.fill: parent
                visible: window.commandMode
                focus: visible
                Keys.onShortcutOverride: function(event) {
                    if (event.key === Qt.Key_A &&
                            event.modifiers === (Qt.ControlModifier | Qt.ShiftModifier))
                        event.accepted = true
                }
                Keys.onPressed: function(event) {
                    if (event.key === Qt.Key_A &&
                            event.modifiers === (Qt.ControlModifier | Qt.ShiftModifier) &&
                            window.selectedNode && window.selectedNode.kind === "item" &&
                            window.selectedNode.attachments &&
                            window.selectedNode.attachments.length > 0) {
                        window.showAttachments()
                        event.accepted = true
                    }
                }

                MouseArea {
                    objectName: "structureMouseArea"
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton
                    cursorShape: Qt.ArrowCursor
                    onClicked: function(mouse) {
                        const position = sourceArea.positionAt(mouse.x, mouse.y)
                        window.selectAtPosition(position, false)
                        structureLayer.forceActiveFocus()
                    }
                }
            }


            DropArea {
                anchors.fill: parent
                onDropped: function(drop) {
                    if (!drop.hasUrls || drop.urls.length === 0) return
                    const resumeSourceEdit = !window.commandMode
                    window.commitSource()
                    if (resumeSourceEdit)
                        documentController.endSourceEdit()
                    const position = sourceArea.positionAt(drop.x, drop.y)
                    window.selectAtPosition(position, false)
                    const selected = window.currentNode()
                    if (selected && selected.kind === "item") {
                        window.rememberSelection(selected.startLine)
                        if (documentController.attachImage(selected.identity, drop.urls[0]))
                            drop.acceptProposedAction()
                    }
                    if (resumeSourceEdit)
                        documentController.beginSourceEdit()
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
                    model: window.keyHelpBindings(window.selectedNode)

                    Row {
                        required property var modelData
                        width: parent.width
                        height: Math.max(bindingKeys.implicitHeight, bindingAction.implicitHeight)

                        Label {
                            id: bindingKeys
                            width: 190
                            text: modelData.keys
                            color: modelData.relevant ? window.accent : window.foreground
                            font.family: "monospace"
                            font.pixelSize: 12
                            font.bold: true
                        }
                        Label {
                            id: bindingAction
                            width: parent.width - bindingKeys.width
                            text: modelData.action
                            color: modelData.relevant ? window.foreground : window.muted
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

    Rectangle {
        id: attachmentPane
        objectName: "attachmentPane"
        anchors.fill: parent
        visible: window.attachmentsVisible
        focus: visible
        z: 20
        color: Qt.alpha(window.canvasColor, 0.96)

        Keys.onPressed: function(event) {
            const count = window.selectedNode && window.selectedNode.attachments
                          ? window.selectedNode.attachments.length : 0
            const imageShortcut = event.key === Qt.Key_A &&
                                  event.modifiers === (Qt.ControlModifier | Qt.ShiftModifier)
            if (event.key === Qt.Key_Escape || imageShortcut) {
                window.closeAttachments()
                event.accepted = true
            } else if ((event.key === Qt.Key_Down || event.key === Qt.Key_J) && count) {
                window.attachmentIndex = Math.min(count - 1, window.attachmentIndex + 1)
                event.accepted = true
            } else if ((event.key === Qt.Key_Up || event.key === Qt.Key_K) && count) {
                window.attachmentIndex = Math.max(0, window.attachmentIndex - 1)
                event.accepted = true
            } else if ((event.key === Qt.Key_D || event.key === Qt.Key_Delete) && count) {
                window.removeSelectedAttachment()
                event.accepted = true
            }
        }

        Column {
            anchors.centerIn: parent
            width: Math.min(720, parent.width - 80)
            spacing: 14

            Label {
                text: "attached images"
                color: window.accent
                font.family: "monospace"
                font.pixelSize: 15
                font.bold: true
            }

            Repeater {
                model: window.selectedNode && window.selectedNode.attachments
                       ? window.selectedNode.attachments : []

                Rectangle {
                    id: attachmentRow
                    required property var modelData
                    required property int index
                    width: parent.width
                    height: 104
                    color: index === window.attachmentIndex
                           ? Qt.alpha(window.accent, 0.14)
                           : Qt.alpha(window.foreground, 0.05)
                    border.color: index === window.attachmentIndex
                                  ? window.accent : Qt.alpha(window.foreground, 0.12)

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 12

                        Image {
                            Layout.preferredWidth: 88
                            Layout.preferredHeight: 88
                            source: modelData.url
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            cache: false
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            Label {
                                Layout.fillWidth: true
                                text: modelData.alt || "image"
                                color: window.foreground
                                elide: Text.ElideRight
                                font.family: "monospace"
                            }
                            Label {
                                Layout.fillWidth: true
                                text: modelData.path
                                color: window.muted
                                elide: Text.ElideMiddle
                                font.family: "monospace"
                                font.pixelSize: 10
                            }
                        }
                        Button {
                            text: "remove"
                            onClicked: {
                                const selected = window.currentNode()
                                if (selected)
                                    documentController.removeAttachment(selected.identity,
                                                                        modelData.path)
                            }
                        }
                    }
                }
            }

            Label {
                visible: !window.selectedNode || !window.selectedNode.attachments ||
                         window.selectedNode.attachments.length === 0
                text: "No images are associated with this item."
                color: window.muted
                font.family: "monospace"
            }

            Button {
                text: "close"
                onClicked: window.closeAttachments()
            }

            Label {
                text: "J/K or arrows select · D removes · Ctrl+Shift+A or Esc closes"
                color: window.muted
                font.family: "monospace"
                font.pixelSize: 11
            }
        }

        TapHandler {
            acceptedButtons: Qt.RightButton
            onTapped: window.closeAttachments()
        }
    }

    footer: Rectangle {
        objectName: "editorFooter"
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
                    text: window.visualMode ? "VISUAL" : window.commandMode ? "NORMAL" : "INSERT"
                    color: window.commandMode ? window.canvasColor : window.selectionForeground
                    font.family: "monospace"
                    font.bold: true
                    font.pixelSize: 11
                }
                TapHandler {
                    onTapped: window.commandMode ? window.enterInsert() : window.enterNormal()
                }
            }

            Label {
                id: documentSummaryLabel
                objectName: "documentSummaryLabel"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.preferredWidth: 0
                Layout.fillHeight: true
                verticalAlignment: Text.AlignVCenter
                leftPadding: 10
                rightPadding: 8
                text: documentController.conflict
                      ? documentController.displayPath + " · CONFLICT  " + documentController.status
                      : documentController.modified
                        ? documentController.displayPath + " · saving…"
                        : documentController.displayPath + " · " + documentController.status
                color: documentController.conflict ? "#d98b8b" : window.foreground
                elide: Text.ElideMiddle
                font.family: "monospace"
                font.pixelSize: 10
                ToolTip.visible: summaryHover.hovered && documentController.filePath !== ""
                ToolTip.text: documentController.filePath
                ToolTip.delay: 500
                HoverHandler { id: summaryHover }
            }

            FooterAction {
                visible: documentController.conflict
                label: "reload disk"
                onTriggered: reloadConflictDialog.open()
            }
            FooterAction {
                visible: documentController.conflict
                label: "save copy"
                onTriggered: conflictCopyDialog.open()
            }
            FooterAction {
                visible: documentController.recoveryAvailable
                label: "restore draft"
                onTriggered: documentController.restoreRecovery()
            }
            FooterAction {
                visible: documentController.recoveryAvailable
                label: "discard draft"
                onTriggered: discardRecoveryDialog.open()
            }

            FooterAction {
                objectName: "visualAction"
                visible: !documentController.conflict && !documentController.recoveryAvailable
                label: "visual"
                onTriggered: window.showPreview()
            }

            Rectangle {
                visible: !documentController.conflict && !documentController.recoveryAvailable
                Layout.fillHeight: true
                implicitWidth: 62
                color: window.accent
                Label {
                    objectName: "cursorPositionLabel"
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
            if (window.provisionalInsert === null) autosaveTimer.restart()
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
            if (documentController.modified && window.provisionalInsert === null)
                autosaveTimer.restart()
        }
        function onDocumentChanged() { window.restoreSelection() }
        function onThemeChanged() {
            markdownHighlighter.setColors(window.foreground, window.muted, window.accent)
        }
    }

    Shortcut {
        sequence: "Escape"
        enabled: window.visualMode && !window.keyHelpVisible && !window.recentVisible
        onActivated: visualLoader.item.handleEscape()
    }
    Shortcut {
        sequence: "Escape"
        enabled: window.provisionalInsert !== null
        onActivated: window.finishProvisional(false)
    }
    Shortcut {
        sequence: "Escape"
        enabled: !window.commandMode && !window.recentVisible &&
                 window.provisionalInsert === null
        onActivated: window.enterNormal()
    }
    Shortcut {
        sequence: "?"
        enabled: window.commandMode && !window.visualMode && !window.recentVisible && !window.attachmentsVisible
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
    Shortcut {
        sequence: "Escape"
        enabled: window.attachmentsVisible
        onActivated: window.closeAttachments()
    }
    Shortcut { sequence: "I"; enabled: window.structuralShortcutsEnabled; onActivated: window.enterInsert() }
    Shortcut { sequence: "Return"; enabled: window.structuralShortcutsEnabled; onActivated: window.enterInsert() }
    Shortcut { sequence: "O"; enabled: window.structuralShortcutsEnabled; onActivated: window.openSibling(true) }
    Shortcut { sequence: "Shift+O"; enabled: window.structuralShortcutsEnabled; onActivated: window.openSibling(false) }
    Shortcut { sequence: "A"; enabled: window.structuralShortcutsEnabled && window.canAddToCurrentList(); onActivated: window.openListBoundary(true) }
    Shortcut { sequence: "Ctrl+Return"; enabled: window.structuralShortcutsEnabled && window.canAddToCurrentList(); onActivated: window.openListBoundary(true) }
    Shortcut { sequence: "Shift+A"; enabled: window.structuralShortcutsEnabled && window.canAddToCurrentList(); onActivated: window.openListBoundary(false) }
    Shortcut { sequence: "Ctrl+Shift+Return"; enabled: window.structuralShortcutsEnabled && window.canAddToCurrentList(); onActivated: window.openListBoundary(false) }
    Shortcut { sequence: "J"; enabled: window.structuralShortcutsEnabled; onActivated: window.selectRelative(1) }
    Shortcut { sequence: "K"; enabled: window.structuralShortcutsEnabled; onActivated: window.selectRelative(-1) }
    Shortcut { sequence: "Left"; enabled: window.structuralShortcutsEnabled; onActivated: window.selectParent() }
    Shortcut { sequence: "Shift+J"; enabled: window.structuralShortcutsEnabled; onActivated: window.moveRelative(1) }
    Shortcut { sequence: "Shift+K"; enabled: window.structuralShortcutsEnabled; onActivated: window.moveRelative(-1) }
    Shortcut {
        sequence: "Shift+H"
        enabled: window.structuralShortcutsEnabled && window.canShiftLevel(-1, false)
        onActivated: window.shiftSelected("promote", false)
    }
    Shortcut {
        sequence: "Shift+L"
        enabled: window.structuralShortcutsEnabled && window.canShiftLevel(1, false)
        onActivated: window.shiftSelected("demote", false)
    }
    Shortcut {
        sequence: "Ctrl+Shift+H"
        enabled: window.structuralShortcutsEnabled && window.selectedNode &&
                 window.selectedNode.kind === "heading" && window.canShiftLevel(-1, true)
        onActivated: window.shiftSelected("promote", true)
    }
    Shortcut {
        sequence: "Ctrl+Shift+L"
        enabled: window.structuralShortcutsEnabled && window.selectedNode &&
                 window.selectedNode.kind === "heading" && window.canShiftLevel(1, true)
        onActivated: window.shiftSelected("demote", true)
    }
    Shortcut {
        sequence: "Space"
        enabled: window.structuralShortcutsEnabled && window.selectedNode && window.selectedNode.task
        onActivated: window.applySelected("toggle_task")
    }
    Shortcut {
        sequence: "D"
        enabled: window.structuralShortcutsEnabled && window.selectedNode
        onActivated: window.deleteSelectedStructure()
    }
    Shortcut {
        sequence: "Delete"
        enabled: window.structuralShortcutsEnabled && window.selectedNode
        onActivated: window.deleteSelectedStructure()
    }
    Shortcut {
        sequence: "P"
        enabled: window.structuralShortcutsEnabled &&
                 window.selectedNode && window.selectedNode.kind === "item"
        onActivated: window.pasteClipboardImage()
    }
    Shortcut {
        sequence: "Ctrl+V"
        enabled: window.structuralShortcutsEnabled
        onActivated: window.pasteClipboardImage()
    }
    Shortcut {
        sequence: "Ctrl+Shift+V"
        enabled: window.structuralShortcutsEnabled
        onActivated: window.pasteClipboardText()
    }
    Shortcut {
        sequence: "Shift+V"
        enabled: window.structuralShortcutsEnabled
        onActivated: window.showPreview()
    }
    Shortcut {
        sequence: "Ctrl+N"
        onActivated: {
            if (!window.prepareFileSwitch()) return
            visualLoader.active = false
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
    Shortcut {
        sequence: "Ctrl+Z"
        enabled: window.commandMode && !window.visualMode && documentController.canUndo
        onActivated: window.undoSelectedChange()
    }
    Shortcut { sequence: "Ctrl+Shift+S"; onActivated: { window.commitSource(); saveDialog.open() } }
    Shortcut { sequence: "Ctrl+Q"; onActivated: window.close() }

    FileDialog {
        id: openDialog
        title: "Open Markdown file"
        fileMode: FileDialog.OpenFile
        nameFilters: ["Markdown files (*.md *.markdown)", "Text files (*.txt)", "All files (*)"]
        onAccepted: {
            if (documentController.loadFile(selectedFile))
                window.enterOpenedFile()
        }
    }

    FileDialog {
        id: conflictCopyDialog
        title: "Save local work as a copy"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "md"
        nameFilters: ["Markdown files (*.md)"]
        onAccepted: documentController.saveCopy(selectedFile)
    }

    Dialog {
        id: reloadConflictDialog
        anchors.centerIn: parent
        modal: true
        title: "Reload the disk version?"
        standardButtons: Dialog.Yes | Dialog.Cancel
        onAccepted: documentController.reloadFromDisk()
        Label {
            text: "Your local edits remain in the recovery draft until the reload completes."
            color: window.foreground
            wrapMode: Text.Wrap
        }
    }

    Dialog {
        id: discardRecoveryDialog
        anchors.centerIn: parent
        modal: true
        title: "Discard the recovery draft?"
        standardButtons: Dialog.Yes | Dialog.Cancel
        onAccepted: documentController.discardRecovery()
        Label {
            text: "This removes the saved recovery copy."
            color: window.foreground
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
