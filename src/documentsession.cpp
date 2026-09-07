#include "documentsession.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QUuid>

#include <algorithm>
#include <limits>

namespace Mustermark {
namespace {

QString normalizedText(const Node &node) {
    return node.text.normalized(QString::NormalizationForm_C).simplified();
}

QString fingerprintType(const Node &node) {
    switch (node.kind) {
    case NodeKind::Heading: return QStringLiteral("heading");
    case NodeKind::List: return node.ordered ? QStringLiteral("ordered-list")
                                             : QStringLiteral("list");
    case NodeKind::Item: return node.task ? QStringLiteral("task")
                                         : QStringLiteral("item");
    case NodeKind::Block: return QStringLiteral("block");
    }
    return QStringLiteral("block");
}

QStringList words(const Node &node) {
    return normalizedText(node).toLower().split(
        QRegularExpression(QStringLiteral(R"([^\p{L}\p{N}]+)")), Qt::SkipEmptyParts);
}

bool relatedText(const Node &oldNode, const Node &newNode) {
    const QString oldText = normalizedText(oldNode).toLower();
    const QString newText = normalizedText(newNode).toLower();
    if (oldText == newText)
        return true;
    if (oldText.isEmpty() || newText.isEmpty())
        return false;
    if ((oldText.startsWith(newText) || newText.startsWith(oldText)) &&
        std::min(oldText.size(), newText.size()) >= 3)
        return true;

    const QStringList oldWords = words(oldNode);
    const QStringList newWords = words(newNode);
    int shared = 0;
    QStringList remaining = newWords;
    for (const QString &word : oldWords) {
        const int index = remaining.indexOf(word);
        if (index >= 0) {
            ++shared;
            remaining.removeAt(index);
        }
    }
    return shared > 0 && shared * 2 >= std::min(oldWords.size(), newWords.size());
}

QVector<int> containedNodes(const Document &document, int rootIndex) {
    QVector<int> result;
    if (rootIndex < 0 || rootIndex >= document.nodes.size())
        return result;
    const Node &root = document.nodes.at(rootIndex);
    for (int index = 0; index < document.nodes.size(); ++index) {
        const Node &candidate = document.nodes.at(index);
        if (candidate.startByte >= root.startByte && candidate.endByte <= root.endByte)
            result.append(index);
    }
    return result;
}

} // namespace

DocumentSession::DocumentSession() {
    m_sessionPrefix = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    setSource({});
}

QString DocumentSession::fingerprintFor(const Node &node) {
    const QString type = fingerprintType(node);
    const QByteArray material = QByteArrayLiteral("mustermark:v1\0") + type.toUtf8() + '\0' +
                                normalizedText(node).toUtf8();
    const QByteArray digest = QCryptographicHash::hash(material, QCryptographicHash::Sha256)
                                  .left(12)
                                  .toBase64(QByteArray::Base64UrlEncoding |
                                            QByteArray::OmitTrailingEquals);
    return QStringLiteral("mm1:%1:%2").arg(type, QString::fromLatin1(digest));
}

QString DocumentSession::matchingKey(const Document &document, int nodeIndex, bool withContext) {
    const Node &node = document.nodes.at(nodeIndex);
    QString key = QString::number(static_cast<int>(node.kind)) + QLatin1Char('|') + node.fingerprint;
    if (!withContext)
        return key;
    QStringList context;
    int parent = node.parent;
    while (parent >= 0) {
        const Node &ancestor = document.nodes.at(parent);
        if (ancestor.kind == NodeKind::Heading || ancestor.kind == NodeKind::Item)
            context.prepend(QString::number(static_cast<int>(ancestor.kind)) + QLatin1Char(':') +
                            normalizedText(ancestor));
        parent = ancestor.parent;
    }
    return key + QLatin1Char('|') + context.join(QLatin1Char('/'));
}

QString DocumentSession::nextSessionId() {
    return QStringLiteral("s:%1:%2").arg(m_sessionPrefix).arg(++m_nextIdentity);
}

void DocumentSession::setSource(const QByteArray &source) {
    if (source == m_document.source && !m_document.revision.isEmpty())
        return;
    parseAndReconcile(source);
}

void DocumentSession::setHeadingIdentity(const QString &documentId, qint64 generation,
                                        const QHash<qsizetype, QString> &headings,
                                        const QJsonObject &bindings) {
    m_document.documentId = documentId;
    m_document.identityGeneration = generation;
    m_document.externalBindings = bindings;
    for (Node &node : m_document.nodes)
        node.durableId = (node.kind == NodeKind::Heading || node.kind == NodeKind::Item)
            ? headings.value(node.startByte) : QString();
}

QJsonObject DocumentSession::externalBinding(const QString &baseRevision,
                                              const QString &nameSpace,
                                              const QString &value,
                                              const QString &nodeIdentity) {
    const auto failure = [](const QString &code) {
        return QJsonObject{{QStringLiteral("ok"), false},
            {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), code},
                {QStringLiteral("message"), code}}}};
    };
    if (baseRevision.isEmpty())
        return failure(QStringLiteral("base_revision_required"));
    if (baseRevision != m_document.revision)
        return failure(QStringLiteral("stale_revision"));
    static const QRegularExpression pattern(QStringLiteral(R"(^[a-z0-9][a-z0-9_.-]{0,63}$)"));
    if (!pattern.match(nameSpace).hasMatch() || value.trimmed().isEmpty() ||
        value.size() > 256 || value.contains(QChar::Null))
        return failure(QStringLiteral("invalid_external_id"));
    const bool durable = !m_document.documentId.isEmpty();
    const QString bound = durable ? m_document.externalBindings.value(nameSpace).toObject().value(value).toString()
                                 : m_externalBindings.value(nameSpace).value(value);
    if (!bound.isEmpty() && m_document.findNode(bound) < 0)
        return failure(QStringLiteral("external_id_retired"));
    QString target = bound;
    if (!nodeIdentity.isEmpty()) {
        const int index = m_document.findNode(nodeIdentity);
        // Binding requires the actual session identity, never a fingerprint/ref.
        if (index < 0 || (!durable && m_document.nodes.at(index).sessionId != nodeIdentity) ||
            (durable && m_document.nodes.at(index).durableId.isEmpty()))
            return failure(QStringLiteral("unknown_id"));
        const QString identity = durable ? m_document.nodes.at(index).durableId : nodeIdentity;
        if (!bound.isEmpty() && bound != identity)
            return failure(QStringLiteral("external_id_collision"));
        target = identity;
        if (durable) {
            auto bindings = m_document.externalBindings.value(nameSpace).toObject();
            bindings.insert(value, target);
            m_document.externalBindings.insert(nameSpace, bindings);
        } else m_externalBindings[nameSpace].insert(value, target);
    }
    if (target.isEmpty())
        return failure(QStringLiteral("unknown_external_id"));
    return {{QStringLiteral("ok"), true},
        {QStringLiteral("revision"), m_document.revision},
        {QStringLiteral("identityScope"), durable ? QStringLiteral("document") : QStringLiteral("session")},
        {QStringLiteral("namespace"), nameSpace}, {QStringLiteral("value"), value},
        {QStringLiteral("node"), target}};
}

