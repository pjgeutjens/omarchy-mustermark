#include "documentsession.h"

#include <QCryptographicHash>
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

} // namespace

DocumentSession::DocumentSession() {
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
    parseAndReconcile(source);
}

void DocumentSession::parseAndReconcile(const QByteArray &source,
                                        const QString &preferredSessionId,
                                        NodeKind preferredKind,
                                        const QString &preferredFingerprint,
                                        qsizetype preferredByte,
                                        bool targetDeleted) {
    const Document previous = m_document;
    Document next = m_engine.parse(source);
    for (Node &node : next.nodes)
        node.fingerprint = fingerprintFor(node);

    if (!m_tracking) {
        next.tracked = false;
        next.trackingVersion = 0;
        next.renderedHtml = DocumentEngine::enrichHtml(next.renderedHtml, next.nodes);
        m_document = std::move(next);
        return;
    }

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
        assign(oldIndex, best);
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

    for (int oldIndex = 0; oldIndex < previous.nodes.size(); ++oldIndex) {
        if (oldUsed.at(oldIndex) || previous.nodes.at(oldIndex).sessionId.isEmpty())
            continue;
        const QString key = matchingKey(previous, oldIndex, false);
        int best = -1;
        int bestDistance = std::numeric_limits<int>::max();
        for (int newIndex = 0; newIndex < next.nodes.size(); ++newIndex) {
            if (newUsed.at(newIndex) || matchingKey(next, newIndex, false) != key)
                continue;
            const int distance = std::abs(previous.nodes.at(oldIndex).startLine -
                                          next.nodes.at(newIndex).startLine);
            if (distance < bestDistance) {
                best = newIndex;
                bestDistance = distance;
            }
        }
        assign(oldIndex, best);
    }

    // A user may rewrite a heading or item directly in the source editor. Once all
    // unchanged nodes have been matched by content, keep the remaining identity on
    // the nearest node of the same kind. This makes temporary labels survive typing
    // without confusing an inserted duplicate with an existing unchanged node.
    for (int oldIndex = 0; oldIndex < previous.nodes.size(); ++oldIndex) {
        if (oldUsed.at(oldIndex) || previous.nodes.at(oldIndex).sessionId.isEmpty())
            continue;
        int best = -1;
        int bestDistance = std::numeric_limits<int>::max();
        for (int newIndex = 0; newIndex < next.nodes.size(); ++newIndex) {
            if (newUsed.at(newIndex) ||
                previous.nodes.at(oldIndex).kind != next.nodes.at(newIndex).kind)
                continue;
            const int distance = std::abs(previous.nodes.at(oldIndex).startLine -
                                          next.nodes.at(newIndex).startLine);
            if (distance < bestDistance) {
                best = newIndex;
                bestDistance = distance;
            }
        }
        assign(oldIndex, best);
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
    next.tracked = true;
    next.trackingVersion = 1;
    next.renderedHtml = DocumentEngine::enrichHtml(next.renderedHtml, next.nodes);
    m_document = std::move(next);
}

EditResult DocumentSession::startTracking() {
    if (m_tracking)
        return {true, m_document.source, {}, {}, {}};
    m_tracking = true;
    m_sessionPrefix = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    m_nextIdentity = 0;
    m_labels.clear();
    parseAndReconcile(m_document.source);
    return {true, m_document.source, {}, {}, {}};
}

EditResult DocumentSession::stopTracking() {
    if (!m_tracking)
        return {true, m_document.source, {}, {}, {}};
    m_tracking = false;
    m_sessionPrefix.clear();
    m_nextIdentity = 0;
    m_labels.clear();
    parseAndReconcile(m_document.source);
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
        if (!m_tracking || selected.sessionId.isEmpty())
            return {false, m_document.source, QStringLiteral("tracking_required"),
                    QStringLiteral("Start tracking before changing temporary labels."), {}};
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
    parseAndReconcile(result.source, selected.sessionId, selected.kind,
                      action == QStringLiteral("edit") ? QString() : selected.fingerprint,
                      preferredByte, action == QStringLiteral("delete"));
    result.source = m_document.source;
    return result;
}

} // namespace Mustermark
