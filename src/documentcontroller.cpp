#include "documentcontroller.h"

#include "filedocument.h"
#include "previewipc.h"

#include <QCryptographicHash>
#include <QBuffer>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QImageReader>
#include <QCoreApplication>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QUuid>

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
    const QString &html = m_session.document().renderedHtml;
    QSet<QString> attachmentPaths;
    for (const Mustermark::Node &node : m_session.document().nodes)
        for (const Mustermark::Attachment &attachment : node.attachments)
            attachmentPaths.insert(attachment.path);
    static const QRegularExpression imageTag(
        QStringLiteral("<img\\b[^>]*\\bsrc=\"([^\"]+)\"[^>]*>"),
        QRegularExpression::CaseInsensitiveOption);
    QString linked;
    qsizetype cursor = 0;
    auto matches = imageTag.globalMatch(html);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        linked += html.mid(cursor, match.capturedStart() - cursor);
        const QString tag = match.captured();
        const QString path = match.captured(1);
        if (attachmentPaths.contains(path)) {
            linked += QStringLiteral(
                "<span class=\"mm-attachment\" tabindex=\"0\" role=\"button\" aria-label=\"Open attached image\">"
                "<span class=\"mm-attachment-icon\">&#9635;</span>"
                "<span class=\"mm-attachment-preview\">%1</span>"
                "</span>").arg(tag);
        } else {
            linked += tag;
        }
        cursor = match.capturedEnd();
    }
    linked += html.mid(cursor);
    return linked;
}