void DocumentSession::parseAndReconcile(const QByteArray &source,
                                        const QString &preferredSessionId,
                                        NodeKind preferredKind,
                                        const QString &preferredFingerprint,
                                        qsizetype preferredByte,
                                        bool targetDeleted,
                                        bool preserveSubtree) {
    const Document previous = m_document;
    Document next = m_engine.parse(source);
    next.documentId = previous.documentId;
    next.identityGeneration = previous.identityGeneration;
    next.externalBindings = previous.externalBindings;
    for (Node &node : next.nodes)
        node.fingerprint = fingerprintFor(node);

    QVector<bool> oldUsed(previous.nodes.size(), false);
    QVector<bool> newUsed(next.nodes.size(), false);

    auto assign = [&](int oldIndex, int newIndex) {
        if (oldIndex < 0 || newIndex < 0 || oldIndex >= previous.nodes.size() ||
            newIndex >= next.nodes.size() || oldUsed.at(oldIndex) || newUsed.at(newIndex))
            return false;
        const QString identity = previous.nodes.at(oldIndex).sessionId;
        if (identity.isEmpty())
            return false;
        next.nodes[newIndex].sessionId = identity;
        next.nodes[newIndex].durableId = previous.nodes.at(oldIndex).durableId;
        oldUsed[oldIndex] = true;
        newUsed[newIndex] = true;
        return true;
    };

    if (!preferredSessionId.isEmpty() && !targetDeleted) {
        const int oldIndex = std::find_if(previous.nodes.cbegin(), previous.nodes.cend(),
                                          [&](const Node &node) {
                                              return node.sessionId == preferredSessionId;
                                          }) - previous.nodes.cbegin();
        int best = -1;
        qsizetype bestDistance = std::numeric_limits<qsizetype>::max();
        for (int index = 0; index < next.nodes.size(); ++index) {
            const Node &candidate = next.nodes.at(index);
            if (candidate.kind != preferredKind)
                continue;
            if (!preferredFingerprint.isEmpty() && candidate.fingerprint != preferredFingerprint)
                continue;
            const qsizetype distance = preferredByte < 0 ? 0
                : std::abs(candidate.startByte - preferredByte);
            if (distance < bestDistance) {
                best = index;
                bestDistance = distance;
            }
        }
        if (assign(oldIndex, best) && preserveSubtree) {
            const QVector<int> oldSubtree = containedNodes(previous, oldIndex);
            const QVector<int> newSubtree = containedNodes(next, best);
            if (oldSubtree.size() == newSubtree.size()) {
                bool sameStructure = true;
                for (int offset = 0; offset < oldSubtree.size(); ++offset) {
                    const Node &oldNode = previous.nodes.at(oldSubtree.at(offset));
                    const Node &newNode = next.nodes.at(newSubtree.at(offset));
                    if (oldNode.kind != newNode.kind || oldNode.fingerprint != newNode.fingerprint) {
                        sameStructure = false;
                        break;
                    }
                }
                if (sameStructure) {
                    for (int offset = 1; offset < oldSubtree.size(); ++offset)
                        assign(oldSubtree.at(offset), newSubtree.at(offset));
                }
            }
        }
    }

    auto matchUnique = [&](bool withContext) {
        QHash<QString, QVector<int>> oldGroups;
        QHash<QString, QVector<int>> newGroups;
        for (int index = 0; index < previous.nodes.size(); ++index) {
            if (!oldUsed.at(index) && !previous.nodes.at(index).sessionId.isEmpty())
                oldGroups[matchingKey(previous, index, withContext)].append(index);
        }
        for (int index = 0; index < next.nodes.size(); ++index) {
            if (!newUsed.at(index))
                newGroups[matchingKey(next, index, withContext)].append(index);
        }
        for (auto it = oldGroups.cbegin(); it != oldGroups.cend(); ++it) {
            const QVector<int> candidates = newGroups.value(it.key());
            if (it.value().size() == 1 && candidates.size() == 1)
                assign(it.value().first(), candidates.first());
        }
    };

    matchUnique(true);
    matchUnique(false);

    // A direct wording edit may keep identity only when there is one structurally
    // plausible old/new pair and their text remains recognisably related. Position
    // helps choose a candidate; it never establishes identity on its own.
    QHash<NodeKind, QVector<int>> oldByKind;
    QHash<NodeKind, QVector<int>> newByKind;
    for (int index = 0; index < previous.nodes.size(); ++index)
        if (!oldUsed.at(index) && !previous.nodes.at(index).sessionId.isEmpty())
            oldByKind[previous.nodes.at(index).kind].append(index);
    for (int index = 0; index < next.nodes.size(); ++index)
        if (!newUsed.at(index))
            newByKind[next.nodes.at(index).kind].append(index);
    for (auto it = oldByKind.cbegin(); it != oldByKind.cend(); ++it) {
        const QVector<int> candidates = newByKind.value(it.key());
        if (it.value().size() != 1 || candidates.size() != 1)
            continue;
        const int oldIndex = it.value().constFirst();
        const int newIndex = candidates.constFirst();
        const Node &oldNode = previous.nodes.at(oldIndex);
        const Node &newNode = next.nodes.at(newIndex);
        const bool sameParent = (oldNode.parent < 0 && newNode.parent < 0) ||
            (oldNode.parent >= 0 && newNode.parent >= 0 &&
             previous.nodes.at(oldNode.parent).sessionId ==
                 next.nodes.at(newNode.parent).sessionId);
        if (sameParent && relatedText(oldNode, newNode))
            assign(oldIndex, newIndex);
    }

    m_invalidatedIds.clear();
    for (int index = 0; index < previous.nodes.size(); ++index) {
        const QString identity = previous.nodes.at(index).sessionId;
        if (!identity.isEmpty() && !oldUsed.at(index)) {
            m_invalidatedIds.append(identity);
            m_labels.remove(identity);
        }
    }

    for (int index = 0; index < next.nodes.size(); ++index) {
        Node &node = next.nodes[index];
        if (node.sessionId.isEmpty())
            node.sessionId = nextSessionId();
        const QStringList temporaryLabels = m_labels.value(node.sessionId);
        for (const QString &label : temporaryLabels) {
            if (!node.labels.contains(label))
                node.labels.append(label);
        }
    }
    next.invalidatedIds = m_invalidatedIds;
    next.tracked = true;
    next.trackingVersion = 1;
    next.renderedHtml = DocumentEngine::enrichHtml(next.renderedHtml, next.nodes);
    m_document = std::move(next);
}

