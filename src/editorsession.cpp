#include "editorsession.h"
#include "backend.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QQuickWindow>
#include <QSaveFile>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QUuid>
#include <cmath>
#include <climits>
#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace {
QString rawLinkTarget(const QString &path) {
#ifdef Q_OS_UNIX
    const QByteArray name = QFile::encodeName(path);
    QByteArray target(256, Qt::Uninitialized);
    while (target.size() <= 65536) {
        const ssize_t size = ::readlink(name.constData(), target.data(), size_t(target.size()));
        if (size < 0) return {};
        if (size < target.size()) {
            target.resize(size);
            return QFile::decodeName(target);
        }
        target.resize(target.size() * 2);
    }
    return {};
#else
    return QFileInfo(path).symLinkTarget();
#endif
}
QString identity(const QString &path) {
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    if (!canonical.isEmpty()) return canonical;
    // canonicalFilePath() is empty after deletion. Resolve every component,
    // including dangling links, so aliases cannot recreate a conflicted file.
    // Preserve '..' until each preceding symbolic link has been followed:
    // lexical path cleaning would change which file the operating system sees.
    const QString absolute = QDir::isAbsolutePath(path) ? path : QDir::currentPath() + QLatin1Char('/') + path;
    QStringList components = absolute.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString resolved = QStringLiteral("/");
    int links = 0;
    while (!components.isEmpty()) {
        const QString part = components.takeFirst();
        if (part == QStringLiteral(".")) continue;
        if (part == QStringLiteral("..")) { resolved = QFileInfo(resolved).absolutePath(); continue; }
        const QString candidate = QDir(resolved).filePath(part);
        const QFileInfo component(candidate);
        if (component.isSymLink()) {
            const QString target = rawLinkTarget(candidate);
            if (++links > 256 || target.isEmpty()) return {};
            components = target.split(QLatin1Char('/'), Qt::SkipEmptyParts) + components;
            if (QDir::isAbsolutePath(target)) resolved = QStringLiteral("/");
        } else resolved = candidate;
    }
    return QDir::cleanPath(resolved);
}
bool integer(const QJsonValue &value, int *out) {
    if (!value.isDouble()) return false;
    const double n = value.toDouble();
    if (!std::isfinite(n) || n < 0 || n > INT_MAX || std::floor(n) != n) return false;
    *out = int(n);
    return true;
}
}

EditorSession::EditorSession(Backend *backend, QObject *parent)
    : QObject(parent), m_backend(backend), m_id(QUuid::createUuid().toString(QUuid::WithoutBraces)) {
    connect(backend, &Backend::externalChangeDetected, this, [this](bool deleted, bool) {
        m_externalChanged = true;
        m_externalDeleted = deleted;
        advance();
    });
    connect(backend, &Backend::documentLoaded, this, &EditorSession::advance);
    connect(backend, &Backend::documentTextChanged, this, &EditorSession::advance);
}

