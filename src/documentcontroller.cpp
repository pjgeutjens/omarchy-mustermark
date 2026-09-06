#include "documentcontroller.h"

#include "filedocument.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

using namespace Mustermark;

DocumentController::DocumentController(QObject *parent) : QObject(parent) {
    loadRecentFiles();
    m_themePath = QDir::homePath() + QStringLiteral("/.local/state/omarchy/current/theme/colors.toml");
    loadTheme();
    if (QFileInfo::exists(m_themePath))
        m_watcher.addPath(m_themePath);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this,
            [this](const QString &path) {
                if (path == m_themePath) {
                    loadTheme();
                    if (QFileInfo::exists(path) && !m_watcher.files().contains(path))
                        m_watcher.addPath(path);
                } else if (path == m_filePath) {
                    checkExternalChange();
                    watchCurrentFile();
                }
            });
    newDocument();
}

QString DocumentController::source() const {
    return QString::fromUtf8(m_session.document().source);
}

QString DocumentController::renderedHtml() const {
    return m_session.document().renderedHtml;
}

QString DocumentController::title() const {
    return m_filePath.isEmpty() ? QStringLiteral("Untitled — Mustermark")
                                : QFileInfo(m_filePath).fileName() + QStringLiteral(" — Mustermark");
}

QVariantList DocumentController::nodes() const {
    QVariantList values;
    const Document &document = m_session.document();
    for (int index = 0; index < document.nodes.size(); ++index) {
        const Node &node = document.nodes.at(index);
        if (node.kind == NodeKind::List)
            continue;
        QVariantMap value;
        const QString identity = !node.sessionId.isEmpty()
                                     ? node.sessionId
                                     : (node.id.isEmpty() ? node.ref : node.id);
        value.insert(QStringLiteral("identity"), identity);
        value.insert(QStringLiteral("id"), node.sessionId);
        value.insert(QStringLiteral("ref"), identity);
        value.insert(QStringLiteral("fingerprint"), node.fingerprint);
        value.insert(QStringLiteral("kind"), DocumentEngine::kindName(node.kind));
        value.insert(QStringLiteral("text"), node.text);
        value.insert(QStringLiteral("labels"), node.labels);
        value.insert(QStringLiteral("level"), node.level);
        value.insert(QStringLiteral("depth"), node.depth);
        value.insert(QStringLiteral("startLine"), node.startLine);
        value.insert(QStringLiteral("endLine"), node.endLine);
        value.insert(QStringLiteral("task"), node.task);
        value.insert(QStringLiteral("checked"), node.checked);
        value.insert(QStringLiteral("ordered"), node.ordered);
        value.insert(QStringLiteral("hasChildren"), !node.children.isEmpty());
        if (node.parent >= 0) {
            const Node &parent = document.nodes.at(node.parent);
            value.insert(QStringLiteral("parent"), !parent.sessionId.isEmpty()
                                                       ? parent.sessionId
                                                       : (parent.id.isEmpty() ? parent.ref : parent.id));
        } else {
            value.insert(QStringLiteral("parent"), QString());
        }
        values.append(value);
    }
    return values;
}

QVariantList DocumentController::recentFiles() const {
    QVariantList values;
    for (const QString &path : m_recentFiles) {
        const QFileInfo file(path);
        values.append(QVariantMap{
            {QStringLiteral("name"), file.fileName()},
            {QStringLiteral("directory"), file.absolutePath()},
            {QStringLiteral("path"), path},
            {QStringLiteral("url"), QUrl::fromLocalFile(path)},
        });
    }
    return values;
}

void DocumentController::newDocument() {
    if (!m_filePath.isEmpty())
        m_watcher.removePath(m_filePath);
    m_filePath.clear();
    m_diskRevision.clear();
    setConflict(false);
    setSource({}, false);
    setStatus(QStringLiteral("New document"));
    emit filePathChanged();
}

