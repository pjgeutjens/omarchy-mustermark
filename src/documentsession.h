#pragma once

#include "documentengine.h"

#include <QHash>

namespace Mustermark {

class DocumentSession {
public:
    DocumentSession();

    const Document &document() const { return m_document; }
    bool tracking() const { return m_tracking; }

    void setSource(const QByteArray &source);
    EditResult startTracking();
    EditResult stopTracking();
    EditResult apply(const QString &baseRevision, const QString &action,
                     const QString &nodeIdentity, QJsonObject arguments = {});

private:
    void parseAndReconcile(const QByteArray &source, const QString &preferredSessionId = {},
                           NodeKind preferredKind = NodeKind::Block,
                           const QString &preferredFingerprint = {},
                           qsizetype preferredByte = -1, bool targetDeleted = false);
    QString nextSessionId();
    static QString fingerprintFor(const Node &node);
    static QString matchingKey(const Document &document, int nodeIndex, bool withContext);

    DocumentEngine m_engine;
    Document m_document;
    bool m_tracking = false;
    QString m_sessionPrefix;
    quint64 m_nextIdentity = 0;
    QHash<QString, QStringList> m_labels;
};

} // namespace Mustermark