void EditorSession::attachWindow(QQuickWindow *window) {
    m_window = window;
    m_editor = window ? window->findChild<QObject *>(QStringLiteral("sourceEditor")) : nullptr;
    m_seenText = m_backend->currentDocumentText();
    m_seenFile = currentFile();
    if (!currentFile().isEmpty() && !diskMatches())
        emit m_backend->externalChangeDetected(!QFileInfo::exists(currentFile()), m_backend->modified());
}
QString EditorSession::currentFile() const { return m_backend->fileUrl().toLocalFile(); }
QString EditorSession::version() const { return m_id + QLatin1Char(':') + QString::number(m_generation); }
void EditorSession::advance() { ++m_generation; }
void EditorSession::refresh() {
    const QString text = m_backend->currentDocumentText();
    const QString file = currentFile();
    if (text != m_seenText || file != m_seenFile) {
        m_seenText = text;
        m_seenFile = file;
        advance();
    }
}
QString EditorSession::activeDialog() const {
    for (const QString &name : {QStringLiteral("external"), QStringLiteral("print"), QStringLiteral("save"),
                                QStringLiteral("open"), QStringLiteral("unsaved")})
        if (m_dialogs.contains(name)) return name;
    return {};
}
QJsonObject EditorSession::state(bool includeText) {
    refresh();
    QJsonObject result{
        {QStringLiteral("id"), m_id}, {QStringLiteral("pid"), double(QCoreApplication::applicationPid())},
        {QStringLiteral("version"), version()}, {QStringLiteral("file"), currentFile()},
        {QStringLiteral("modified"), m_backend->modified()},
        {QStringLiteral("cursor"), m_editor ? m_editor->property("cursorPosition").toInt() : 0},
        {QStringLiteral("selectionStart"), m_editor ? m_editor->property("selectionStart").toInt() : 0},
        {QStringLiteral("selectionEnd"), m_editor ? m_editor->property("selectionEnd").toInt() : 0},
        {QStringLiteral("canUndo"), m_backend->m_document && m_backend->m_document->isUndoAvailable()},
        {QStringLiteral("canRedo"), m_backend->m_document && m_backend->m_document->isRedoAvailable()},
        {QStringLiteral("externalChanged"), m_externalChanged}, {QStringLiteral("externalDeleted"), m_externalDeleted},
        {QStringLiteral("pendingAction"), m_pendingAction}, {QStringLiteral("activeDialog"), activeDialog()},
        {QStringLiteral("wordCount"), Backend::countWords(m_backend->currentDocumentText())},
        {QStringLiteral("darkMode"), m_backend->darkMode()}, {QStringLiteral("textScale"), m_backend->textScale()}
    };
    result.insert(QStringLiteral("viewMode"), m_backend->viewMode());
    result.insert(QStringLiteral("splitRatio"), m_backend->splitRatio());
    if (includeText) result.insert(QStringLiteral("text"), m_backend->currentDocumentText());
    if (m_window) {
        result.insert(QStringLiteral("window"), QJsonObject{{QStringLiteral("width"), m_window->width()},
            {QStringLiteral("height"), m_window->height()}, {QStringLiteral("visible"), m_window->isVisible()},
            {QStringLiteral("visibility"), int(m_window->visibility())},
            {QStringLiteral("dpr"), m_window->devicePixelRatio()}});
        result.insert(QStringLiteral("searchOpen"), m_window->property("searchOpen").toBool());
        result.insert(QStringLiteral("replaceOpen"), m_window->property("replaceOpen").toBool());
    }
    return result;
}
QJsonObject EditorSession::success() {
    return {{QStringLiteral("protocol"), 1}, {QStringLiteral("ok"), true}, {QStringLiteral("result"), state(false)}};
}
QJsonObject EditorSession::failure(const QString &code, const QString &message) {
    return {{QStringLiteral("protocol"), 1}, {QStringLiteral("ok"), false},
        {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), code}, {QStringLiteral("message"), message}}},
        {QStringLiteral("state"), state(false)}};
}
bool EditorSession::diskMatches() const {
    if (currentFile().isEmpty()) return true;
    QFile file(currentFile());
    if (!QFileInfo::exists(currentFile())) return !m_backend->m_hasKnownFileContents;
    return m_backend->m_hasKnownFileContents && file.open(QIODevice::ReadOnly)
        && file.readAll() == m_backend->m_lastKnownFileContents;
}
QJsonObject EditorSession::saveFile(const QString &path, bool overwrite) {
    if (path.isEmpty()) return failure(QStringLiteral("save_path_required"), QStringLiteral("An unnamed document needs an explicit save path."));
    if (!QDir::isAbsolutePath(path)) return failure(QStringLiteral("invalid_request"), QStringLiteral("Paths must be absolute."));
    const QString destination = identity(path);
    if (destination.isEmpty()) return failure(QStringLiteral("invalid_request"), QStringLiteral("Could not resolve the save path."));
    const bool sameFile = !currentFile().isEmpty() && destination == identity(currentFile());
    if (sameFile && (m_externalChanged || !diskMatches())) {
        if (!m_externalChanged)
            emit m_backend->externalChangeDetected(!QFileInfo::exists(currentFile()), m_backend->modified());
        return failure(QStringLiteral("disk_conflict"), QStringLiteral("Resolve the external change before saving."));
    }
    if (!sameFile && QFileInfo::exists(path) && !overwrite)
        return failure(QStringLiteral("target_exists"), QStringLiteral("Overwriting another existing file requires overwrite=true."));
    if (!m_backend->saveTo(QUrl::fromLocalFile(path), destination))
        return failure(QStringLiteral("io_error"), m_backend->status());
    m_externalChanged = false;
    m_externalDeleted = false;
    advance();
    return success();
}
QJsonObject EditorSession::transition(const QString &action, const QString &path, const QJsonObject &args) {
    QByteArray prepared;
    if (action != QStringLiteral("close")) {
        if (path.isEmpty() || !QDir::isAbsolutePath(path))
            return failure(QStringLiteral("invalid_request"), QStringLiteral("A file operation requires an absolute path."));
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || (prepared = file.readAll(), file.error() != QFile::NoError))
            return failure(QStringLiteral("io_error"), QStringLiteral("Could not read the replacement file."));
    }
    if (m_backend->modified()) {
        const QString policy = args.value(QStringLiteral("onUnsaved")).toString();
        if (policy != QStringLiteral("save") && policy != QStringLiteral("discard"))
            return failure(QStringLiteral("unsaved_changes"), QStringLiteral("Choose onUnsaved=save or discard explicitly."));
        if (policy == QStringLiteral("save")) {
            const QString savePath = args.value(QStringLiteral("savePath")).toString(currentFile());
            const QJsonObject saved = saveFile(savePath, args.value(QStringLiteral("overwrite")).toBool());
            if (!saved.value(QStringLiteral("ok")).toBool()) return saved;
            if (action != QStringLiteral("close")) {
                QFile replacement(path);
                if (!replacement.open(QIODevice::ReadOnly)
                    || (prepared = replacement.readAll(), replacement.error() != QFile::NoError))
                    return failure(QStringLiteral("io_error"), QStringLiteral("Saved the draft, but could not read the replacement file."));
            }
        }
    }
    if (action == QStringLiteral("close")) {
        m_backend->discardRecovery();
        m_closeReady = true;
        return success();
    }
    // The replacement was read before clearing any draft. Commit this exact
    // snapshot, rather than re-reading a potentially vanished target.
    m_backend->loadDocumentText(QString::fromUtf8(prepared));
    m_backend->m_lastKnownFileContents = prepared;
    m_backend->m_hasKnownFileContents = true;
    m_backend->setFileUrl(QUrl::fromLocalFile(path));
    m_backend->watchCurrentFile();
    m_backend->setModified(false);
    m_backend->clearRecovery();
    m_backend->setStatus(QStringLiteral("Opened %1").arg(m_backend->fileName()));
    m_externalChanged = false;
    m_externalDeleted = false;
    advance();
    return success();
}
bool EditorSession::rangeValid(int start, int end) const {
    const QString text = m_backend->currentDocumentText();
    const auto boundary = [&text](int p) {
        return p == 0 || p == text.size() || !(text.at(p - 1).isHighSurrogate() && text.at(p).isLowSurrogate());
    };
    return start >= 0 && start <= end && end <= text.size() && boundary(start) && boundary(end);
}
QJsonObject EditorSession::execute(const QJsonObject &request) {
    refresh();
    if (request.value(QStringLiteral("protocol")) != QJsonValue(1)
        || !request.value(QStringLiteral("command")).isString()
        || !request.value(QStringLiteral("args")).isObject())
        return failure(QStringLiteral("invalid_request"), QStringLiteral("Expected protocol=1, command and args object."));
    const QString command = request.value(QStringLiteral("command")).toString();
    const QJsonObject args = request.value(QStringLiteral("args")).toObject();
    if (command == QStringLiteral("state"))
        return {{QStringLiteral("protocol"), 1}, {QStringLiteral("ok"), true},
            {QStringLiteral("result"), state(args.value(QStringLiteral("includeText")).toBool(true))}};
    const QStringList mutations{QStringLiteral("select"), QStringLiteral("replace_range"), QStringLiteral("undo"),
        QStringLiteral("redo"), QStringLiteral("open"), QStringLiteral("save"), QStringLiteral("save_as"),
        QStringLiteral("reload"), QStringLiteral("resolve_external"), QStringLiteral("close")};
    if (command != QStringLiteral("screenshot") && !mutations.contains(command))
        return failure(QStringLiteral("unknown_command"), QStringLiteral("Unknown control command."));
    if ((command != QStringLiteral("screenshot") || args.contains(QStringLiteral("expectedVersion")))
        && args.value(QStringLiteral("expectedVersion")).toString() != version())
        return failure(QStringLiteral("version_conflict"), QStringLiteral("Read the current document version before modifying it."));
    const bool resolvingExternal = command == QStringLiteral("resolve_external")
        && (m_externalChanged || m_dialogs.contains(QStringLiteral("external")));
    if (command != QStringLiteral("screenshot") && (m_capturing || m_closeReady
        || ((!m_dialogs.isEmpty() || !m_pendingAction.isEmpty()) && !resolvingExternal)))
        return failure(QStringLiteral("busy"), QStringLiteral("A document decision is pending."));
    if (command == QStringLiteral("select") || command == QStringLiteral("replace_range")) {
        int start, end;
        if (!m_editor || !m_backend->m_document) return failure(QStringLiteral("busy"), QStringLiteral("Editor is not ready."));
        if (!integer(args.value(QStringLiteral("start")), &start) || !integer(args.value(QStringLiteral("end")), &end)
            || !rangeValid(start, end))
            return failure(QStringLiteral("invalid_range"), QStringLiteral("Expected valid UTF-16 boundaries with end exclusive."));
        if (command == QStringLiteral("select")) {
            QMetaObject::invokeMethod(m_editor, "select", Q_ARG(int, start), Q_ARG(int, end));
        } else {
            if (!args.value(QStringLiteral("text")).isString())
                return failure(QStringLiteral("invalid_request"), QStringLiteral("Replacement text must be a string."));
            QString text = args.value(QStringLiteral("text")).toString();
            text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
            text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
            QTextCursor cursor(m_backend->m_document);
            cursor.beginEditBlock();
            cursor.setPosition(start);
            cursor.setPosition(end, QTextCursor::KeepAnchor);
            cursor.insertText(text);
            cursor.endEditBlock();
            m_editor->setProperty("cursorPosition", start + text.size());
            m_backend->editorTextChanged();
            refresh();
        }
        return success();
    }
    if (command == QStringLiteral("undo") || command == QStringLiteral("redo")) {
        if (!m_editor) return failure(QStringLiteral("busy"), QStringLiteral("Editor is not ready."));
        QMetaObject::invokeMethod(m_editor, command.toUtf8().constData());
        m_backend->editorTextChanged();
        refresh();
        return success();
    }
    if (command == QStringLiteral("save") || command == QStringLiteral("save_as"))
        return saveFile(command == QStringLiteral("save") ? currentFile() : args.value(QStringLiteral("path")).toString(),
                        args.value(QStringLiteral("overwrite")).toBool());
    if (command == QStringLiteral("open") || command == QStringLiteral("reload") || command == QStringLiteral("close"))
        return transition(command, command == QStringLiteral("reload") ? currentFile() : args.value(QStringLiteral("path")).toString(), args);
    if (command == QStringLiteral("resolve_external")) {
        const QString choice = args.value(QStringLiteral("choice")).toString();
        if (choice != QStringLiteral("keep") && choice != QStringLiteral("reload"))
            return failure(QStringLiteral("invalid_request"), QStringLiteral("Expected choice=keep or reload."));
        if (!m_externalChanged && diskMatches())
            return failure(QStringLiteral("invalid_request"), QStringLiteral("There is no external change to resolve."));
        QJsonObject result;
        if (choice == QStringLiteral("reload")) {
            result = transition(QStringLiteral("reload"), currentFile(), args);
            if (!result.value(QStringLiteral("ok")).toBool()) return result;
        } else {
            m_backend->keepExternalVersion();
            m_externalChanged = false;
            m_externalDeleted = false;
            advance();
        }
        m_dialogs.remove(QStringLiteral("external"));
        emit externalResolved();
        return success();
    }
    // A nested rendering wait may admit user edits: keep control mutations
    // blocked and recheck the captured version before writing any image.
    if (!m_window || !m_window->isVisible() || m_window->visibility() == QWindow::Minimized)
        return failure(QStringLiteral("capture_unavailable"), QStringLiteral("The window is not renderable."));
    const QString path = args.value(QStringLiteral("path")).toString();
    if (!QDir::isAbsolutePath(path)) return failure(QStringLiteral("invalid_request"), QStringLiteral("Screenshot path must be absolute."));
    if (QFileInfo::exists(path) && !args.value(QStringLiteral("overwrite")).toBool())
        return failure(QStringLiteral("target_exists"), QStringLiteral("Screenshot already exists."));
    const QString before = version();
    m_capturing = true;
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(m_window, &QQuickWindow::afterAnimating, &loop, &QEventLoop::quit);
    m_window->update();
    deadline.start(1500);
    loop.exec();
    // Flush the word-count debounce before taking the visible snapshot.
    m_backend->m_wordCountTimer.stop();
    m_backend->refreshWordCount();
    const QImage image = m_window->grabWindow();
    m_capturing = false;
    refresh();
    if (before != version()) return failure(QStringLiteral("version_conflict"), QStringLiteral("Document changed while rendering."));
    if (image.isNull()) return failure(QStringLiteral("capture_unavailable"), QStringLiteral("Qt could not capture the window."));
    if (args.value(QStringLiteral("overwrite")).toBool()) {
        QSaveFile output(path);
        if (!output.open(QIODevice::WriteOnly) || !image.save(&output, "PNG") || !output.commit())
            return failure(QStringLiteral("io_error"), QStringLiteral("Could not save screenshot."));
    } else {
        QFile output(path);
        if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            return failure(QFileInfo::exists(path) ? QStringLiteral("target_exists") : QStringLiteral("io_error"),
                           QStringLiteral("Could not create a new screenshot file."));
        if (!image.save(&output, "PNG") || !output.flush()) {
            output.close();
            output.remove();
            return failure(QStringLiteral("io_error"), QStringLiteral("Could not save screenshot."));
        }
    }
    QJsonObject result = state(false);
    result.insert(QStringLiteral("path"), path);
    result.insert(QStringLiteral("width"), image.width());
    result.insert(QStringLiteral("height"), image.height());
    result.insert(QStringLiteral("dpr"), m_window->devicePixelRatio());
    return {{QStringLiteral("protocol"), 1}, {QStringLiteral("ok"), true}, {QStringLiteral("result"), result}};
}