EditResult DocumentSession::startTracking() {
    return {true, m_document.source, {}, {}, {}};
}

EditResult DocumentSession::stopTracking() {
    // Kept as a compatibility query for older clients. Session identity is now
    // automatic and lasts until this DocumentSession is destroyed.
    return {true, m_document.source, {}, {}, {}};
}

EditResult DocumentSession::apply(const QString &baseRevision, const QString &action,
                                  const QString &nodeIdentity, QJsonObject arguments) {
    if (baseRevision != m_document.revision)
        return {false, m_document.source, QStringLiteral("stale_revision"),
                QStringLiteral("The document changed after this instruction was prepared."), {}};
    const int nodeIndex = m_document.findNode(nodeIdentity);
    if (nodeIndex < 0)
        return {false, m_document.source, QStringLiteral("unknown_id"),
                QStringLiteral("The selected node no longer exists."), {}};
    const Node selected = m_document.nodes.at(nodeIndex);

    if (action == QStringLiteral("label_add") || action == QStringLiteral("label_remove")) {
        if (selected.sessionId.isEmpty())
            return {false, m_document.source, QStringLiteral("tracking_required"),
                    QStringLiteral("Temporary labels require an active document session."), {}};
        const QString label = arguments.value(QStringLiteral("label")).toString();
        static const QRegularExpression labelPattern(QStringLiteral(R"(^[a-z0-9][a-z0-9_.\/-]{0,63}$)"));
        if (!labelPattern.match(label).hasMatch())
            return {false, m_document.source, QStringLiteral("invalid_label"),
                    QStringLiteral("Labels must be lowercase slugs of at most 64 characters."), {}};
        QStringList labels = m_labels.value(selected.sessionId);
        if (action == QStringLiteral("label_add") && !labels.contains(label))
            labels.append(label);
        if (action == QStringLiteral("label_remove"))
            labels.removeAll(label);
        m_labels.insert(selected.sessionId, labels);
        parseAndReconcile(m_document.source, selected.sessionId, selected.kind,
                          selected.fingerprint, selected.startByte);
        return {true, m_document.source, {}, {}, {}};
    }

    const QString targetIdentity = arguments.value(QStringLiteral("target")).toString();
    int targetIndex = -1;
    if (!targetIdentity.isEmpty()) {
        targetIndex = m_document.findNode(targetIdentity);
        if (targetIndex < 0)
            return {false, m_document.source, QStringLiteral("unknown_id"),
                    QStringLiteral("The destination no longer exists."), {}};
        const Node &target = m_document.nodes.at(targetIndex);
        arguments.insert(QStringLiteral("target"),
                         target.id.isEmpty() ? target.ref : target.id);
    }

    qsizetype preferredByte = selected.startByte;
    if ((action == QStringLiteral("move_before") || action == QStringLiteral("move_after")) &&
        targetIndex >= 0) {
        const Node &target = m_document.nodes.at(targetIndex);
        const qsizetype insertion = action == QStringLiteral("move_before")
                                        ? target.startByte : target.endByte;
        const qsizetype blockSize = selected.endByte - selected.startByte;
        preferredByte = insertion - (selected.startByte < insertion ? blockSize : 0);
    }

    const QString internalIdentity = selected.id.isEmpty() ? selected.ref : selected.id;
    EditResult result = m_engine.apply(m_document.source, m_document.revision, action,
                                       internalIdentity, arguments);
    if (!result.ok)
        return result;
    const bool preserveSubtree = action == QStringLiteral("move_before") ||
        action == QStringLiteral("move_after") || action == QStringLiteral("promote") ||
        action == QStringLiteral("demote") || action == QStringLiteral("indent") ||
        action == QStringLiteral("outdent");
    if (action == QStringLiteral("delete")) {
        for (auto &bindings : m_externalBindings) {
            for (auto it = bindings.begin(); it != bindings.end(); ++it) {
                const int index = m_document.findNode(it.value());
                if (index >= 0 && m_document.nodes.at(index).startByte >= selected.startByte &&
                    m_document.nodes.at(index).endByte <= selected.endByte)
                    it.value() = QStringLiteral("retired:") + it.value();
            }
        }
    }
    parseAndReconcile(result.source, selected.sessionId, selected.kind,
                      (action == QStringLiteral("edit") || action == QStringLiteral("replace"))
                          ? QString() : selected.fingerprint,
                      preferredByte, action == QStringLiteral("delete"), preserveSubtree);
    result.source = m_document.source;
    return result;
}

