import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Dialogs as Dialogs
import QtQuick.Layouts
import QtQuick.Window
import "EditorMutations.js" as EditorMutations

ApplicationWindow {
    id: win
    width: 1280
    height: 820
    minimumWidth: 720
    minimumHeight: 520
    visible: true
    title: (backend.modified ? "* " : "") + backend.fileName + " - Omascribe"

    readonly property bool darkMode: backend.darkMode
    readonly property color pageColor: backend.themeBackground
    readonly property color textColor: backend.themeForeground
    readonly property color strongTextColor: backend.themeForeground
    readonly property color mutedColor: darkMode ? "#909191" : "#aeb1b5"
    readonly property color selectionFill: backend.themeSelection
    // The desktop's text size knob (GNOME's text-scaling-factor, which
    // `omarchy display text size` drives) anchored so its 12px default leaves
    // the app at the sizes it was designed around.
    readonly property real textScale: backend.textScale
    readonly property int editorFontPixelSize: scaledSize(20)
    readonly property int footerHeight: scaledSize(40)
    readonly property string viewMode: backend.viewMode
    property bool synchronizingScroll: false
    property bool cursorEnsurePending: false
    property real scrollProgress: 0

    function pageWidth(availableWidth) {
        return Math.min(Math.round(writerFontMetrics.averageCharacterWidth * 65),
                        Math.max(1, availableWidth - scaledSize(24)));
    }

    function trackScroll(viewport, userNavigation) {
        if (synchronizingScroll || !viewport.visible
                || (scrollSync.running && !userNavigation && !viewport.userScrolling))
            return;
        var extent = Math.max(0, viewport.contentHeight - viewport.height);
        scrollProgress = extent > 0 ? Math.max(0, Math.min(1, viewport.contentY / extent)) : 0;
        if (viewMode === "split")
            alignScroll(viewport);
    }

    function alignScroll(leader) {
        synchronizingScroll = true;
        if (leader !== editorFlick && sourcePane.visible)
            editorFlick.scrollTo(scrollProgress * Math.max(0, editorFlick.contentHeight - editorFlick.height));
        if (leader !== previewFlick && previewPane.visible)
            previewFlick.scrollTo(scrollProgress * Math.max(0, previewFlick.contentHeight - previewFlick.height));
        synchronizingScroll = false;
    }

    Timer {
        id: scrollSync
        interval: 0
        onTriggered: {
            win.alignScroll(null);
            if (win.cursorEnsurePending) {
                win.cursorEnsurePending = false;
                editorFlick.ensureCursorVisible();
            }
        }
    }

    Timer {
        id: previewUpdate
        interval: 0
        onTriggered: {
            if (renderedPreview.baseUrl.toString() !== backend.previewBaseUrl.toString()) {
                // File transitions replace text before publishing the new path.
                // Update together after that transition, avoiding stale image URLs.
                renderedPreview.text = "";
                renderedPreview.baseUrl = backend.previewBaseUrl;
            }
            renderedPreview.text = editor.text;
            backend.formatPreviewDocument(renderedPreview.textDocument, win.editorFontPixelSize, editor.text);
        }
    }
    Connections {
        target: editor
        function onTextChanged() { previewUpdate.restart(); }
    }
    Connections {
        target: backend
        function onFileUrlChanged() { previewUpdate.restart(); }
    }

    function focusDocument() {
        if (viewMode === "preview") renderedPreview.forceActiveFocus();
        else editor.forceActiveFocus();
    }

    function openSearch(replace) {
        if (viewMode === "preview") backend.viewMode = "split";
        searchOpen = true;
        replaceOpen = replace;
        searchField.forceActiveFocus();
        searchField.selectAll();
    }

    onViewModeChanged: {
        scrollSync.restart();
        if (viewMode === "preview" && searchOpen) closeSearch();
        Qt.callLater(function() {
            if (!win.searchOpen) win.focusDocument();
        });
    }
    property bool closeConfirmed: false
    property bool searchOpen: false
    property bool searchUpdating: false
    property var searchMatches: []
    property int searchMatchIndex: -1
    readonly property string pendingAction: session.pendingAction
    property bool replaceOpen: false

    Material.theme: darkMode ? Material.Dark : Material.Light
    Material.accent: backend.themeAccent
    color: pageColor

    onClosing: function(close) {
        if (closeConfirmed)
            return;
        close.accepted = session.requestClose();
    }

    function requestOpen(url) {
        session.requestOpen(url);
    }

    FontMetrics {
        id: writerFontMetrics
        font.family: backend.editorFont
        font.pixelSize: win.editorFontPixelSize
    }

    // Every hardcoded size in the interface is expressed at text scale 1.
    function scaledSize(pixels) {
        return Math.max(1, Math.round(pixels * win.textScale));
    }

    function toggleFullScreen() {
        win.visibility = win.visibility === Window.FullScreen
            ? Window.Windowed
            : Window.FullScreen;
    }

    function updateSearch() {
        var matches = [];
        var query = searchField.text;
        if (query.length > 0) {
            var haystack = editor.text.toLocaleLowerCase();
            var needle = query.toLocaleLowerCase();
            var position = 0;
            while ((position = haystack.indexOf(needle, position)) !== -1) {
                matches.push(position);
                position += Math.max(1, needle.length);
            }
        }
        searchMatches = matches;
        searchMatchIndex = matches.length > 0 ? 0 : -1;
        showSearchMatch();
    }

    function showSearchMatch() {
        var start = searchMatchIndex >= 0 ? searchMatches[searchMatchIndex] : -1;
        searchUpdating = true;
        backend.setSearchHighlight(searchField.text, start);
        if (start >= 0) {
            editor.select(start, start + searchField.text.length);
            editorFlick.ensureCursorVisible();
        }
        searchUpdating = false;
    }

    function moveSearch(direction) {
        if (searchMatches.length === 0)
            return;
        searchMatchIndex = (searchMatchIndex + direction + searchMatches.length)
                           % searchMatches.length;
        showSearchMatch();
    }

    function closeSearch() {
        searchOpen = false;
        searchUpdating = true;
        backend.setSearchHighlight("", -1);
        editor.deselect();
        searchUpdating = false;
        replaceOpen = false;
        win.focusDocument();
    }

    Shortcut {
        sequence: "Ctrl+S"
        context: Qt.ApplicationShortcut
        onActivated: session.requestSave()
    }

    Shortcut {
        sequence: "Ctrl+H"
        context: Qt.ApplicationShortcut
        onActivated: {
            win.openSearch(true);
        }
    }

    Shortcut {
        sequence: "Ctrl+B"
        context: Qt.WindowShortcut
        enabled: win.viewMode !== "preview" && editor.activeFocus
        onActivated: editor.wrapSelection("**", "**")
    }

    Shortcut {
        sequence: "Ctrl+I"
        context: Qt.WindowShortcut
        enabled: win.viewMode !== "preview" && editor.activeFocus
        onActivated: editor.wrapSelection("*", "*")
    }

    Shortcut {
        sequence: "Ctrl+K"
        context: Qt.WindowShortcut
        enabled: win.viewMode !== "preview" && editor.activeFocus
        onActivated: editor.insertLink()
    }

    Shortcut {
        sequence: "Ctrl+?"
        context: Qt.ApplicationShortcut
        onActivated: shortcutsDialog.open()
    }

    Shortcut {
        sequence: "Ctrl+O"
        context: Qt.ApplicationShortcut
        onActivated: backend.openDialog()
    }

    Shortcut {
        sequence: "Ctrl+N"
        context: Qt.ApplicationShortcut
        onActivated: backend.newWindow()
    }

    Shortcut {
        sequence: "Ctrl+Shift+S"
        context: Qt.ApplicationShortcut
        onActivated: session.requestSaveAs()
    }

    Shortcut {
        sequence: "Ctrl+P"
        context: Qt.ApplicationShortcut
        onActivated: session.printDocument()
    }

    Shortcut {
        sequences: ["Meta+F", "F11"]
        context: Qt.ApplicationShortcut
        onActivated: toggleFullScreen()
    }

    Shortcut {
        sequence: "Ctrl+Z"
        context: Qt.WindowShortcut
        enabled: win.viewMode !== "preview" && editor.activeFocus
        onActivated: editor.undo()
    }

    Shortcut {
        sequences: ["Ctrl+Shift+Z", "Ctrl+Y"]
        context: Qt.WindowShortcut
        enabled: win.viewMode !== "preview" && editor.activeFocus
        onActivated: editor.redo()
    }

    Shortcut {
        sequence: "Ctrl+F"
        context: Qt.ApplicationShortcut
        onActivated: {
            win.openSearch(false);
        }
    }

    Shortcut {
        sequence: "Ctrl+G"
        context: Qt.ApplicationShortcut
        enabled: win.searchOpen
        onActivated: win.moveSearch(1)
    }

    Connections {
        target: backend

        function onOpenDialogRequested() {
            openFileDialog.open();
        }

        function onSaveDialogRequested(suggestedUrl) {
            saveFileDialog.selectedFile = suggestedUrl;
            saveFileDialog.open();
        }

        function onExternalChangeDetected(deleted, locallyModified) {
            externalChangeDialog.deleted = deleted;
            externalChangeDialog.locallyModified = locallyModified;
            externalChangeDialog.open();
        }
    }

    Connections {
        target: session
        function onUnsavedDecisionRequested() { unsavedChangesDialog.open(); }
        function onCloseRequested() {
            win.closeConfirmed = true;
            win.close();
        }
        function onExternalResolved() { externalChangeDialog.close(); }
    }

    Dialogs.FileDialog {
        id: openFileDialog
        objectName: "openFileDialog"
        title: "Open File"
        fileMode: Dialogs.FileDialog.OpenFile
        nameFilters: ["Markdown files (*.md *.markdown)", "All files (*)"]
        onAccepted: win.requestOpen(selectedFile)
        onVisibleChanged: session.setDialog("open", visible)
    }

    Dialogs.FileDialog {
        id: saveFileDialog
        objectName: "saveFileDialog"
        title: "Save File"
        fileMode: Dialogs.FileDialog.SaveFile
        nameFilters: ["Markdown files (*.md *.markdown)", "All files (*)"]
        onAccepted: session.acceptSave(selectedFile)
        onVisibleChanged: session.setDialog("save", visible)
        onRejected: {
            backend.fileDialogCanceled();
            session.cancelPending();
        }
    }

    UnsavedChangesDialog {
        id: unsavedChangesDialog
        objectName: "unsavedChangesDialog"
        onVisibleChanged: session.setDialog("unsaved", visible)
        onOpened: session.setDialog("unsaved", true)
        onClosed: session.setDialog("unsaved", false)
        fileName: backend.fileName
        darkMode: win.darkMode
        textScale: win.textScale
        textColor: win.textColor
        strongTextColor: win.strongTextColor
        activeButtonColor: backend.themeAccent
        containerWidth: win.width
        containerHeight: win.height

        onDiscardRequested: session.resolvePending("discard")
        onSaveRequested: session.resolvePending("save")
        onCancelRequested: session.resolvePending("cancel")
    }

    ExternalChangeDialog {
        id: externalChangeDialog
        objectName: "externalChangeDialog"
        onVisibleChanged: session.setDialog("external", visible)
        onOpened: session.setDialog("external", true)
        onClosed: session.setDialog("external", false)
        darkMode: win.darkMode
        textScale: win.textScale
        textColor: win.textColor
        strongTextColor: win.strongTextColor
        containerWidth: win.width
        containerHeight: win.height

        onKeepRequested: session.resolveExternal("keep")
        onReloadRequested: session.resolveExternal("reload")
    }

    Dialog {
        id: shortcutsDialog
        objectName: "shortcutsDialog"
        modal: true
        title: "Keyboard shortcuts"
        standardButtons: Dialog.Close
        anchors.centerIn: parent
        width: Math.min(win.width - 48, 600)
        contentItem: Label {
            text: "Ctrl+S  Save\nCtrl+Shift+S  Save As\nCtrl+O  Open\nCtrl+N  New Window\nCtrl+F  Find\nCtrl+H  Find and Replace\nCtrl+B  Bold\nCtrl+I  Italic\nCtrl+K  Link\nCtrl+P  Print\nF11 / Super+F  Fullscreen\nCtrl+?  Shortcuts"
            lineHeight: 1.5
            wrapMode: Text.Wrap
        }
    }

    Item {
        anchors.fill: parent

        SplitView {
            id: documentSplit
            objectName: "documentSplit"
            anchors.fill: parent
            anchors.leftMargin: 24
            anchors.rightMargin: 24
            anchors.topMargin: win.footerHeight
            anchors.bottomMargin: win.footerHeight
            orientation: Qt.Horizontal
            property real usableWidth: Math.max(1, width - win.scaledSize(6))
            onResizingChanged: {
                if (!resizing && win.viewMode === "split")
                    backend.splitRatio = sourcePane.width / usableWidth;
            }
            onWidthChanged: restoreSplitWidth()
            function restoreSplitWidth() {
                if (!resizing)
                    sourcePane.SplitView.preferredWidth = usableWidth * backend.splitRatio;
            }
            Connections {
                target: backend
                function onViewModeChanged() { documentSplit.restoreSplitWidth(); }
                function onSplitRatioChanged() { documentSplit.restoreSplitWidth(); }
            }
            handle: Rectangle {
                implicitWidth: win.scaledSize(6)
                color: "transparent"
                Rectangle {
                    anchors.centerIn: parent
                    width: 1
                    height: parent.height
                    color: backend.themeAccent
                    opacity: SplitHandle.pressed ? 0.8 : SplitHandle.hovered ? 0.5 : 0.18
                }
            }

            Item {
                id: sourcePane
                objectName: "sourcePane"
                visible: win.viewMode !== "preview"
                onWidthChanged: scrollSync.restart()
                SplitView.minimumWidth: win.viewMode === "split"
                    ? Math.min(win.scaledSize(220), documentSplit.usableWidth / 2) : 0
                SplitView.fillWidth: win.viewMode === "source"

                DocumentViewport {
                    id: editorFlick
                    objectName: "sourceViewport"
                    anchors.fill: parent
                    textScale: win.textScale
                    contentWidth: width
                    contentHeight: Math.max(height, editor.y + editor.implicitHeight + 220)
                    onContentYChanged: win.trackScroll(editorFlick)
                    onScrollNavigated: win.trackScroll(editorFlick, true)
                    onContentHeightChanged: scrollSync.restart()
                    onHeightChanged: scrollSync.restart()
                    // Keep the editing caret within the viewport so writing past the
                    // bottom edge scrolls the page along with the text.
                    function ensureCursorVisible() {
                        if (!sourcePane.visible || scrollSync.running) return;
                        var margin = win.editorFontPixelSize * 2;
                        var cursorTop = editor.y + editor.cursorRectangle.y;
                        var cursorBottom = cursorTop + editor.cursorRectangle.height;
                        var maxContentY = Math.max(0, contentHeight - height);

                        if (cursorBottom + margin > contentY + height)
                            scrollTo(Math.min(maxContentY, cursorBottom + margin - height));
                        else if (cursorTop - margin < contentY)
                            scrollTo(Math.max(0, cursorTop - margin));
                    }

                    TextEdit {
                        id: editor
                        objectName: "sourceEditor"
                        x: Math.round((editorFlick.width - width) / 2)
                        // The resting position counts from the window's top edge, so
                        // take back the top inset the Flickable already adds.
                        y: Math.max(0, Math.max(42, Math.round(win.height * 0.05)) - win.footerHeight)
                        width: win.pageWidth(editorFlick.width)
                        height: Math.max(editorFlick.height - y - 96, implicitHeight + 20)
                        text: ""
                        textFormat: TextEdit.PlainText
                        wrapMode: TextEdit.Wrap
                        selectByMouse: true
                        persistentSelection: true
                        activeFocusOnPress: true
                        color: win.textColor
                        selectedTextColor: win.strongTextColor
                        selectionColor: win.selectionFill
                        font.family: backend.editorFont
                        font.pixelSize: win.editorFontPixelSize
                        font.weight: Font.Normal
                        // Native rendering hints glyphs to the pixel grid, which is
                        // crispest at whole scale factors but misplaces and unevenly
                        // rasterizes glyphs at fractional ones (and goes stale when
                        // the compositor delivers the fractional scale after the
                        // first frame). Fall back to Qt's scalable renderer there.
                        renderType: Screen.devicePixelRatio % 1 === 0 ? TextEdit.NativeRendering : TextEdit.QtRendering
                        cursorDelegate: Rectangle {
                            width: 1
                            color: win.strongTextColor
                        }
                        // Reflow changes the caret rectangle without user
                        // navigation. Keep the reading position during resizing.
                        onCursorRectangleChanged: {
                            if (win.cursorEnsurePending) editorFlick.ensureCursorVisible();
                        }
                        onCursorPositionChanged: {
                            win.cursorEnsurePending = true;
                            scrollSync.restart();
                        }
                        onPreeditTextChanged: {
                            win.cursorEnsurePending = true;
                            scrollSync.restart();
                        }
                        onFontChanged: backend.documentFontChanged()

                        function replaceSelectionWith(replacement) {
                            var start = Math.min(selectionStart, selectionEnd);
                            var end = Math.max(selectionStart, selectionEnd);
                            EditorMutations.replaceRange(editor, start, end, replacement);
                        }

                        function wrapSelection(before, after) {
                            forceActiveFocus();
                            var start = Math.min(selectionStart, selectionEnd);
                            var end = Math.max(selectionStart, selectionEnd);
                            var selected = text.slice(start, end);
                            EditorMutations.replaceRange(editor, start, end,
                                                         before + selected + after,
                                                         before.length,
                                                         before.length + selected.length);
                        }

                        function insertLink() {
                            var start = Math.min(selectionStart, selectionEnd);
                            var end = Math.max(selectionStart, selectionEnd);
                            var selected = text.slice(start, end);
                            var url = backend.clipboardUrl();
                            var label = selected.length > 0 ? selected : "link text";
                            var destination = url.length > 0 ? url : "https://";
                            var escapedLabel = escapeMarkdownLinkText(label);
                            var markdown = "[" + escapedLabel + "](" + escapeMarkdownLinkDestination(destination) + ")";
                            if (selected.length === 0) {
                                EditorMutations.replaceRange(editor, start, end, markdown,
                                                             1, 1 + escapedLabel.length);
                            } else if (url.length === 0) {
                                EditorMutations.replaceRange(editor, start, end, markdown,
                                                             escapedLabel.length + 3,
                                                             markdown.length - 1);
                            } else {
                                EditorMutations.replaceRange(editor, start, end, markdown);
                            }
                        }

                        function smartReturn(softBreak) {
                            if (softBreak) {
                                replaceSelectionWith("\n");
                                return;
                            }
                            var lineStart = text.lastIndexOf("\n", cursorPosition - 1) + 1;
                            var line = text.slice(lineStart, cursorPosition);
                            var before = text.slice(0, cursorPosition);
                            var fences = (before.match(/^\s*```/gm) || []).length;
                            if ((fences % 2) === 1) {
                                replaceSelectionWith("\n");
                                return;
                            }
                            var match = line.match(/^(\s*)([-+*]|\d+[.)]|>+)\s+(.*)$/);
                            if (match) {
                                if (match[3].length === 0) {
                                    EditorMutations.replaceRange(editor, lineStart,
                                                                 cursorPosition, "\n");
                                } else {
                                    var marker = match[2];
                                    if (/^\d/.test(marker))
                                        marker = (parseInt(marker) + 1) + marker.slice(-1);
                                    replaceSelectionWith("\n" + match[1] + marker + " ");
                                }
                                return;
                            }
                            replaceSelectionWith("\n\n");
                        }

                        function escapeMarkdownLinkText(linkText) {
                            return linkText.replace(/\\/g, "\\\\")
                                           .replace(/\[/g, "\\[")
                                           .replace(/\]/g, "\\]");
                        }

                        function escapeMarkdownLinkDestination(linkUrl) {
                            return linkUrl.replace(/\\/g, "\\\\")
                                          .replace(/\(/g, "\\(")
                                          .replace(/\)/g, "\\)");
                        }

                        function pasteClipboardUrlAsMarkdownLink() {
                            var start = Math.min(selectionStart, selectionEnd);
                            var end = Math.max(selectionStart, selectionEnd);
                            if (start === end)
                                return false;

                            var url = backend.clipboardUrl();
                            if (url === "")
                                return false;

                            var selected = text.slice(start, end);
                            var leading = selected.match(/^\s*/)[0];
                            var trailing = selected.match(/\s*$/)[0];
                            var linkText = selected.slice(leading.length,
                                                          selected.length - trailing.length);
                            if (linkText === "")
                                return false;

                            replaceSelectionWith(leading + "[" + escapeMarkdownLinkText(linkText) + "]("
                                                 + escapeMarkdownLinkDestination(url) + ")" + trailing);
                            return true;
                        }

                        function pasteClipboardAsPlainText() {
                            var pastedText = backend.clipboardText();
                            if (pastedText.length > 0)
                                replaceSelectionWith(pastedText);
                        }

                        function skipHiddenForward(position) {
                            var pos = position;
                            var ranges = backend.hiddenRangesAt(pos);
                            for (var i = 0; i < ranges.length; i++) {
                                if (pos >= ranges[i].start && pos < ranges[i].end) {
                                    pos = ranges[i].end;
                                    i = -1;
                                }
                            }
                            return pos;
                        }

                        function skipHiddenBackward(position) {
                            var pos = position;
                            var ranges = backend.hiddenRangesAt(pos);
                            for (var i = ranges.length - 1; i >= 0; i--) {
                                if (pos > ranges[i].start && pos <= ranges[i].end) {
                                    pos = ranges[i].start;
                                    i = ranges.length;
                                }
                            }
                            return pos;
                        }

                        function moveCursorVisibly(direction) {
                            if (selectionStart !== selectionEnd) {
                                cursorPosition = direction > 0
                                    ? Math.max(selectionStart, selectionEnd)
                                    : Math.min(selectionStart, selectionEnd);
                                return;
                            }

                            var pos = Math.max(0, Math.min(text.length, cursorPosition + direction));
                            cursorPosition = direction > 0
                                ? skipHiddenForward(pos)
                                : skipHiddenBackward(pos);
                        }

                        function movePage(direction, extendSelection) {
                            var pageStep = Math.max(win.editorFontPixelSize,
                                                    editorFlick.height - win.editorFontPixelSize * 2);
                            var rect = cursorRectangle;
                            var targetY = rect.y + rect.height / 2 + direction * pageStep;
                            var target = positionAt(rect.x, Math.max(0, targetY));
                            if (extendSelection)
                                moveCursorSelection(target, TextEdit.SelectCharacters);
                            else
                                cursorPosition = target;
                        }

                        function deleteParagraphBreakBehindCursor() {
                            if (selectionStart !== selectionEnd || cursorPosition < 2)
                                return false;

                            if (text.slice(cursorPosition - 2, cursorPosition) !== "\n\n")
                                return false;

                            var start = cursorPosition - 2;
                            remove(start, cursorPosition);
                            cursorPosition = start;
                            return true;
                        }

                        Keys.priority: Keys.BeforeItem
                        Keys.onPressed: function(event) {
                            var pasteKey = (event.key === Qt.Key_V)
                                && (event.modifiers & Qt.ControlModifier)
                                && !(event.modifiers & (Qt.AltModifier | Qt.MetaModifier | Qt.ShiftModifier));
                            var shiftInsert = (event.key === Qt.Key_Insert)
                                && (event.modifiers & Qt.ShiftModifier)
                                && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier));
                            if (pasteKey || shiftInsert) {
                                if (!pasteClipboardUrlAsMarkdownLink())
                                    pasteClipboardAsPlainText();
                                event.accepted = true;
                                return;
                            }

                            var returnKey = event.key === Qt.Key_Return || event.key === Qt.Key_Enter;
                            var commandModifier = event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier);
                            if (returnKey && !commandModifier) {
                                smartReturn(event.modifiers & Qt.ShiftModifier);
                                event.accepted = true;
                            } else if (!commandModifier && event.key === Qt.Key_Backspace
                                       && deleteParagraphBreakBehindCursor()) {
                                event.accepted = true;
                            } else if (!commandModifier && !(event.modifiers & Qt.ShiftModifier)
                                       && event.key === Qt.Key_Right) {
                                moveCursorVisibly(1);
                                event.accepted = true;
                            } else if (!commandModifier && !(event.modifiers & Qt.ShiftModifier)
                                       && event.key === Qt.Key_Left) {
                                moveCursorVisibly(-1);
                                event.accepted = true;
                            } else if (!commandModifier
                                       && (event.key === Qt.Key_PageDown || event.key === Qt.Key_PageUp)) {
                                movePage(event.key === Qt.Key_PageDown ? 1 : -1,
                                         event.modifiers & Qt.ShiftModifier);
                                event.accepted = true;
                            }
                        }

                        onTextChanged: {
                            if (win.searchUpdating)
                                return;
                            var contentChanged = backend.editorTextChanged();
                            if (win.searchOpen && contentChanged)
                                win.updateSearch();
                        }

                        Text {
                            anchors.left: parent.left
                            anchors.top: parent.top
                            text: "# Start writing"
                            visible: editor.text.length === 0 && !editor.activeFocus
                            color: win.mutedColor
                            font.family: editor.font.family
                            font.pixelSize: editor.font.pixelSize
                            font.weight: editor.font.weight
                        }

                        Component.onCompleted: {
                            backend.attachDocument(textDocument);
                        }
                    }
                }

            }

            Item {
                id: previewPane
                objectName: "previewPane"
                visible: win.viewMode !== "source"
                onWidthChanged: scrollSync.restart()
                SplitView.minimumWidth: win.viewMode === "split"
                    ? Math.min(win.scaledSize(220), documentSplit.usableWidth / 2) : 0
                SplitView.fillWidth: true

                DocumentViewport {
                    id: previewFlick
                    objectName: "previewViewport"
                    anchors.fill: parent
                    textScale: win.textScale
                    contentWidth: Math.max(width, renderedPreview.x + renderedPreview.contentWidth + win.scaledSize(12))
                    contentHeight: Math.max(height, renderedPreview.y + renderedPreview.contentHeight + 220)
                    onContentYChanged: win.trackScroll(previewFlick)
                    onScrollNavigated: win.trackScroll(previewFlick, true)
                    onContentHeightChanged: scrollSync.restart()
                    onHeightChanged: scrollSync.restart()
                    ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }

                    TextEdit {
                        id: renderedPreview
                        objectName: "renderedPreview"
                        x: Math.round((previewFlick.width - width) / 2)
                        y: editor.y
                        width: win.pageWidth(previewFlick.width)
                        height: Math.max(previewFlick.height - y - 96, contentHeight + 20)
                        text: ""
                        textFormat: TextEdit.MarkdownText
                        baseUrl: ""
                        readOnly: true
                        selectByMouse: true
                        selectByKeyboard: true
                        persistentSelection: true
                        wrapMode: TextEdit.Wrap
                        color: win.textColor
                        selectionColor: win.selectionFill
                        selectedTextColor: win.strongTextColor
                        font.family: backend.editorFont
                        font.pixelSize: win.editorFontPixelSize
                        renderType: editor.renderType
                        onFontChanged: previewUpdate.restart()
                        onLinkActivated: function(link) { backend.openExternalUrl(link); }
                        Accessible.name: "Markdown preview"
                    }
                }
            }
            Component.onCompleted: restoreSplitWidth()
        }

        ViewModeSelector {
            id: modeToggle
            objectName: "modeToggle"
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: win.scaledSize(4)
            textScale: win.textScale
            mode: win.viewMode
            textColor: win.mutedColor
            accentColor: backend.themeAccent
            onModeChosen: function(mode) {
                backend.viewMode = mode;
                Qt.callLater(function() {
                    if (!win.searchOpen) win.focusDocument();
                });
            }
        }

        Row {
            id: footerStatus
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.leftMargin: 12
            anchors.bottomMargin: 10
            spacing: 12
            opacity: 0.55

            FooterIconButton {
                objectName: "saveButton"
                iconName: "save"
                iconColor: win.mutedColor
                tooltip: "Save"
                onClicked: session.requestSave()
            }

            FooterIconButton {
                objectName: "openButton"
                iconName: "open"
                iconColor: win.mutedColor
                tooltip: "Open"
                onClicked: backend.openDialog()
            }

            FooterIconButton {
                id: fontButton
                objectName: "fontButton"
                iconName: "font"
                iconColor: win.mutedColor
                tooltip: "Font"
                onClicked: fontPicker.opened ? fontPicker.close() : fontPicker.open()
            }

            Label {
                text: backend.status
                color: win.mutedColor
                font.family: "IBM Plex Mono"
                font.pixelSize: win.scaledSize(11)
                visible: text !== ""
                elide: Text.ElideRight
                width: Math.max(0, modeToggle.x - footerStatus.x - x - 12)
                height: win.scaledSize(16)
                verticalAlignment: Text.AlignVCenter
            }
        }

        FontPicker {
            id: fontPicker
            objectName: "fontPicker"
            x: footerStatus.x + fontButton.x - 8
            y: footerStatus.y - height - 10
            width: win.scaledSize(320)
            maximumHeight: Math.min(win.scaledSize(440), win.height - 80)
            fonts: backend.availableFonts
            currentFont: backend.editorFont
            defaultFont: "IBM Plex Mono"
            darkMode: win.darkMode
            textScale: win.textScale
            textColor: win.textColor
            mutedColor: win.mutedColor
            highlightColor: backend.themeAccent
            onFontChosen: function(family) { backend.editorFont = family; }
            onClosed: win.focusDocument()
        }

        Label {
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.rightMargin: 12
            anchors.bottomMargin: 10
            text: backend.wordCount + (backend.wordCount === 1 ? " Word" : " Words")
            color: win.mutedColor
            opacity: 0.75
            font.family: "IBM Plex Mono"
            font.pixelSize: win.scaledSize(11)
        }


        Pane {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.topMargin: 12
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            height: win.scaledSize(win.replaceOpen ? 104 : 56)
            visible: win.searchOpen
            z: 10
            leftPadding: 16
            rightPadding: 8
            topPadding: 0
            bottomPadding: 0
            Material.elevation: 8

            background: Rectangle {
                radius: 9
                color: win.darkMode ? "#22221f" : "#fffef2"
            }

            RowLayout {
                anchors.fill: parent
                spacing: 8

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    TextInput {
                        id: searchField
                        objectName: "searchField"
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        height: win.replaceOpen ? parent.height / 2 : parent.height
                        verticalAlignment: TextInput.AlignVCenter
                        selectByMouse: true
                        color: win.textColor
                        selectionColor: win.selectionFill
                        selectedTextColor: win.strongTextColor
                        font.pixelSize: win.scaledSize(17)
                        clip: true
                        onTextChanged: win.updateSearch()
                        Keys.onReturnPressed: function(event) {
                            win.moveSearch((event.modifiers & Qt.ShiftModifier) ? -1 : 1);
                            event.accepted = true;
                        }
                        Keys.onEscapePressed: function(event) {
                            win.closeSearch();
                            event.accepted = true;
                        }
                    }

                    TextInput {
                        id: replaceField
                        objectName: "replaceField"
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: parent.height / 2
                        visible: win.replaceOpen
                        verticalAlignment: TextInput.AlignVCenter
                        color: win.textColor
                        selectionColor: win.selectionFill
                        selectedTextColor: win.strongTextColor
                        font.pixelSize: win.scaledSize(17)
                        Keys.onReturnPressed: replaceCurrentButton.clicked()
                    }

                    Label {
                        anchors.verticalCenter: replaceField.verticalCenter
                        text: "Replace with"
                        visible: win.replaceOpen && replaceField.text.length === 0
                        color: win.mutedColor
                        font.pixelSize: win.scaledSize(17)
                    }

                    Label {
                        anchors.verticalCenter: searchField.verticalCenter
                        text: "Find"
                        visible: searchField.text.length === 0
                        color: win.mutedColor
                        font.pixelSize: win.scaledSize(17)
                    }
                }

                Label {
                    Layout.preferredWidth: win.scaledSize(58)
                    Layout.fillHeight: true
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    text: win.searchMatches.length === 0
                        ? "0/0"
                        : (win.searchMatchIndex + 1) + "/" + win.searchMatches.length
                    color: win.darkMode ? win.textColor : "#62635f"
                    font.pixelSize: win.scaledSize(16)
                }

                Button {
                    id: replaceCurrentButton
                    objectName: "replaceCurrentButton"
                    visible: win.replaceOpen
                    text: "Replace"
                    onClicked: {
                        if (win.searchMatchIndex < 0) return;
                        var start = win.searchMatches[win.searchMatchIndex];
                        EditorMutations.replaceRange(editor, start,
                                                     start + searchField.text.length,
                                                     replaceField.text);
                        win.updateSearch();
                    }
                }

                Button {
                    objectName: "replaceAllButton"
                    visible: win.replaceOpen
                    text: "All"
                    onClicked: {
                        if (searchField.text.length === 0) return;
                        for (var i = win.searchMatches.length - 1; i >= 0; --i) {
                            var start = win.searchMatches[i];
                            EditorMutations.replaceRange(editor, start,
                                                         start + searchField.text.length,
                                                         replaceField.text);
                        }
                        win.updateSearch();
                    }
                }

                Rectangle {
                    Layout.preferredWidth: 1
                    Layout.preferredHeight: 34
                    color: win.darkMode ? "#6f6f62" : "#d5d56e"
                }

                SearchIconButton {
                    iconName: "up"
                    iconColor: win.darkMode ? win.textColor : "#62635f"
                    onClicked: win.moveSearch(-1)
                }

                SearchIconButton {
                    iconName: "down"
                    iconColor: win.darkMode ? win.textColor : "#62635f"
                    onClicked: win.moveSearch(1)
                }

                SearchIconButton {
                    iconName: "close"
                    iconColor: win.darkMode ? win.textColor : "#62635f"
                    onClicked: win.closeSearch()
                }
            }
        }
    }

    Component.onCompleted: {
        var geometry = backend.windowGeometry();
        if (geometry.x >= 0) x = geometry.x;
        if (geometry.y >= 0) y = geometry.y;
        width = geometry.width;
        height = geometry.height;
        if (geometry.maximized) showMaximized();
        previewUpdate.restart();
        Qt.callLater(win.focusDocument);
    }

    Component.onDestruction: backend.saveWindowGeometry(x, y, width, height, visibility === Window.Maximized)

}