void EditorSession::completeClose() { if (m_closeReady) emit closeRequested(); }
void EditorSession::setDialog(const QString &name, bool visible) {
    if (visible) m_dialogs.insert(name);
    else m_dialogs.remove(name);
}
void EditorSession::finishUi(const QJsonObject &result) {
    if (!result.value(QStringLiteral("ok")).toBool()) {
        m_backend->setStatus(result.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString());
        cancelPending();
    }
}
void EditorSession::requestOpen(const QUrl &url) {
    const QString path = url.toLocalFile();
    if (m_backend->modified()) {
        m_pendingAction = QStringLiteral("open");
        m_pendingPath = path;
        emit pendingChanged();
        emit unsavedDecisionRequested();
    } else finishUi(transition(QStringLiteral("open"), path, {}));
}
bool EditorSession::requestClose() {
    if (m_closeReady) return true;
    if (!m_backend->modified()) { m_backend->discardRecovery(); return true; }
    m_pendingAction = QStringLiteral("close");
    emit pendingChanged();
    emit unsavedDecisionRequested();
    return false;
}
void EditorSession::requestSave() {
    if (currentFile().isEmpty()) {
        m_backend->saveAsDialog();
        return;
    }
    const QJsonObject saved = saveFile(currentFile(), false);
    if (!saved.value(QStringLiteral("ok")).toBool()) { finishUi(saved); return; }
    if (m_uiSavingPending) {
        const QString action = m_pendingAction, path = m_pendingPath;
        cancelPending();
        const QJsonObject result = transition(action, path, {});
        finishUi(result);
        if (action == QStringLiteral("close") && result.value(QStringLiteral("ok")).toBool()) completeClose();
    }
}
void EditorSession::requestSaveAs() { m_backend->saveAsDialog(); }
void EditorSession::acceptSave(const QUrl &url) {
    const QJsonObject saved = saveFile(url.toLocalFile(), true);
    if (!saved.value(QStringLiteral("ok")).toBool()) { finishUi(saved); return; }
    if (m_uiSavingPending) {
        const QString action = m_pendingAction, path = m_pendingPath;
        cancelPending();
        const QJsonObject result = transition(action, path, {});
        finishUi(result);
        if (action == QStringLiteral("close") && result.value(QStringLiteral("ok")).toBool()) completeClose();
    }
}
void EditorSession::cancelPending() {
    m_pendingAction.clear(); m_pendingPath.clear(); m_uiSavingPending = false;
    emit pendingChanged();
}
void EditorSession::resolvePending(const QString &choice) {
    if (choice == QStringLiteral("cancel")) { cancelPending(); return; }
    if (choice == QStringLiteral("save")) { m_uiSavingPending = true; requestSave(); return; }
    if (choice != QStringLiteral("discard") || m_pendingAction.isEmpty()) return;
    const QString action = m_pendingAction, path = m_pendingPath;
    cancelPending();
    const QJsonObject result = transition(action, path, {{QStringLiteral("onUnsaved"), QStringLiteral("discard")}});
    finishUi(result);
    if (action == QStringLiteral("close") && result.value(QStringLiteral("ok")).toBool()) completeClose();
}
void EditorSession::resolveExternal(const QString &choice) {
    const QJsonObject result = execute({{QStringLiteral("protocol"), 1}, {QStringLiteral("command"), QStringLiteral("resolve_external")},
        {QStringLiteral("args"), QJsonObject{{QStringLiteral("expectedVersion"), state(false).value(QStringLiteral("version"))},
                                           {QStringLiteral("choice"), choice},
                                           {QStringLiteral("onUnsaved"), QStringLiteral("discard")}}}});
    if (!result.value(QStringLiteral("ok")).toBool())
        m_backend->setStatus(result.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString());
}
void EditorSession::printDocument() {
    setDialog(QStringLiteral("print"), true);
    m_backend->printDocument();
    setDialog(QStringLiteral("print"), false);
}
