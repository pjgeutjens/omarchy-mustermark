#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace Mustermark {

enum class NodeKind {
    Heading,
    List,
    Item,
    Block,
};

struct Node {
    NodeKind kind = NodeKind::Block;
    QString ref;
    QString id;
    QString text;
    QStringList labels;
    QString marker;
    int level = 0;
    int depth = 0;
    int parent = -1;
    QVector<int> children;
    int startLine = 0;
    int endLine = 0;
    qsizetype startByte = 0;
    qsizetype endByte = 0;
    int metadataLine = -1;
    qsizetype metadataStart = -1;
    qsizetype metadataEnd = -1;
    bool task = false;
    bool checked = false;
    bool ordered = false;
};

struct Diagnostic {
    QString code;
    QString message;
    int line = 0;
};

struct Document {
    QByteArray source;
    QString revision;
    QVector<Node> nodes;
    QVector<Diagnostic> diagnostics;
    QString renderedHtml;
    bool tracked = false;
    int trackingVersion = 0;

    QJsonObject toJson() const;
    int findNode(const QString &identity) const;
};

struct EditResult {
    bool ok = false;
    QByteArray source;
    QString errorCode;
    QString errorMessage;
    QJsonArray edits;
};

class DocumentEngine {
public:
    Document parse(const QByteArray &source) const;
    EditResult track(const QByteArray &source) const;
    EditResult repair(const QByteArray &source) const;
    EditResult untrack(const QByteArray &source) const;
    EditResult apply(const QByteArray &source, const QString &baseRevision,
                     const QString &action, const QString &nodeIdentity,
                     const QJsonObject &arguments = {}) const;

    static QString revisionFor(const QByteArray &source);
    static QString kindName(NodeKind kind);

private:
    EditResult applyEdits(const QByteArray &source, QJsonArray edits) const;
};

} // namespace Mustermark
