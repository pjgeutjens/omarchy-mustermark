#pragma once

#include "documentengine.h"

#include <QHash>

namespace Mustermark {

class DocumentSession {
public:
    DocumentSession();

    const Document &document() const { return m_document; }
    bool tracking() const { return m_tracking; }
    const QStringList &invalidatedIds() const { return m_invalidatedIds; }

    void setSource(const QByteArray &source);
    void setHeadingIdentity(const QString &documentId, qint64 generation,
                            const QHash<qsizetype, QString> &headings,
                            const QJsonObject &bindings = {});
    QJsonObject externalBinding(const QString &baseRevision, const QString &nameSpace,
                                const QString &value, const QString &nodeIdentity = {});
    EditResult startTracking();
    EditResult stopTracking();
    EditResult apply(const QString &baseRevision, const QString &action,
                     const QString &nodeIdentity, QJsonObject arguments = {});
    QJsonObject snapshot(const QString &path, const QString &nodeIdentity,
                         const QString &scope = QStringLiteral("item"), bool includeAttachmentData = false) const;

private:
    void parseAndReconcile(const QByteArray &source, const QString &preferredSessionId = {},
                           NodeKind preferredKind = NodeKind::Block,
                           const QString &preferredFingerprint = {},
                           qsizetype preferredByte = -1, bool targetDeleted = false,
                           bool preserveSubtree = false);
    QString nextSessionId();
    static QString fingerprintFor(const Node &node);
    static QString matchingKey(const Document &document, int nodeIndex, bool withContext);

    DocumentEngine m_engine;
    Document m_document;
    bool m_tracking = true;
    QString m_sessionPrefix;
    quint64 m_nextIdentity = 0;
    QHash<QString, QStringList> m_labels;
    QStringList m_invalidatedIds;
    // Retired targets remain recorded so an external value cannot be reassigned.
    QHash<QString, QHash<QString, QString>> m_externalBindings;
};

} // namespace Mustermark