bool DocumentController::loadFile(const QUrl &url) {
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    const FileResult result = FileDocument::read(path);
    if (!result.ok) {
        setStatus(result.error);
        return false;
    }
    if (!m_filePath.isEmpty())
        m_watcher.removePath(m_filePath);
    m_filePath = path;
    m_diskRevision = DocumentEngine::revisionFor(result.source);
    setConflict(false);
    setSource(result.source, false);
    watchCurrentFile();
    recordRecentFile(path);
    setStatus(QStringLiteral("Opened %1").arg(QFileInfo(path).fileName()));
    emit filePathChanged();
    return true;
}

bool DocumentController::save() {
    if (m_filePath.isEmpty()) {
        setStatus(QStringLiteral("Choose Save As for this document"));
        return false;
    }
    if (m_conflict) {
        setStatus(QStringLiteral("Autosave is paused because the file changed on disk"));
        return false;
    }
    const Document &document = m_session.document();
    const FileResult result = FileDocument::writeAtomic(m_filePath, document.source, m_diskRevision);
    if (!result.ok) {
        if (result.error == QStringLiteral("stale_revision"))
            setConflict(true);
        setStatus(result.error);
        writeRecovery();
        return false;
    }
    m_diskRevision = document.revision;
    if (m_modified) {
        m_modified = false;
        emit modifiedChanged();
    }
    clearRecovery();
    watchCurrentFile();
    setStatus(QStringLiteral("Saved"));
    return true;
}

bool DocumentController::saveAs(const QUrl &url) {
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    const Document &document = m_session.document();
    const FileResult result = FileDocument::writeAtomic(path, document.source);
    if (!result.ok) {
        setStatus(result.error);
        return false;
    }
    if (!m_filePath.isEmpty())
        m_watcher.removePath(m_filePath);
    m_filePath = path;
    m_diskRevision = document.revision;
    m_modified = false;
    setConflict(false);
    clearRecovery();
    watchCurrentFile();
    recordRecentFile(path);
    setStatus(QStringLiteral("Saved %1").arg(QFileInfo(path).fileName()));
    emit filePathChanged();
    emit modifiedChanged();
    return true;
}

void DocumentController::updateSource(const QString &sourceText) {
    const QByteArray bytes = sourceText.toUtf8();
    if (bytes == m_session.document().source)
        return;
    setSource(bytes, true);
    writeRecovery();
}

bool DocumentController::enableTracking() {
    const EditResult result = m_session.startTracking();
    if (!result.ok) {
        setStatus(result.errorMessage);
        return false;
    }
    emit documentChanged();
    setStatus(QStringLiteral("Session tracking started"));
    return true;
}

bool DocumentController::repairTracking() {
    return disableTracking();
}

bool DocumentController::disableTracking() {
    const EditResult result = m_session.stopTracking();
    if (!result.ok) {
        setStatus(result.errorMessage);
        return false;
    }
    emit documentChanged();
    setStatus(QStringLiteral("Session tracking stopped"));
    return true;
}

bool DocumentController::applyAction(const QString &action, const QString &node,
                                     const QString &target, const QString &label,
                                     const QString &text) {
    QJsonObject arguments;
    if (!target.isEmpty()) arguments.insert(QStringLiteral("target"), target);
    if (!label.isEmpty()) arguments.insert(QStringLiteral("label"), label);
    if (!text.isNull()) arguments.insert(QStringLiteral("text"), text);
    const QByteArray previous = m_session.document().source;
    const EditResult result = m_session.apply(m_session.document().revision, action, node, arguments);
    if (!result.ok) {
        setStatus(result.errorMessage);
        return false;
    }
    notifySessionChanged(previous != m_session.document().source);
    setStatus(QStringLiteral("%1 applied").arg(action));
    return true;
}

bool DocumentController::shiftLevel(const QString &action, const QString &node,
                                    bool includeDescendants) {
    const Document &document = m_session.document();
    const int nodeIndex = document.findNode(node);
    const bool item = nodeIndex >= 0 && document.nodes.at(nodeIndex).kind == NodeKind::Item;
    const bool effectiveDescendants = item || includeDescendants;
    const EditResult result = m_session.apply(
        document.revision, action, node,
        {{QStringLiteral("scope"), effectiveDescendants ? QStringLiteral("subtree")
                                                         : QStringLiteral("self")}});
    if (!result.ok) {
        setStatus(result.errorMessage);
        return false;
    }
    notifySessionChanged(true);
    setStatus(QStringLiteral("%1 applied to %2")
                  .arg(action, effectiveDescendants ? QStringLiteral("subtree")
                                                    : QStringLiteral("selection")));
    return true;
}

