#include "documentcontroller.h"

#include "filedocument.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

using namespace Mustermark;

DocumentController::DocumentController(QObject *parent) : QObject(parent) {
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
    return QString::fromUtf8(m_document.source);
}

QString DocumentController::renderedHtml() const {
    return m_document.renderedHtml;
}

QString DocumentController::title() const {
    return m_filePath.isEmpty() ? QStringLiteral("Untitled — Mustermark")
                                : QFileInfo(m_filePath).fileName() + QStringLiteral(" — Mustermark");
}

QVariantList DocumentController::nodes() const {
    QVariantList values;
    for (int index = 0; index < m_document.nodes.size(); ++index) {
        const Node &node = m_document.nodes.at(index);
        if (node.kind == NodeKind::List)
            continue;
        QVariantMap value;
        value.insert(QStringLiteral("identity"), node.id.isEmpty() ? node.ref : node.id);
        value.insert(QStringLiteral("id"), node.id);
        value.insert(QStringLiteral("ref"), node.ref);
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
            const Node &parent = m_document.nodes.at(node.parent);
            value.insert(QStringLiteral("parent"), parent.id.isEmpty() ? parent.ref : parent.id);
        } else {
            value.insert(QStringLiteral("parent"), QString());
        }
        values.append(value);
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
    const FileResult result = FileDocument::writeAtomic(m_filePath, m_document.source, m_diskRevision);
    if (!result.ok) {
        if (result.error == QStringLiteral("stale_revision"))
            setConflict(true);
        setStatus(result.error);
        writeRecovery();
        return false;
    }
    m_diskRevision = m_document.revision;
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
    const FileResult result = FileDocument::writeAtomic(path, m_document.source);
    if (!result.ok) {
        setStatus(result.error);
        return false;
    }
    if (!m_filePath.isEmpty())
        m_watcher.removePath(m_filePath);
    m_filePath = path;
    m_diskRevision = m_document.revision;
    m_modified = false;
    setConflict(false);
    clearRecovery();
    watchCurrentFile();
    setStatus(QStringLiteral("Saved %1").arg(QFileInfo(path).fileName()));
    emit filePathChanged();
    emit modifiedChanged();
    return true;
}

void DocumentController::updateSource(const QString &sourceText) {
    const QByteArray bytes = sourceText.toUtf8();
    if (bytes == m_document.source)
        return;
    setSource(bytes, true);
    writeRecovery();
}

bool DocumentController::enableTracking() {
    const EditResult result = m_engine.track(m_document.source);
    if (!result.ok) {
        setStatus(result.errorMessage);
        return false;
    }
    setSource(result.source, true);
    setStatus(QStringLiteral("Tracking enabled"));
    return true;
}

bool DocumentController::repairTracking() {
    const EditResult result = m_engine.repair(m_document.source);
    if (!result.ok) {
        setStatus(result.errorMessage);
        return false;
    }
    setSource(result.source, true);
    setStatus(QStringLiteral("Tracking metadata repaired"));
    return true;
}

bool DocumentController::disableTracking() {
    const EditResult result = m_engine.untrack(m_document.source);
    if (!result.ok) {
        setStatus(result.errorMessage);
        return false;
    }
    setSource(result.source, true);
    setStatus(QStringLiteral("Tracking metadata removed"));
    return true;
}

bool DocumentController::applyAction(const QString &action, const QString &node,
                                     const QString &target, const QString &label,
                                     const QString &text) {
    QJsonObject arguments;
    if (!target.isEmpty()) arguments.insert(QStringLiteral("target"), target);
    if (!label.isEmpty()) arguments.insert(QStringLiteral("label"), label);
    if (!text.isNull()) arguments.insert(QStringLiteral("text"), text);
    const EditResult result = m_engine.apply(m_document.source, m_document.revision,
                                             action, node, arguments);
    if (!result.ok) {
        setStatus(result.errorMessage);
        return false;
    }
    setSource(result.source, true);
    setStatus(QStringLiteral("%1 applied").arg(action));
    return true;
}

bool DocumentController::setHeadingLevel(const QString &node, int level) {
    const EditResult result = m_engine.apply(
        m_document.source, m_document.revision, QStringLiteral("set_heading_level"),
        node, {{QStringLiteral("level"), level}});
    if (!result.ok) {
        setStatus(result.errorMessage);
        return false;
    }
    setSource(result.source, true);
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
    m_document = m_engine.parse(bytes);
    emit sourceChanged();
    emit documentChanged();
    if (m_modified != isModified) {
        m_modified = isModified;
        emit modifiedChanged();
    }
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
        }
    }
    m_theme = values;
    emit themeChanged();
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
        file.write(m_document.source);
        file.commit();
    }
}

void DocumentController::clearRecovery() const {
    QFile::remove(recoveryPath());
}