QJsonObject DocumentSession::snapshot(const QString &path, const QString &nodeIdentity,
                                      const QString &scope, bool includeAttachmentData) const {
    const auto failure = [](const QString &code, const QString &message) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("error"), QJsonObject{
                               {QStringLiteral("code"), code},
                               {QStringLiteral("message"), message}}}};
    };
    const int selectedIndex = m_document.findNode(nodeIdentity);
    if (selectedIndex < 0)
        return failure(QStringLiteral("unknown_id"),
                       QStringLiteral("The selected node no longer exists."));
    const Node &selected = m_document.nodes.at(selectedIndex);
    QVector<int> unitIndexes;
    if (scope == QStringLiteral("item")) {
        if (selected.kind != NodeKind::Item)
            return failure(QStringLiteral("invalid_scope"),
                           QStringLiteral("Item snapshots require a list item."));
        unitIndexes.append(selectedIndex);
    } else if (scope == QStringLiteral("section")) {
        if (selected.kind != NodeKind::Heading)
            return failure(QStringLiteral("invalid_scope"),
                           QStringLiteral("Section snapshots require a heading."));
        for (int index = 0; index < m_document.nodes.size(); ++index) {
            const Node &candidate = m_document.nodes.at(index);
            if (candidate.kind != NodeKind::Item || candidate.startByte <= selected.startByte ||
                candidate.endByte > selected.endByte || candidate.parent < 0)
                continue;
            const Node &list = m_document.nodes.at(candidate.parent);
            if (list.kind == NodeKind::List && list.parent == selectedIndex)
                unitIndexes.append(index);
        }
    } else {
        return failure(QStringLiteral("invalid_scope"),
                       QStringLiteral("Snapshot scope must be item or section."));
    }

    const QFileInfo documentFile(path);
    const QDir documentDirectory = documentFile.absoluteDir();
    const QString canonicalRoot = QFileInfo(documentDirectory.absolutePath()).canonicalFilePath();
    if (canonicalRoot.isEmpty())
        return failure(QStringLiteral("invalid_path"),
                       QStringLiteral("The document directory does not exist."));

    QJsonArray units;
    qint64 totalBytes = 0;
    for (int unitIndex : unitIndexes) {
        const Node &unit = m_document.nodes.at(unitIndex);
        QStringList texts;
        QJsonArray attachments;
        for (const Node &candidate : m_document.nodes) {
            if (candidate.startByte < unit.startByte || candidate.endByte > unit.endByte)
                continue;
            if (candidate.kind == NodeKind::Item && !candidate.text.isEmpty())
                texts.append(candidate.text.normalized(QString::NormalizationForm_C));
            for (const Attachment &attachment : candidate.attachments) {
                const QString clean = QDir::cleanPath(attachment.path);
                if (!QDir::isRelativePath(clean) || clean == QStringLiteral("..") ||
                    clean.startsWith(QStringLiteral("../")))
                    return failure(QStringLiteral("unsafe_attachment"),
                                   QStringLiteral("An attachment path escapes the document directory."));
                const QFileInfo file(documentDirectory.absoluteFilePath(clean));
                if (!file.isFile() || file.isSymLink())
                    return failure(file.exists() ? QStringLiteral("unsafe_attachment")
                                                 : QStringLiteral("missing_attachment"),
                                   QStringLiteral("An attachment is missing or is not a regular file."));
                const QString canonical = file.canonicalFilePath();
                if (canonical.isEmpty() ||
                    !(canonical == canonicalRoot || canonical.startsWith(canonicalRoot + QDir::separator())))
                    return failure(QStringLiteral("unsafe_attachment"),
                                   QStringLiteral("An attachment resolves outside the document directory."));
                constexpr qint64 maxSnapshotAttachmentBytes = 8 * 1024 * 1024;
                if (file.size() < 0 || file.size() > maxSnapshotAttachmentBytes)
                    return failure(QStringLiteral("attachment_too_large"),
                                   QStringLiteral("Snapshot attachments are limited to 8 MiB each."));
                QFile input(canonical);
                if (!input.open(QIODevice::ReadOnly))
                    return failure(QStringLiteral("attachment_unreadable"),
                                   QStringLiteral("An attachment could not be read."));
                const QByteArray bytes = input.read(maxSnapshotAttachmentBytes + 1);
                totalBytes += bytes.size();
                if (bytes.size() > maxSnapshotAttachmentBytes || (includeAttachmentData && totalBytes > maxSnapshotAttachmentBytes))
                    return failure("attachment_too_large", "Snapshot attachment data exceeds 8 MiB.");
                const QString suffix = file.suffix().toLower();
                const QHash<QString, QString> mimeTypes{
                    {QStringLiteral("png"), QStringLiteral("image/png")},
                    {QStringLiteral("jpg"), QStringLiteral("image/jpeg")},
                    {QStringLiteral("jpeg"), QStringLiteral("image/jpeg")},
                    {QStringLiteral("webp"), QStringLiteral("image/webp")},
                    {QStringLiteral("gif"), QStringLiteral("image/gif")},
                };
                if (!mimeTypes.contains(suffix))
                    return failure(QStringLiteral("unsupported_attachment"),
                                   QStringLiteral("Only PNG, JPEG, WebP, and GIF attachments are supported."));
                QJsonObject attachmentData{
                    {QStringLiteral("relativePath"), clean},
                    {QStringLiteral("mimeType"), mimeTypes.value(suffix)},
                    {QStringLiteral("size"), static_cast<double>(bytes.size())},
                    {QStringLiteral("sha256"), QStringLiteral("sha256:") +
                        QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())},
                };
                if (includeAttachmentData) attachmentData.insert("dataBase64", QString::fromLatin1(bytes.toBase64()));
                attachments.append(attachmentData);
            }
        }
        units.append(QJsonObject{
            {QStringLiteral("durableId"), unit.durableId},
            {QStringLiteral("markdown"), QString::fromUtf8(
                m_document.source.mid(unit.startByte, unit.endByte - unit.startByte))},
            {QStringLiteral("text"), texts.join(QLatin1Char('\n'))},
            {QStringLiteral("task"), unit.task},
            {QStringLiteral("checked"), unit.checked},
            {QStringLiteral("attachments"), attachments},
        });
    }

    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("snapshotVersion"), 1},
        {QStringLiteral("path"), documentFile.absoluteFilePath()},
        {QStringLiteral("revision"), m_document.revision},
        {QStringLiteral("scope"), scope},
        {QStringLiteral("node"), QJsonObject{
            {QStringLiteral("kind"), DocumentEngine::kindName(selected.kind)},
            {QStringLiteral("ref"), selected.ref},
            {QStringLiteral("sessionId"), selected.sessionId},
            {QStringLiteral("fingerprint"), selected.fingerprint},
        }},
        {QStringLiteral("units"), units},
    };
}

} // namespace Mustermark