bool DocumentController::setHeadingLevel(const QString &node, int level) {
    const EditResult result = m_session.apply(
        m_session.document().revision, QStringLiteral("set_heading_level"),
        node, {{QStringLiteral("level"), level}});
    if (!result.ok) {
        setStatus(result.errorMessage);
        return false;
    }
    notifySessionChanged(true);
    setStatus(QStringLiteral("Heading changed to H%1").arg(level));
    return true;
}

void DocumentController::checkExternalChange() {
    if (m_filePath.isEmpty() || !QFileInfo::exists(m_filePath))
        return;
    const FileResult disk = FileDocument::read(m_filePath);
    if (!disk.ok)
        return;
    const QString revision = DocumentEngine::revisionFor(disk.source);
    if (revision == m_diskRevision)
        return;
    if (m_modified) {
        setConflict(true);
        setStatus(QStringLiteral("File changed on disk; autosave paused"));
        writeRecovery();
        return;
    }
    m_diskRevision = revision;
    setSource(disk.source, false);
    setStatus(QStringLiteral("Reloaded external changes"));
}

void DocumentController::setSource(const QByteArray &bytes, bool isModified) {
    m_session.setSource(bytes);
    emit sourceChanged();
    emit documentChanged();
    if (m_modified != isModified) {
        m_modified = isModified;
        emit modifiedChanged();
    }
}

void DocumentController::notifySessionChanged(bool didChangeSource) {
    emit sourceChanged();
    emit documentChanged();
    if (didChangeSource && !m_modified) {
        m_modified = true;
        emit modifiedChanged();
    }
    if (didChangeSource)
        writeRecovery();
}

QJsonObject DocumentController::apiState() const {
    QJsonObject result = m_session.document().toJson();
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("path"), m_filePath);
    result.insert(QStringLiteral("source"), source());
    result.insert(QStringLiteral("html"), m_session.document().renderedHtml);
    return result;
}

QJsonObject DocumentController::applyApiInstruction(const QJsonObject &instruction) {
    auto failure = [](const QString &code, const QString &message) {
        return QJsonObject{
            {QStringLiteral("ok"), false},
            {QStringLiteral("error"), QJsonObject{
                {QStringLiteral("code"), code},
                {QStringLiteral("message"), message},
            }},
        };
    };

    const QString expected = instruction.value(QStringLiteral("baseRevision")).toString();
    if (expected.isEmpty())
        return failure(QStringLiteral("base_revision_required"),
                       QStringLiteral("baseRevision is required."));
    if (expected != m_session.document().revision)
        return failure(QStringLiteral("stale_revision"),
                       QStringLiteral("The document changed after this instruction was prepared."));

    const QByteArray previousSource = m_session.document().source;
    const QString action = instruction.value(QStringLiteral("action")).toString();
    EditResult edit;
    if (action == QStringLiteral("track") || action == QStringLiteral("tracking_start"))
        edit = m_session.startTracking();
    else if (action == QStringLiteral("untrack") || action == QStringLiteral("tracking_stop"))
        edit = m_session.stopTracking();
    else
        edit = m_session.apply(expected, action,
                               instruction.value(QStringLiteral("node")).toString(), instruction);
    if (!edit.ok)
        return failure(edit.errorCode, edit.errorMessage);

    const bool didChangeSource = previousSource != m_session.document().source;
    if (didChangeSource) {
        if (m_filePath.isEmpty()) {
            m_session.setSource(previousSource);
            return failure(QStringLiteral("unsaved_document"),
                           QStringLiteral("Save the document before changing it through HTTP."));
        }
        const FileResult written = FileDocument::writeAtomic(
            m_filePath, m_session.document().source, m_diskRevision);
        if (!written.ok) {
            m_session.setSource(previousSource);
            return failure(written.error == QStringLiteral("stale_revision")
                               ? QStringLiteral("stale_revision")
                               : QStringLiteral("write_failed"), written.error);
        }
        m_diskRevision = m_session.document().revision;
        if (m_modified) {
            m_modified = false;
            emit modifiedChanged();
        }
        clearRecovery();
        watchCurrentFile();
    }
    emit sourceChanged();
    emit documentChanged();

    QJsonObject result = apiState();
    result.insert(QStringLiteral("edits"), edit.edits);
    return result;
}