QString DocumentController::previewPage() const {
    const QString background = m_theme.value(QStringLiteral("background")).toString();
    const QString foreground = m_theme.value(QStringLiteral("foreground")).toString();
    const QString muted = m_theme.value(QStringLiteral("muted")).toString();
    const QString accent = m_theme.value(QStringLiteral("accent")).toString();
    QString customStyle;
    const QString stylePath = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
                              QStringLiteral("/mustermark/preview.css");
    QFile style(stylePath);
    if (style.open(QIODevice::ReadOnly) && style.size() <= 256 * 1024)
        customStyle = QString::fromUtf8(style.readAll());

    return QStringLiteral(R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
:root{color-scheme:dark;--bg:%1;--fg:%2;--muted:%3;--accent:%4}
*{box-sizing:border-box}
body{max-width:860px;margin:0 auto;padding:16px 22px 34px;background:var(--bg);color:var(--fg);font:16px/1.35 sans-serif}
h1,h2,h3,h4,h5,h6{line-height:1.15;margin:1em 0 .35em}
h1:first-child,h2:first-child,h3:first-child,h4:first-child,h5:first-child,h6:first-child{margin-top:0}
p{margin:.5em 0}
ul,ol{margin:.45em 0;padding-inline-start:1.45em}
li{margin:.08em 0}
a{color:var(--accent)}
img{max-width:100%%;height:auto}
.mm-attachment{position:relative;display:inline-block;margin:0 0 0 -2px;color:var(--accent);cursor:pointer;vertical-align:middle}
.mm-attachment-icon{display:inline-grid;place-items:center;width:1.2rem;height:1.2rem;border:1px solid color-mix(in srgb,var(--accent) 65%%,transparent);border-radius:3px;font:700 13px/1 monospace}
.mm-attachment-preview{display:none;position:fixed;z-index:1000;right:28px;bottom:28px;padding:8px;background:var(--bg);border:1px solid var(--accent);box-shadow:0 12px 36px #0009;pointer-events:none}
.mm-attachment.mm-open .mm-attachment-preview{display:block}
.mm-attachment-preview img{display:block;max-width:min(70vw,720px);max-height:75vh;width:auto;height:auto}
%5
</style></head><body>%6
<script>
function toggleAttachment(attachment){
  const shouldOpen=!attachment.classList.contains('mm-open');
  document.querySelectorAll('.mm-attachment.mm-open').forEach(item=>item.classList.remove('mm-open'));
  if(shouldOpen)attachment.classList.add('mm-open');
}
document.addEventListener('click',event=>{
  const attachment=event.target.closest('.mm-attachment');
  if(attachment)toggleAttachment(attachment);
});
document.addEventListener('keydown',event=>{
  if(event.key==='Escape'){
    document.querySelectorAll('.mm-attachment.mm-open').forEach(item=>item.classList.remove('mm-open'));
    return;
  }
  const attachment=event.target.closest('.mm-attachment');
  if(attachment&&(event.key==='Enter'||event.key===' ')){
    event.preventDefault();
    toggleAttachment(attachment);
  }
});
</script></body></html>)HTML")
        .arg(background, foreground, muted, accent, customStyle, renderedHtml());
}

QUrl DocumentController::documentBaseUrl() const {
    if (m_filePath.isEmpty())
        return {};
    return QUrl::fromLocalFile(QFileInfo(m_filePath).absolutePath() + QDir::separator());
}

QString DocumentController::title() const {
    return m_filePath.isEmpty() ? QStringLiteral("Untitled — Mustermark")
                                : QFileInfo(m_filePath).fileName() + QStringLiteral(" — Mustermark");
}

QString DocumentController::displayPath() const {
    if (m_filePath.isEmpty())
        return QStringLiteral("Untitled");
    const QString home = QDir::homePath();
    if (m_filePath == home)
        return QStringLiteral("~");
    if (m_filePath.startsWith(home + QDir::separator()))
        return QStringLiteral("~") + m_filePath.mid(home.size());
    return m_filePath;
}

QVariantList DocumentController::nodes() const {
    QVariantList values;
    const Document &document = m_session.document();
    for (int index = 0; index < document.nodes.size(); ++index) {
        const Node &node = document.nodes.at(index);
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
        QVariantList attachments;
        for (const Attachment &attachment : node.attachments) {
            const QString url = attachmentUrl(attachment.path);
            attachments.append(QVariantMap{
                {QStringLiteral("path"), attachment.path},
                {QStringLiteral("alt"), attachment.alt},
                {QStringLiteral("url"), url},
                {QStringLiteral("available"), !url.isEmpty()},
            });
        }
        value.insert(QStringLiteral("attachments"), attachments);
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
    m_sourceEditActive = false;
    if (!m_apiUndo.isEmpty()) {
        m_apiUndo.clear();
        emit undoAvailableChanged();
    }
    m_filePath.clear();
    m_diskRevision.clear();
    setConflict(false);
    setSource({}, false);
    setRecoveryAvailable(QFileInfo::exists(recoveryPath()));
    setStatus(m_recoveryAvailable ? QStringLiteral("Unsaved recovery draft available")
                                  : QStringLiteral("New document"));
    emit filePathChanged();
}

bool DocumentController::loadFile(const QUrl &url) {
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    const FileResult result = FileDocument::read(path);
    if (!result.ok) {
        setStatus(result.error);
        return false;
    }
    m_sourceEditActive = false;
    if (!m_apiUndo.isEmpty()) {
        m_apiUndo.clear();
        emit undoAvailableChanged();
    }
    if (!m_filePath.isEmpty())
        m_watcher.removePath(m_filePath);
    m_filePath = path;
    m_diskRevision = DocumentEngine::revisionFor(result.source);
    setConflict(false);
    setSource(result.source, false);
    m_session.setHeadingIdentity({}, 0, {});
    if (QFileInfo::exists(QFileInfo(path).canonicalFilePath() + QStringLiteral(".mustermark.json"))) {
        const auto linked = FileDocument::readLinked(path, m_session);
        if (!linked.ok) { setStatus(linked.error); setConflict(true); return false; }
    }
    setRecoveryAvailable(QFileInfo::exists(recoveryPath()));
    watchCurrentFile();
    recordRecentFile(path);
    setStatus(m_recoveryAvailable
                  ? QStringLiteral("Recovery draft available for %1").arg(QFileInfo(path).fileName())
                  : QStringLiteral("Opened %1").arg(QFileInfo(path).fileName()));
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
    const FileResult result = FileDocument::writeAtomic(m_filePath, document.source, m_diskRevision, &m_session);
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
    m_session.setHeadingIdentity({}, 0, {});
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

void DocumentController::beginSourceEdit() {
    if (m_sourceEditActive)
        return;
    m_sourceEditActive = true;
    m_sourceEditStart = m_session.document().source;
    m_sourceEditStartModified = m_modified;
}

void DocumentController::endSourceEdit() {
    if (!m_sourceEditActive)
        return;
    m_sourceEditActive = false;
    if (m_sourceEditStart == m_session.document().source)
        return;

    const QString startingRevision = DocumentEngine::revisionFor(m_sourceEditStart);
    if (!m_apiUndo.isEmpty() && m_apiUndo.constLast().resultingRevision != startingRevision)
        m_apiUndo.clear();
    m_apiUndo.append({m_sourceEditStart, m_session.document().revision,
                      m_sourceEditStartModified, {}, {}});
    constexpr qsizetype maxUndoEntries = 100;
    while (m_apiUndo.size() > maxUndoEntries)
        m_apiUndo.removeFirst();
    emit undoAvailableChanged();
}

void DocumentController::updateSource(const QString &sourceText) {
    const QByteArray bytes = sourceText.toUtf8();
    if (bytes == m_session.document().source)
        return;
    if (!m_sourceEditActive && !m_apiUndo.isEmpty()) {
        m_apiUndo.clear();
        emit undoAvailableChanged();
    }
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
    const bool changed = previous != m_session.document().source;
    if (changed)
        pushApiUndo(previous);
    notifySessionChanged(changed);
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
    pushApiUndo(document.source);
    notifySessionChanged(true);
    setStatus(QStringLiteral("%1 applied to %2")
                  .arg(action, effectiveDescendants ? QStringLiteral("subtree")
                                                    : QStringLiteral("selection")));
    return true;
}

bool DocumentController::setHeadingLevel(const QString &node, int level) {
    const QByteArray previous = m_session.document().source;
    const EditResult result = m_session.apply(
        m_session.document().revision, QStringLiteral("set_heading_level"),
        node, {{QStringLiteral("level"), level}});
    if (!result.ok) {
        setStatus(result.errorMessage);
        return false;
    }
    pushApiUndo(previous);
    notifySessionChanged(true);
    setStatus(QStringLiteral("Heading changed to H%1").arg(level));
    return true;
}

QString DocumentController::attachmentRoot() const {
    if (m_filePath.isEmpty())
        return {};
    QString stem = QFileInfo(m_filePath).completeBaseName().toLower();
    stem.replace(QRegularExpression(QStringLiteral("[^a-z0-9._-]+")), QStringLiteral("-"));
    stem = stem.trimmed();
    while (stem.startsWith(QLatin1Char('-')) || stem.startsWith(QLatin1Char('.')))
        stem.remove(0, 1);
    while (stem.endsWith(QLatin1Char('-')) || stem.endsWith(QLatin1Char('.')))
        stem.chop(1);
    if (stem.isEmpty())
        stem = QStringLiteral("document");
    return QFileInfo(m_filePath).dir().absoluteFilePath(stem + QStringLiteral(".assets"));
}

QString DocumentController::attachmentUrl(const QString &relativePath) const {
    if (m_filePath.isEmpty() || relativePath.isEmpty() || !QDir::isRelativePath(relativePath))
        return {};
    const QDir documentDirectory = QFileInfo(m_filePath).dir();
    const QString clean = QDir::cleanPath(relativePath);
    if (clean == QStringLiteral("..") || clean.startsWith(QStringLiteral("../")))
        return {};
    const QString absolute = documentDirectory.absoluteFilePath(clean);
    const QFileInfo file(absolute);
    return file.isFile() ? QUrl::fromLocalFile(file.absoluteFilePath()).toString() : QString();
}

bool DocumentController::attachImageData(const QString &node, const QByteArray &data,
                                         const QString &extension, const QString &alt) {
    if (m_filePath.isEmpty()) {
        setStatus(QStringLiteral("Save the document before attaching an image"));
        return false;
    }
    if (data.isEmpty() || data.size() > 8 * 1024 * 1024) {
        setStatus(QStringLiteral("Images must be between 1 byte and 8 MiB"));
        return false;
    }
    static const QSet<QString> extensions{
        QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("webp"), QStringLiteral("gif"),
    };
    const QString suffix = extension.toLower();
    if (!extensions.contains(suffix)) {
        setStatus(QStringLiteral("Use a PNG, JPEG, WebP, or GIF image"));
        return false;
    }

    const QString root = attachmentRoot();
    if (root.isEmpty() || !QDir().mkpath(root)) {
        setStatus(QStringLiteral("Could not create the document attachment folder"));
        return false;
    }
    const QString fileName = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower() +
                             QLatin1Char('.') + suffix;
    const QString absolutePath = QDir(root).absoluteFilePath(fileName);
    QSaveFile output(absolutePath);
    if (!output.open(QIODevice::WriteOnly) ||
        !output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        output.write(data) != data.size() || !output.commit()) {
        setStatus(QStringLiteral("Could not store the image"));
        return false;
    }

    const QString relativePath = QFileInfo(m_filePath).dir().relativeFilePath(absolutePath);
    const QByteArray previous = m_session.document().source;
    const bool wasModified = m_modified;
    const EditResult result = m_session.apply(
        m_session.document().revision, QStringLiteral("attachment_add"), node,
        {{QStringLiteral("path"), relativePath}, {QStringLiteral("alt"), alt}});
    if (!result.ok) {
        QFile::remove(absolutePath);
        setStatus(result.errorMessage);
        return false;
    }
    notifySessionChanged(true);
    if (!save()) {
        setSource(previous, wasModified);
        if (wasModified)
            writeRecovery();
        else
            clearRecovery();
        QFile::remove(absolutePath);
        return false;
    }
    pushApiUndo(previous, {relativePath});
    setStatus(QStringLiteral("Image attached"));
    return true;
}

bool DocumentController::pasteImage(const QString &node) {
    const QImage image = QGuiApplication::clipboard()->image();
    if (image.isNull()) {
        setStatus(QStringLiteral("The clipboard does not contain an image"));
        return false;
    }
    if (image.width() > 8192 || image.height() > 8192) {
        setStatus(QStringLiteral("Images may be at most 8192 × 8192 pixels"));
        return false;
    }
    QByteArray data;
    QBuffer buffer(&data);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) {
        setStatus(QStringLiteral("Could not encode the clipboard image"));
        return false;
    }
    return attachImageData(node, data, QStringLiteral("png"), QStringLiteral("clipboard image"));
}

bool DocumentController::attachImage(const QString &node, const QUrl &url) {
    if (!url.isLocalFile()) {
        setStatus(QStringLiteral("Drop a local image file"));
        return false;
    }
    const QString path = url.toLocalFile();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > 8 * 1024 * 1024) {
        setStatus(QStringLiteral("Images must be readable and at most 8 MiB"));
        return false;
    }
    QImageReader reader(path);
    const QSize size = reader.size();
    if (!reader.canRead() || !size.isValid() || size.width() > 8192 || size.height() > 8192) {
        setStatus(QStringLiteral("The dropped file is not a supported image"));
        return false;
    }
    QString extension = QFileInfo(path).suffix().toLower();
    if (extension == QStringLiteral("jpe"))
        extension = QStringLiteral("jpg");
    const QByteArray data = file.readAll();
    return attachImageData(node, data, extension, QFileInfo(path).completeBaseName());
}

QJsonObject DocumentController::attachApiImage(const QString &node, const QByteArray &data,
                                               const QString &contentType,
                                               const QString &fileName,
                                               const QString &baseRevision) {
    const auto failure = [](const QString &code, const QString &message) {
        return QJsonObject{
            {QStringLiteral("ok"), false},
            {QStringLiteral("error"), QJsonObject{
                {QStringLiteral("code"), code},
                {QStringLiteral("message"), message},
            }},
        };
    };
    if (baseRevision.isEmpty())
        return failure(QStringLiteral("base_revision_required"),
                       QStringLiteral("baseRevision is required."));
    if (baseRevision != m_session.document().revision)
        return failure(QStringLiteral("stale_revision"),
                       QStringLiteral("The document changed after this upload was prepared."));

    static const QHash<QString, QString> extensions{
        {QStringLiteral("image/png"), QStringLiteral("png")},
        {QStringLiteral("image/jpeg"), QStringLiteral("jpg")},
        {QStringLiteral("image/webp"), QStringLiteral("webp")},
        {QStringLiteral("image/gif"), QStringLiteral("gif")},
    };
    const QString mime = contentType.section(QLatin1Char(';'), 0, 0).trimmed().toLower();
    if (!extensions.contains(mime))
        return failure(QStringLiteral("invalid_image"),
                       QStringLiteral("Use a PNG, JPEG, WebP, or GIF image."));
    if (data.isEmpty() || data.size() > 8 * 1024 * 1024)
        return failure(QStringLiteral("invalid_image"),
                       QStringLiteral("Images must be between 1 byte and 8 MiB."));

    QBuffer buffer;
    buffer.setData(data);
    if (!buffer.open(QIODevice::ReadOnly))
        return failure(QStringLiteral("invalid_image"),
                       QStringLiteral("The image data could not be read."));
    QImageReader reader(&buffer);
    const QSize size = reader.size();
    if (!reader.canRead() || !size.isValid() || size.width() > 8192 || size.height() > 8192)
        return failure(QStringLiteral("invalid_image"),
                       QStringLiteral("The image is invalid or larger than 8192 by 8192 pixels."));

    QString alt = QFileInfo(fileName).completeBaseName().simplified();
    if (alt.isEmpty())
        alt = QStringLiteral("pasted image");
    if (!attachImageData(node, data, extensions.value(mime), alt))
        return failure(QStringLiteral("attachment_failed"), status());
    return apiState();
}

bool DocumentController::removeAttachment(const QString &node, const QString &path) {
    const QByteArray previous = m_session.document().source;
    const bool wasModified = m_modified;
    QVector<ApiUndoEntry::RemovedAttachment> removedAttachments;
    const QString root = attachmentRoot();
    const QString absolute = QFileInfo(m_filePath).dir().absoluteFilePath(QDir::cleanPath(path));
    if (!root.isEmpty() && QFileInfo(absolute).absolutePath() == QFileInfo(root).absoluteFilePath()) {
        QFile attachmentFile(absolute);
        if (attachmentFile.open(QIODevice::ReadOnly))
            removedAttachments.append({path, attachmentFile.readAll()});
    }
    const EditResult result = m_session.apply(
        m_session.document().revision, QStringLiteral("attachment_remove"), node,
        {{QStringLiteral("path"), path}});
    if (!result.ok) {
        setStatus(result.errorMessage);
        return false;
    }
    notifySessionChanged(true);
    if (!save()) {
        setSource(previous, wasModified);
        if (wasModified)
            writeRecovery();
        else
            clearRecovery();
        return false;
    }
    if (!root.isEmpty() && QFileInfo(absolute).absolutePath() == QFileInfo(root).absoluteFilePath())
        QFile::remove(absolute);
    pushApiUndo(previous, {}, removedAttachments);
    setStatus(QStringLiteral("Image removed"));
    return true;
}

bool DocumentController::launchPreview() {
    if (m_filePath.isEmpty()) {
        setStatus(QStringLiteral("Save the document before opening its preview"));
        return false;
    }
    if (m_modified && !save())
        return false;
    if (Mustermark::activatePreview(m_filePath)) {
        setStatus(QStringLiteral("Preview activated"));
        return true;
    }
    const bool started = QProcess::startDetached(
        QCoreApplication::applicationFilePath(), {m_filePath, QStringLiteral("--visual")});
    setStatus(started ? QStringLiteral("Preview launched")
                      : QStringLiteral("Could not launch the preview application"));
    return started;
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
    if (!m_apiUndo.isEmpty()) {
        m_apiUndo.clear();
        emit undoAvailableChanged();
    }
    if (m_sourceEditActive) {
        m_sourceEditStart = disk.source;
        m_sourceEditStartModified = false;
    }
    setSource(disk.source, false);
    setStatus(QStringLiteral("Reloaded external changes"));
}

bool DocumentController::reloadFromDisk() {
    if (m_filePath.isEmpty())
        return false;
    const FileResult disk = FileDocument::read(m_filePath);
    if (!disk.ok) {
        setStatus(disk.error);
        return false;
    }
    m_diskRevision = DocumentEngine::revisionFor(disk.source);
    setSource(disk.source, false);
    setConflict(false);
    clearRecovery();
    m_apiUndo.clear();
    emit undoAvailableChanged();
    if (m_sourceEditActive) {
        m_sourceEditStart = disk.source;
        m_sourceEditStartModified = false;
    }
    setStatus(QStringLiteral("Reloaded disk version"));
    return true;
}

bool DocumentController::saveCopy(const QUrl &url) {
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    const FileResult result = FileDocument::writeAtomic(path, m_session.document().source);
    if (!result.ok) {
        setStatus(result.error);
        return false;
    }
    setStatus(QStringLiteral("Saved copy as %1").arg(QFileInfo(path).fileName()));
    return true;
}

bool DocumentController::restoreRecovery() {
    QFile file(recoveryPath());
    if (!file.open(QIODevice::ReadOnly)) {
        setStatus(QStringLiteral("Recovery draft is no longer available"));
        setRecoveryAvailable(false);
        return false;
    }
    const QByteArray recovered = file.readAll();
    if (!m_apiUndo.isEmpty()) {
        m_apiUndo.clear();
        emit undoAvailableChanged();
    }
    if (m_sourceEditActive) {
        m_sourceEditStart = recovered;
        m_sourceEditStartModified = true;
    }
    setSource(recovered, true);
    setConflict(false);
    setStatus(QStringLiteral("Recovery draft restored; save to write the document"));
    return true;
}

void DocumentController::discardRecovery() {
    clearRecovery();
    setStatus(QStringLiteral("Recovery draft discarded"));
}

bool DocumentController::undoDocumentChange() {
    const QJsonObject result = undoApiChange();
    if (!result.value(QStringLiteral("ok")).toBool()) {
        setStatus(result.value(QStringLiteral("error")).toObject()
                      .value(QStringLiteral("message")).toString());
        return false;
    }
    return true;
}

void DocumentController::setSource(const QByteArray &bytes, bool isModified) {
    m_session.setSource(bytes);
    if (m_filePath.isEmpty()) m_session.setHeadingIdentity({}, 0, {});
    else if (!isModified && QFileInfo::exists(QFileInfo(m_filePath).canonicalFilePath() + ".mustermark.json")) {
        const auto linked = FileDocument::readLinked(m_filePath, m_session);
        if (!linked.ok) { setConflict(true); setStatus(linked.error); }
    }
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
    result.insert(QStringLiteral("name"), m_filePath.isEmpty() ? QStringLiteral("Untitled")
                                                               : QFileInfo(m_filePath).fileName());
    result.insert(QStringLiteral("status"), m_status);
    result.insert(QStringLiteral("conflict"), m_conflict);
    result.insert(QStringLiteral("source"), source());
    result.insert(QStringLiteral("html"), renderedHtml());
    return result;
}

void DocumentController::pushApiUndo(const QByteArray &source,
                                     const QStringList &createdAttachments,
                                     const QVector<ApiUndoEntry::RemovedAttachment> &removedAttachments) {
    const QString currentRevision = m_session.document().revision;
    if (!m_apiUndo.isEmpty() && m_apiUndo.constLast().resultingRevision !=
                                    DocumentEngine::revisionFor(source))
        m_apiUndo.clear();
    m_apiUndo.append({source, currentRevision, m_modified, createdAttachments, removedAttachments});
    constexpr qsizetype maxUndoEntries = 100;
    while (m_apiUndo.size() > maxUndoEntries)
        m_apiUndo.removeFirst();
    emit undoAvailableChanged();
}

QJsonObject DocumentController::undoApiChange() {
    const auto failure = [](const QString &code, const QString &message) {
        return QJsonObject{
            {QStringLiteral("ok"), false},
            {QStringLiteral("error"), QJsonObject{
                {QStringLiteral("code"), code},
                {QStringLiteral("message"), message},
            }},
        };
    };
    if (m_apiUndo.isEmpty())
        return failure(QStringLiteral("nothing_to_undo"),
                       QStringLiteral("There is no structural change to undo."));
    const ApiUndoEntry entry = m_apiUndo.constLast();
    if (entry.resultingRevision != m_session.document().revision) {
        m_apiUndo.clear();
        return failure(QStringLiteral("nothing_to_undo"),
                       QStringLiteral("The document changed outside this session; its undo history was cleared."));
    }
    QStringList restoredFiles;
    if (!m_filePath.isEmpty()) {
        const QDir documentDirectory = QFileInfo(m_filePath).dir();
        for (const ApiUndoEntry::RemovedAttachment &attachment : entry.removedAttachments) {
            const QString absolute = documentDirectory.absoluteFilePath(QDir::cleanPath(attachment.path));
            QSaveFile file(absolute);
            if (!file.open(QIODevice::WriteOnly) || file.write(attachment.data) != attachment.data.size() ||
                !file.commit()) {
                for (const QString &restored : restoredFiles)
                    QFile::remove(restored);
                return failure(QStringLiteral("write_failed"),
                               QStringLiteral("Could not restore a removed attachment."));
            }
            restoredFiles.append(absolute);
        }
    }
    if (m_filePath.isEmpty()) {
        setSource(entry.source, entry.wasModified);
    } else {
        const FileResult written = FileDocument::writeAtomic(m_filePath, entry.source, m_diskRevision);
        if (!written.ok) {
            for (const QString &restored : restoredFiles)
                QFile::remove(restored);
            return failure(written.error == QStringLiteral("stale_revision")
                               ? QStringLiteral("stale_revision")
                               : QStringLiteral("write_failed"), written.error);
        }
        m_session.setSource(entry.source);
        if (QFileInfo::exists(QFileInfo(m_filePath).canonicalFilePath() + ".mustermark.json")) {
            const auto linked = FileDocument::readLinked(m_filePath, m_session);
            if (!linked.ok) return failure(linked.error, linked.error);
        }
        m_diskRevision = m_session.document().revision;
    }
    m_apiUndo.removeLast();
    emit undoAvailableChanged();
    if (!m_filePath.isEmpty() && m_modified) {
        m_modified = false;
        emit modifiedChanged();
    }
    if (m_filePath.isEmpty() && entry.wasModified)
        writeRecovery();
    else
        clearRecovery();
    watchCurrentFile();

    QSet<QString> retainedAttachments;
    for (const Node &node : m_session.document().nodes)
        for (const Attachment &attachment : node.attachments)
            retainedAttachments.insert(attachment.path);
    const QString root = attachmentRoot();
    const QDir documentDirectory = QFileInfo(m_filePath).dir();
    for (const QString &path : entry.createdAttachments) {
        if (retainedAttachments.contains(path) || root.isEmpty() || !QDir::isRelativePath(path))
            continue;
        const QString absolute = documentDirectory.absoluteFilePath(QDir::cleanPath(path));
        if (QFileInfo(absolute).absolutePath() == QFileInfo(root).absoluteFilePath())
            QFile::remove(absolute);
    }

    emit sourceChanged();
    emit documentChanged();
    setStatus(QStringLiteral("Document change undone"));
    QJsonObject result = apiState();
    result.insert(QStringLiteral("edits"), QJsonArray{
        QJsonObject{{QStringLiteral("kind"), QStringLiteral("undo")}}
    });
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

    const QString action = instruction.value(QStringLiteral("action")).toString();
    if (action == QStringLiteral("snapshot"))
        return m_session.snapshot(
            m_filePath, instruction.value(QStringLiteral("node")).toString(),
            instruction.value(QStringLiteral("scope")).toString(QStringLiteral("item")));
    if (action == QStringLiteral("undo"))
        return undoApiChange();

    const QByteArray previousSource = m_session.document().source;
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
            m_filePath, m_session.document().source, m_diskRevision, &m_session);
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
        pushApiUndo(previousSource);
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

void DocumentController::writeRecovery() {
    const QString path = recoveryPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(m_session.document().source);
        file.commit();
    }
}

void DocumentController::clearRecovery() {
    QFile::remove(recoveryPath());
    setRecoveryAvailable(false);
}

void DocumentController::setRecoveryAvailable(bool available) {
    if (m_recoveryAvailable == available)
        return;
    m_recoveryAvailable = available;
    emit recoveryAvailableChanged();
}