void DocumentController::setStatus(const QString &value) {
    if (m_status == value)
        return;
    m_status = value;
    emit statusChanged();
}

void DocumentController::setConflict(bool value) {
    if (m_conflict == value)
        return;
    m_conflict = value;
    emit conflictChanged();
}

void DocumentController::watchCurrentFile() {
    if (!m_filePath.isEmpty() && QFileInfo::exists(m_filePath) &&
        !m_watcher.files().contains(m_filePath))
        m_watcher.addPath(m_filePath);
}

void DocumentController::loadTheme() {
    QVariantMap values{
        {QStringLiteral("background"), QStringLiteral("#161214")},
        {QStringLiteral("foreground"), QStringLiteral("#ded6cc")},
        {QStringLiteral("accent"), QStringLiteral("#b59caa")},
        {QStringLiteral("muted"), QStringLiteral("#766f70")},
        {QStringLiteral("selection"), QStringLiteral("#3a3034")},
        {QStringLiteral("selectionForeground"), QStringLiteral("#ded6cc")},
    };
    QFile file(m_themePath);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QRegularExpression assignment(
            QStringLiteral(R"rx(^\s*([a-z0-9_]+)\s*=\s*"(#[0-9a-fA-F]{6,8})")rx"));
        while (!file.atEnd()) {
            const auto match = assignment.match(QString::fromUtf8(file.readLine()));
            if (!match.hasMatch())
                continue;
            const QString name = match.captured(1);
            const QString color = match.captured(2);
            if (name == QStringLiteral("background") || name == QStringLiteral("foreground") ||
                name == QStringLiteral("accent"))
                values.insert(name, color);
            else if (name == QStringLiteral("color8"))
                values.insert(QStringLiteral("muted"), color);
            else if (name == QStringLiteral("selection_background"))
                values.insert(QStringLiteral("selection"), color);
            else if (name == QStringLiteral("selection_foreground"))
                values.insert(QStringLiteral("selectionForeground"), color);
        }
    }
    m_theme = values;
    emit themeChanged();
}

void DocumentController::loadRecentFiles() {
    QSettings settings;
    const QStringList stored = settings.value(QStringLiteral("recentFiles")).toStringList();
    for (const QString &path : stored) {
        const QFileInfo file(path);
        if (!file.isFile())
            continue;
        const QString resolved = file.canonicalFilePath();
        if (!resolved.isEmpty() && !m_recentFiles.contains(resolved))
            m_recentFiles.append(resolved);
        if (m_recentFiles.size() == 10)
            break;
    }
    if (m_recentFiles != stored)
        saveRecentFiles();
}

void DocumentController::recordRecentFile(const QString &path) {
    const QFileInfo file(path);
    QString resolved = file.canonicalFilePath();
    if (resolved.isEmpty())
        resolved = file.absoluteFilePath();
    m_recentFiles.removeAll(resolved);
    m_recentFiles.prepend(resolved);
    while (m_recentFiles.size() > 10)
        m_recentFiles.removeLast();
    saveRecentFiles();
    emit recentFilesChanged();
}

void DocumentController::saveRecentFiles() const {
    QSettings settings;
    settings.setValue(QStringLiteral("recentFiles"), m_recentFiles);
}

QString DocumentController::recoveryPath() const {
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
                         QStringLiteral("/recovery");
    const QByteArray identity = m_filePath.isEmpty() ? QByteArray("untitled") : m_filePath.toUtf8();
    const QString name = QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256)
                                                  .toHex().left(20));
    return root + QLatin1Char('/') + name + QStringLiteral(".md");
}

void DocumentController::writeRecovery() const {
    const QString path = recoveryPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(m_session.document().source);
        file.commit();
    }
}

void DocumentController::clearRecovery() const {
    QFile::remove(recoveryPath());
}
