#include "documentengine.h"

#include <cmark-gfm-core-extensions.h>
#include <cmark-gfm-extension_api.h>
#include <cmark-gfm.h>

#include <QCryptographicHash>
#include <QDir>
#include <QHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cstdlib>

namespace Mustermark {
namespace {

QString externalIdentity(const Node &node) {
    if (!node.sessionId.isEmpty())
        return node.sessionId;
    if (!node.id.isEmpty())
        return node.id;
    return node.ref;
}

struct Lines {
    QByteArray source;
    QVector<qsizetype> starts;

    explicit Lines(const QByteArray &bytes) : source(bytes), starts{0} {
        for (qsizetype i = 0; i < source.size(); ++i) {
            if (source.at(i) == '\n' && i + 1 < source.size())
                starts.append(i + 1);
        }
    }

    int count() const { return starts.size(); }

    qsizetype start(int oneBasedLine) const {
        const int index = std::clamp(oneBasedLine - 1, 0, static_cast<int>(starts.size()) - 1);
        return starts.at(index);
    }

    qsizetype end(int oneBasedLine) const {
        if (oneBasedLine < starts.size())
            return starts.at(oneBasedLine);
        return source.size();
    }

    QByteArray bytes(int oneBasedLine) const {
        QByteArray value = source.mid(start(oneBasedLine), end(oneBasedLine) - start(oneBasedLine));
        while (value.endsWith('\n') || value.endsWith('\r'))
            value.chop(1);
        return value;
    }
};

struct Metadata {
    bool valid = false;
    QString kind;
    QString id;
    QStringList labels;
};

const QRegularExpression metadataPattern(
    QStringLiteral(R"(^\s*<!--\s*mustermark:(list|item)\s+id=([^\s]+)(?:\s+labels=([^\s]+))?\s*-->\s*$)"));
const QRegularExpression trackingPattern(
    QStringLiteral(R"(^\s*<!--\s*mustermark:tracking\s+version=(\d+)\s*-->\s*$)"));
const QRegularExpression itemPattern(
    QStringLiteral(R"(^(\s*)((?:[-+*])|(?:\d+[.)]))(\s+)(?:\[([ xX])\](\s*))?(.*)$)"));
const QRegularExpression atxHeadingPattern(
    QStringLiteral(R"(^(\s*)(#{1,6})(\s+)(.*?)(\s+#+\s*)?$)"));
const QRegularExpression attachmentPattern(
    QStringLiteral(R"(^(\s*)!\[([^\]\r\n]*)\]\(([^()\s]+)\)\s*$)"));

Metadata parseMetadata(const QByteArray &line) {
    const auto match = metadataPattern.match(QString::fromUtf8(line));
    if (!match.hasMatch())
        return {};
    Metadata result;
    result.valid = true;
    result.kind = match.captured(1);
    result.id = match.captured(2);
    if (!match.captured(3).isEmpty()) {
        result.labels = match.captured(3).split(',', Qt::SkipEmptyParts);
        result.labels.removeDuplicates();
        std::sort(result.labels.begin(), result.labels.end());
    }
    return result;
}

QString metadataLine(const QString &indent, const QString &kind, const QString &id,
                     QStringList labels) {
    labels.removeDuplicates();
    std::sort(labels.begin(), labels.end());
    QString line = indent + QStringLiteral("<!-- mustermark:%1 id=%2").arg(kind, id);
    if (!labels.isEmpty())
        line += QStringLiteral(" labels=%1").arg(labels.join(','));
    return line + QStringLiteral(" -->");
}

QString nodeText(cmark_node *node) {
    QString result;
    std::function<void(cmark_node *)> visit = [&](cmark_node *current) {
        const auto type = cmark_node_get_type(current);
        if (type == CMARK_NODE_TEXT || type == CMARK_NODE_CODE ||
            type == CMARK_NODE_HTML_INLINE || type == CMARK_NODE_HTML_BLOCK) {
            if (const char *literal = cmark_node_get_literal(current))
                result += QString::fromUtf8(literal);
        } else if (type == CMARK_NODE_SOFTBREAK || type == CMARK_NODE_LINEBREAK) {
            result += QLatin1Char(' ');
        }
        for (cmark_node *child = cmark_node_first_child(current); child;
             child = cmark_node_next(child))
            visit(child);
    };
    visit(node);
    return result.simplified();
}

int leadingSpaces(const QByteArray &line) {
    int count = 0;
    while (count < line.size()) {
        if (line.at(count) == ' ')
            ++count;
        else if (line.at(count) == '\t')
            count += 4;
        else
            break;
    }
    return count;
}

QString lineIndent(const QByteArray &line) {
    qsizetype count = 0;
    while (count < line.size() && (line.at(count) == ' ' || line.at(count) == '\t'))
        ++count;
    return QString::fromUtf8(line.left(count));
}

QString enrichedHtml(QString html, const QVector<Node> &nodes) {
    html.replace(QRegularExpression(QStringLiteral(R"(\s*<!-- raw HTML omitted -->\s*)")),
                 QStringLiteral("\n"));
    html.remove(QRegularExpression(QStringLiteral(R"(\sdata-mm-(?:kind|ref|id|fingerprint|level|depth|labels|task|checked)="[^"]*")")));
    const QRegularExpression openingTag(
        QStringLiteral(R"(<(h[1-6]|ul|ol|li|p)\b([^>]*\bdata-sourcepos="(\d+):\d+-(\d+):\d+"[^>]*)>)"));
    QRegularExpressionMatchIterator matches = openingTag.globalMatch(html);
    struct Injection { qsizetype position; QString attributes; };
    QVector<Injection> injections;
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        const QString tag = match.captured(1);
        const int startLine = match.captured(3).toInt();
        const NodeKind wantedKind = tag == QStringLiteral("li")
                                      ? NodeKind::Item
                                      : (tag == QStringLiteral("ul") || tag == QStringLiteral("ol"))
                                            ? NodeKind::List
                                            : tag == QStringLiteral("p") ? NodeKind::Block
                                                                         : NodeKind::Heading;
        const auto node = std::find_if(nodes.cbegin(), nodes.cend(), [&](const Node &candidate) {
            return candidate.kind == wantedKind && candidate.startLine == startLine;
        });
        if (node == nodes.cend())
            continue;
        QString attributes = QStringLiteral(" data-mm-kind=\"") +
                             DocumentEngine::kindName(node->kind).toHtmlEscaped() +
                             QStringLiteral("\" data-mm-ref=\"") + externalIdentity(*node).toHtmlEscaped() +
                             QStringLiteral("\"");
        const QString identity = !node->sessionId.isEmpty() ? node->sessionId : node->id;
        if (!identity.isEmpty())
            attributes += QStringLiteral(" data-mm-id=\"") + identity.toHtmlEscaped() +
                          QStringLiteral("\"");
        if (!node->fingerprint.isEmpty())
            attributes += QStringLiteral(" data-mm-fingerprint=\"") +
                          node->fingerprint.toHtmlEscaped() + QStringLiteral("\"");
        if (node->kind == NodeKind::Heading)
            attributes += QStringLiteral(" data-mm-level=\"") + QString::number(node->level) +
                          QStringLiteral("\"");
        if (node->kind == NodeKind::Item)
            attributes += QStringLiteral(" data-mm-depth=\"") + QString::number(node->depth) +
                          QStringLiteral("\"");
        if (!node->labels.isEmpty())
            attributes += QStringLiteral(" data-mm-labels=\"") +
                          node->labels.join(QLatin1Char(',')).toHtmlEscaped() +
                          QStringLiteral("\"");
        if (node->kind == NodeKind::Item && node->task)
            attributes += QStringLiteral(" data-mm-task=\"true\" data-mm-checked=\"") +
                          (node->checked ? QStringLiteral("true") : QStringLiteral("false")) +
                          QStringLiteral("\"");
        injections.append({match.capturedEnd(2), attributes});
    }
    for (auto injection = injections.crbegin(); injection != injections.crend(); ++injection)
        html.insert(injection->position, injection->attributes);
    return html;
}

QByteArray shiftIndent(const QByteArray &block, int delta, bool *ok) {
    *ok = true;
    const bool hasFinalNewline = block.endsWith('\n');
    QList<QByteArray> lines = block.split('\n');
    if (hasFinalNewline && !lines.isEmpty())
        lines.removeLast();
    for (QByteArray &line : lines) {
        if (line.trimmed().isEmpty())
            continue;
        if (delta > 0) {
            line.prepend(QByteArray(delta, ' '));
        } else {
            int remaining = -delta;
            while (remaining > 0 && !line.isEmpty() && line.at(0) == ' ') {
                line.remove(0, 1);
                --remaining;
            }
            if (remaining != 0) {
                *ok = false;
                return block;
            }
        }
    }
    QByteArray result = lines.join('\n');
    if (hasFinalNewline)
        result += '\n';
    return result;
}

bool validLabel(const QString &label) {
    static const QRegularExpression pattern(QStringLiteral(R"(^[a-z0-9][a-z0-9_.\/-]{0,63}$)"));
    return pattern.match(label).hasMatch() && !label.contains(QStringLiteral("--"));
}

bool validAttachmentPath(const QString &path) {
    if (path.isEmpty() || path.size() > 512 || !QDir::isRelativePath(path) ||
        path.contains(QLatin1Char('\n')) || path.contains(QLatin1Char('\r')) ||
        path.contains(QLatin1Char('(')) || path.contains(QLatin1Char(')')))
        return false;
    const QString clean = QDir::cleanPath(path);
    return clean != QStringLiteral(".") && clean != QStringLiteral("..") &&
           !clean.startsWith(QStringLiteral("../"));
}

QJsonObject diagnosticJson(const Diagnostic &diagnostic) {
    return {{QStringLiteral("code"), diagnostic.code},
            {QStringLiteral("message"), diagnostic.message},
            {QStringLiteral("line"), diagnostic.line}};
}

} // namespace

QString DocumentEngine::revisionFor(const QByteArray &source) {
    return QStringLiteral("sha256:") +
           QString::fromLatin1(QCryptographicHash::hash(source, QCryptographicHash::Sha256).toHex());
}

QString DocumentEngine::kindName(NodeKind kind) {
    switch (kind) {
    case NodeKind::Heading: return QStringLiteral("heading");
    case NodeKind::List: return QStringLiteral("list");
    case NodeKind::Item: return QStringLiteral("item");
    case NodeKind::Block: return QStringLiteral("block");
    }
    return QStringLiteral("block");
}

QString DocumentEngine::enrichHtml(QString html, const QVector<Node> &nodes) {
    return enrichedHtml(std::move(html), nodes);
}

int Document::findNode(const QString &identity) const {
    for (int index = 0; index < nodes.size(); ++index) {
        if (nodes.at(index).sessionId == identity || nodes.at(index).id == identity ||
            (!identity.isEmpty() && nodes.at(index).durableId == identity) ||
            nodes.at(index).ref == identity)
            return index;
    }
    return -1;
}

QJsonObject Document::toJson() const {
    QJsonArray sections;
    QJsonArray unsectionedLists;
    for (int index = 0; index < nodes.size(); ++index) {
        const Node &heading = nodes.at(index);
        if (heading.kind == NodeKind::List && heading.parent < 0)
            unsectionedLists.append(externalIdentity(heading));
        if (heading.kind != NodeKind::Heading)
            continue;
        QJsonArray lists;
        QJsonArray items;
        for (int child : heading.children) {
            const Node &list = nodes.at(child);
            if (list.kind != NodeKind::List)
                continue;
            lists.append(externalIdentity(list));
            for (int item : list.children)
                if (nodes.at(item).kind == NodeKind::Item)
                    items.append(externalIdentity(nodes.at(item)));
        }
        if (lists.isEmpty())
            continue;
        QStringList path;
        for (int ancestor = index; ancestor >= 0; ancestor = nodes.at(ancestor).parent)
            if (nodes.at(ancestor).kind == NodeKind::Heading)
                path.prepend(nodes.at(ancestor).text);
        sections.append(QJsonObject{{QStringLiteral("node"), externalIdentity(heading)},
            {QStringLiteral("durableId"), heading.durableId},
            {QStringLiteral("path"), QJsonArray::fromStringList(path)},
            {QStringLiteral("lists"), lists}, {QStringLiteral("items"), items}});
    }
    QJsonArray nodeArray;
    for (int index = 0; index < nodes.size(); ++index) {
        const Node &node = nodes.at(index);
        QJsonArray childArray;
        for (int child : node.children) {
            const Node &childNode = nodes.at(child);
            childArray.append(externalIdentity(childNode));
        }
        QJsonObject value{
            {QStringLiteral("ref"), externalIdentity(node)},
            {QStringLiteral("sourceRef"), node.ref},
            {QStringLiteral("kind"), DocumentEngine::kindName(node.kind)},
            {QStringLiteral("text"), node.text},
            {QStringLiteral("level"), node.level},
            {QStringLiteral("depth"), node.depth},
            {QStringLiteral("startLine"), node.startLine},
            {QStringLiteral("endLine"), node.endLine},
            {QStringLiteral("startByte"), static_cast<double>(node.startByte)},
            {QStringLiteral("endByte"), static_cast<double>(node.endByte)},
            {QStringLiteral("children"), childArray},
            {QStringLiteral("task"), node.task},
            {QStringLiteral("checked"), node.checked},
            {QStringLiteral("ordered"), node.ordered},
        };
        if (!node.sessionId.isEmpty())
            value.insert(QStringLiteral("id"), node.sessionId);
        else if (!node.id.isEmpty())
            value.insert(QStringLiteral("id"), node.id);
        if (!node.fingerprint.isEmpty())
            value.insert(QStringLiteral("fingerprint"), node.fingerprint);
        if (!node.durableId.isEmpty())
            value.insert(QStringLiteral("durableId"), node.durableId);
        if (node.parent >= 0) {
            const Node &parentNode = nodes.at(node.parent);
            value.insert(QStringLiteral("parent"), externalIdentity(parentNode));
        }
        QJsonArray labels;
        for (const QString &label : node.labels)
            labels.append(label);
        value.insert(QStringLiteral("labels"), labels);
        QJsonArray attachments;
        for (const Attachment &attachment : node.attachments) {
            attachments.append(QJsonObject{
                {QStringLiteral("path"), attachment.path},
                {QStringLiteral("alt"), attachment.alt},
                {QStringLiteral("line"), attachment.line},
            });
        }
        value.insert(QStringLiteral("attachments"), attachments);
        nodeArray.append(value);
    }

    QJsonArray diagnosticArray;
    for (const Diagnostic &diagnostic : diagnostics)
        diagnosticArray.append(diagnosticJson(diagnostic));
    QJsonArray invalidatedArray;
    for (const QString &identity : invalidatedIds)
        invalidatedArray.append(identity);

    return {
        {QStringLiteral("apiVersion"), QStringLiteral("0.2")},
        {QStringLiteral("sectionSchemaVersion"), 1},
        {QStringLiteral("documentId"), documentId},
        {QStringLiteral("durableHeadingIdentityVersion"), documentId.isEmpty() ? 0 : 1},
        {QStringLiteral("durableTaskIdentityVersion"), documentId.isEmpty() ? 0 : 1},
        {QStringLiteral("identityGeneration"), identityGeneration},
        {QStringLiteral("sections"), sections},
        {QStringLiteral("unsectionedLists"), unsectionedLists},
        {QStringLiteral("revision"), revision},
        {QStringLiteral("tracked"), tracked},
        {QStringLiteral("trackingVersion"), trackingVersion},
        {QStringLiteral("nodes"), nodeArray},
        {QStringLiteral("diagnostics"), diagnosticArray},
        {QStringLiteral("invalidatedIds"), invalidatedArray},
    };
}

Document DocumentEngine::parse(const QByteArray &source) const {
    Document result;
    result.source = source;
    result.revision = revisionFor(source);
    const Lines lines(source);

    for (int line = 1; line <= lines.count(); ++line) {
        const auto match = trackingPattern.match(QString::fromUtf8(lines.bytes(line)));
        if (match.hasMatch()) {
            result.tracked = true;
            result.trackingVersion = match.captured(1).toInt();
            break;
        }
    }

    cmark_gfm_core_extensions_ensure_registered();
    cmark_parser *parser = cmark_parser_new(CMARK_OPT_SOURCEPOS | CMARK_OPT_VALIDATE_UTF8);
    for (const char *name : {"table", "strikethrough", "autolink", "tasklist"}) {
        if (cmark_syntax_extension *extension = cmark_find_syntax_extension(name))
            cmark_parser_attach_syntax_extension(parser, extension);
    }
    cmark_parser_feed(parser, source.constData(), static_cast<size_t>(source.size()));
    cmark_node *root = cmark_parser_finish(parser);

    QHash<quintptr, int> indices;
    QVector<int> headingStack;
    QSet<QString> seenIds;
    int ordinal = 0;

    std::function<void(cmark_node *)> visit = [&](cmark_node *current) {
        const cmark_node_type type = cmark_node_get_type(current);
        const bool isHeading = type == CMARK_NODE_HEADING;
        const bool isList = type == CMARK_NODE_LIST;
        const bool isItem = type == CMARK_NODE_ITEM;
        const bool isTopBlock = !isHeading && !isList && !isItem &&
            cmark_node_parent(current) == root && type != CMARK_NODE_DOCUMENT;

        int currentIndex = -1;
        if (isHeading || isList || isItem || isTopBlock) {
            Node node;
            node.kind = isHeading ? NodeKind::Heading
                                  : isList ? NodeKind::List
                                           : isItem ? NodeKind::Item : NodeKind::Block;
            node.startLine = std::max(1, cmark_node_get_start_line(current));
            node.endLine = std::max(node.startLine, cmark_node_get_end_line(current));
            node.startByte = lines.start(node.startLine);
            node.endByte = lines.end(node.endLine);
            node.ref = QStringLiteral("r:%1:%2")
                           .arg(result.revision.mid(7, 12))
                           .arg(++ordinal);
            node.text = nodeText(current);

            if (isHeading) {
                node.level = cmark_node_get_heading_level(current);
                while (!headingStack.isEmpty() &&
                       result.nodes.at(headingStack.last()).level >= node.level)
                    headingStack.removeLast();
                node.parent = headingStack.isEmpty() ? -1 : headingStack.last();
            } else if (isList || isItem) {
                cmark_node *parent = cmark_node_parent(current);
                const auto found = indices.constFind(reinterpret_cast<quintptr>(parent));
                node.parent = found == indices.constEnd()
                                  ? (headingStack.isEmpty() ? -1 : headingStack.last())
                                  : found.value();
                node.depth = node.parent >= 0 ? result.nodes.at(node.parent).depth + 1 : 0;
                if (isList) {
                    node.ordered = cmark_node_get_list_type(current) == CMARK_ORDERED_LIST;
                    node.marker = node.ordered ? QStringLiteral("1.") : QStringLiteral("-");
                    const int metadataCandidate = node.startLine - 1;
                    if (metadataCandidate >= 1) {
                        const Metadata metadata = parseMetadata(lines.bytes(metadataCandidate));
                        if (metadata.valid && metadata.kind == QStringLiteral("list")) {
                            node.id = metadata.id;
                            node.labels = metadata.labels;
                            node.metadataLine = metadataCandidate;
                            node.metadataStart = lines.start(metadataCandidate);
                            node.metadataEnd = lines.end(metadataCandidate);
                            node.startByte = node.metadataStart;
                        }
                    }
                } else {
                    const QByteArray firstLine = lines.bytes(node.startLine);
                    const auto match = itemPattern.match(QString::fromUtf8(firstLine));
                    if (match.hasMatch()) {
                        node.marker = match.captured(2);
                        node.task = !match.captured(4).isNull();
                        node.checked = node.task && match.captured(4).toLower() == QStringLiteral("x");
                        node.text = match.captured(6).trimmed();
                    }
                    const int itemIndent = leadingSpaces(firstLine);
                    int structuralEnd = node.endLine;
                    for (int line = node.startLine + 1; line <= node.endLine; ++line) {
                        const QByteArray candidate = lines.bytes(line);
                        if (candidate.trimmed().isEmpty())
                            continue;
                        if (leadingSpaces(candidate) <= itemIndent) {
                            structuralEnd = line - 1;
                            break;
                        }
                    }
                    while (structuralEnd > node.startLine &&
                           lines.bytes(structuralEnd).trimmed().isEmpty())
                        --structuralEnd;
                    node.endLine = structuralEnd;
                    node.endByte = lines.end(node.endLine);
                    if (node.startLine + 1 <= node.endLine) {
                        const Metadata metadata = parseMetadata(lines.bytes(node.startLine + 1));
                        if (metadata.valid && metadata.kind == QStringLiteral("item")) {
                            node.id = metadata.id;
                            node.labels = metadata.labels;
                            node.metadataLine = node.startLine + 1;
                            node.metadataStart = lines.start(node.metadataLine);
                            node.metadataEnd = lines.end(node.metadataLine);
                        }
                    }
                    const int attachmentIndent = match.hasMatch()
                                                     ? match.capturedEnd(3)
                                                     : itemIndent + 2;
                    for (int line = node.startLine + 1; line <= node.endLine; ++line) {
                        const QByteArray candidate = lines.bytes(line);
                        if (leadingSpaces(candidate) != attachmentIndent)
                            continue;
                        const auto attachment = attachmentPattern.match(QString::fromUtf8(candidate));
                        if (!attachment.hasMatch() || !validAttachmentPath(attachment.captured(3)))
                            continue;
                        node.attachments.append({
                            QDir::cleanPath(attachment.captured(3)), attachment.captured(2), line,
                            lines.start(line), lines.end(line),
                        });
                    }
                }
            } else {
                node.parent = headingStack.isEmpty() ? -1 : headingStack.last();
                node.text = lines.bytes(node.startLine).trimmed().left(120);
            }

            currentIndex = result.nodes.size();
            result.nodes.append(node);
            indices.insert(reinterpret_cast<quintptr>(current), currentIndex);
            if (node.parent >= 0)
                result.nodes[node.parent].children.append(currentIndex);
            if (isHeading)
                headingStack.append(currentIndex);

            if (!node.id.isEmpty()) {
                if (seenIds.contains(node.id)) {
                    result.diagnostics.append({QStringLiteral("duplicate_id"),
                        QStringLiteral("The ID %1 appears more than once.").arg(node.id),
                        node.metadataLine});
                }
                seenIds.insert(node.id);
            } else if (result.tracked && (isList || isItem)) {
                result.diagnostics.append({QStringLiteral("missing_id"),
                    QStringLiteral("A tracked %1 has no ID.").arg(kindName(node.kind)),
                    node.startLine});
            }
        }

        for (cmark_node *child = cmark_node_first_child(current); child;
             child = cmark_node_next(child))
            visit(child);
    };
    visit(root);

    for (int index = 0; index < result.nodes.size(); ++index) {
        Node &node = result.nodes[index];
        if (node.kind == NodeKind::List) {
            int itemCount = 0;
            for (int child : node.children) {
                if (result.nodes.at(child).kind == NodeKind::Item)
                    ++itemCount;
            }
            node.text = QStringLiteral("%1 list · %2 %3")
                            .arg(node.ordered ? QStringLiteral("Ordered") : QStringLiteral("Bullet"))
                            .arg(itemCount)
                            .arg(itemCount == 1 ? QStringLiteral("item") : QStringLiteral("items"));
        }
    }

    for (int index = 0; index < result.nodes.size(); ++index) {
        Node &heading = result.nodes[index];
        if (heading.kind != NodeKind::Heading)
            continue;
        int subtreeEnd = lines.count();
        for (int next = index + 1; next < result.nodes.size(); ++next) {
            const Node &candidate = result.nodes.at(next);
            if (candidate.kind == NodeKind::Heading && candidate.level <= heading.level) {
                subtreeEnd = candidate.startLine - 1;
                break;
            }
        }
        heading.endLine = std::max(heading.startLine, subtreeEnd);
        heading.endByte = lines.end(heading.endLine);
    }

    if (char *html = cmark_render_html(root, CMARK_OPT_SOURCEPOS,
                                       cmark_parser_get_syntax_extensions(parser))) {
        result.renderedHtml = enrichHtml(QString::fromUtf8(html), result.nodes);
        std::free(html);
    }

    cmark_node_free(root);
    cmark_parser_free(parser);
    return result;
}

EditResult DocumentEngine::applyEdits(const QByteArray &source, QJsonArray edits) const {
    struct Replacement { qsizetype start; qsizetype end; QByteArray text; int order; };
    QVector<Replacement> replacements;
    replacements.reserve(edits.size());
    int order = 0;
    for (const QJsonValue &value : edits) {
        const QJsonObject edit = value.toObject();
        const qsizetype start = static_cast<qsizetype>(edit.value(QStringLiteral("startByte")).toDouble(-1));
        const qsizetype end = static_cast<qsizetype>(edit.value(QStringLiteral("endByte")).toDouble(-1));
        if (start < 0 || end < start || end > source.size())
            return {false, source, QStringLiteral("invalid_edit"),
                    QStringLiteral("An edit range is outside the document."), {}};
        replacements.append({start, end, edit.value(QStringLiteral("text")).toString().toUtf8(), order++});
    }
    std::stable_sort(replacements.begin(), replacements.end(),
                     [](const Replacement &a, const Replacement &b) {
                         if (a.start != b.start) return a.start > b.start;
                         return a.order > b.order;
                     });
    qsizetype previousStart = source.size() + 1;
    for (const Replacement &replacement : replacements) {
        if (replacement.end > previousStart && replacement.start != replacement.end)
            return {false, source, QStringLiteral("overlapping_edits"),
                    QStringLiteral("The requested edits overlap."), {}};
        previousStart = replacement.start;
    }
    QByteArray changed = source;
    for (const Replacement &replacement : replacements)
        changed.replace(replacement.start, replacement.end - replacement.start, replacement.text);
    return {true, changed, {}, {}, edits};
}

QByteArray DocumentEngine::renumberOrderedLists(const QByteArray &source, int forcedOrdinal,
                                                int forcedStart) const {
    const Document document = parse(source);
    const Lines lines(source);
    QJsonArray edits;
    int orderedOrdinal = 0;
    for (const Node &list : document.nodes) {
        if (list.kind != NodeKind::List || !list.ordered)
            continue;
        QVector<const Node *> items;
        for (const int childIndex : list.children) {
            const Node &child = document.nodes.at(childIndex);
            if (child.kind == NodeKind::Item)
                items.append(&child);
        }
        if (items.isEmpty())
            continue;
        const auto firstMatch = itemPattern.match(QString::fromUtf8(lines.bytes(items.first()->startLine)));
        if (!firstMatch.hasMatch())
            continue;
        const QString firstMarker = firstMatch.captured(2);
        bool validStart = false;
        int start = firstMarker.chopped(1).toInt(&validStart);
        if (!validStart)
            continue;
        if (orderedOrdinal == forcedOrdinal)
            start = forcedStart;
        ++orderedOrdinal;
        for (int position = 0; position < items.size(); ++position) {
            const Node &item = *items.at(position);
            const auto match = itemPattern.match(QString::fromUtf8(lines.bytes(item.startLine)));
            if (!match.hasMatch())
                continue;
            const QString marker = match.captured(2);
            const QString number = marker.chopped(1);
            const QString expected = QString::number(start + position);
            if (number == expected)
                continue;
            const qsizetype markerStart = lines.start(item.startLine) + match.capturedStart(2);
            edits.append(QJsonObject{
                {QStringLiteral("startByte"), static_cast<double>(markerStart)},
                {QStringLiteral("endByte"), static_cast<double>(markerStart + number.toUtf8().size())},
                {QStringLiteral("text"), expected},
            });
        }
    }
    const EditResult result = applyEdits(source, edits);
    return result.ok ? result.source : source;
}

EditResult DocumentEngine::untrack(const QByteArray &source) const {
    const Lines lines(source);
    QJsonArray edits;
    for (int line = 1; line <= lines.count(); ++line) {
        const QString value = QString::fromUtf8(lines.bytes(line));
        const bool isTracking = trackingPattern.match(value).hasMatch();
        if (metadataPattern.match(value).hasMatch() || isTracking) {
            const int finalLine = isTracking && line < lines.count() && lines.bytes(line + 1).trimmed().isEmpty()
                                      ? line + 1 : line;
            edits.append(QJsonObject{{QStringLiteral("startByte"), static_cast<double>(lines.start(line))},
                          {QStringLiteral("endByte"), static_cast<double>(lines.end(finalLine))},
                          {QStringLiteral("text"), QString()}});
            line = finalLine;
        }
    }
    EditResult result = applyEdits(source, edits);
    if (result.ok)
        result.source.replace("\n\n\n", "\n\n");
    return result;
}

EditResult DocumentEngine::apply(const QByteArray &source, const QString &baseRevision,
                                 const QString &action, const QString &nodeIdentity,
                                 const QJsonObject &arguments) const {
    const QString actualRevision = revisionFor(source);
    if (baseRevision != actualRevision)
        return {false, source, QStringLiteral("stale_revision"),
                QStringLiteral("The document changed after this command was prepared."), {}};

    const Document document = parse(source);
    const int nodeIndex = document.findNode(nodeIdentity);
    if (nodeIndex < 0)
        return {false, source, QStringLiteral("unknown_id"),
                QStringLiteral("The selected node no longer exists."), {}};
    const Node &node = document.nodes.at(nodeIndex);
    const Lines lines(source);
    QJsonArray edits;
    int affectedOrderedOrdinal = -1;
    int affectedOrderedStart = 1;
    int affectedListIndex = -1;
    if (node.kind == NodeKind::Item && node.parent >= 0 &&
        document.nodes.at(node.parent).kind == NodeKind::List &&
        document.nodes.at(node.parent).ordered)
        affectedListIndex = node.parent;
    else if (action == QStringLiteral("item_add") && node.kind == NodeKind::List && node.ordered)
        affectedListIndex = nodeIndex;
    const bool preserveOrderedStart = action == QStringLiteral("item_add") ||
        action == QStringLiteral("move_before") || action == QStringLiteral("move_after") ||
        action == QStringLiteral("delete");
    if (affectedListIndex >= 0 && preserveOrderedStart) {
        int ordinal = 0;
        for (int index = 0; index < document.nodes.size(); ++index) {
            const Node &candidate = document.nodes.at(index);
            if (candidate.kind != NodeKind::List || !candidate.ordered)
                continue;
            if (index == affectedListIndex) {
                affectedOrderedOrdinal = ordinal;
                break;
            }
            ++ordinal;
        }
        const Node &list = document.nodes.at(affectedListIndex);
        int itemCount = 0;
        for (const int childIndex : list.children) {
            const Node &child = document.nodes.at(childIndex);
            if (child.kind != NodeKind::Item)
                continue;
            if (itemCount == 0) {
                bool validStart = false;
                affectedOrderedStart = child.marker.chopped(1).toInt(&validStart);
                if (!validStart)
                    affectedOrderedOrdinal = -1;
            }
            ++itemCount;
        }
        if (action == QStringLiteral("delete") && itemCount <= 1)
            affectedOrderedOrdinal = -1;
    }

    if (action == QStringLiteral("section_add")) {
        if (node.kind != NodeKind::Heading)
            return {false, source, QStringLiteral("invalid_action"),
                    QStringLiteral("New sections must be placed after a heading."), {}};
        const QString text = arguments.value(QStringLiteral("text")).toString().trimmed();
        if (text.isEmpty() || text.size() > 240 || text.contains(QLatin1Char('\n')) ||
            text.contains(QLatin1Char('\r')))
            return {false, source, QStringLiteral("invalid_text"),
                    QStringLiteral("Section titles must be one non-empty line of at most 240 characters."), {}};
        const QByteArray newline = source.contains("\r\n") ? QByteArray("\r\n")
                                                            : QByteArray("\n");
        const qsizetype insertionOffset = node.endByte;
        const QByteArray before = source.left(insertionOffset);
        const QByteArray after = source.mid(insertionOffset);
        QByteArray insertion;
        if (!before.isEmpty() && !before.endsWith(newline + newline))
            insertion += before.endsWith(newline) ? newline : newline + newline;
        insertion += QByteArray(node.level, '#') + " " + text.toUtf8() + newline;
        if (!after.isEmpty() && !after.startsWith(newline))
            insertion += newline;
        else if (after.isEmpty())
            insertion += newline;
        edits.append(QJsonObject{{QStringLiteral("startByte"), static_cast<double>(insertionOffset)},
                      {QStringLiteral("endByte"), static_cast<double>(insertionOffset)},
                      {QStringLiteral("text"), QString::fromUtf8(insertion)}});
    } else if (action == QStringLiteral("list_add")) {
        if (node.kind != NodeKind::Heading)
            return {false, source, QStringLiteral("invalid_action"),
                    QStringLiteral("New lists must be placed inside a section heading."), {}};
        QString text = arguments.value(QStringLiteral("text")).toString();
        text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        text = text.trimmed();
        if (text.isEmpty() || text.size() > 65536 || text.contains(QLatin1Char('\r')))
            return {false, source, QStringLiteral("invalid_text"),
                    QStringLiteral("List item text must contain at most 65536 characters."), {}};
        const QByteArray newline = source.contains("\r\n") ? QByteArray("\r\n")
                                                            : QByteArray("\n");
        const qsizetype insertionOffset = lines.end(node.startLine);
        const QByteArray after = source.mid(insertionOffset);
        const QByteArray marker = arguments.value(QStringLiteral("ordered")).toBool()
                                      ? QByteArray("1.") : QByteArray("-");
        const QByteArray task = arguments.value(QStringLiteral("task")).toBool()
                                    ? QByteArray("[ ] ") : QByteArray();
        QStringList textLines = text.split(QLatin1Char('\n'));
        QByteArray formatted = textLines.takeFirst().toUtf8();
        const QByteArray continuationIndent(marker.size() + 1 + task.size(), ' ');
        for (const QString &line : textLines)
            formatted += newline + continuationIndent + line.toUtf8();
        QByteArray insertion = newline + marker + " " + task + formatted + newline;
        if (!after.isEmpty() && !after.startsWith(newline))
            insertion += newline;
        edits.append(QJsonObject{{QStringLiteral("startByte"), static_cast<double>(insertionOffset)},
                      {QStringLiteral("endByte"), static_cast<double>(insertionOffset)},
                      {QStringLiteral("text"), QString::fromUtf8(insertion)}});
    } else if (action == QStringLiteral("item_add")) {
        if (node.kind != NodeKind::List && node.kind != NodeKind::Item)
            return {false, source, QStringLiteral("invalid_action"),
                    QStringLiteral("New items must be placed in a list or after another item."), {}};
        QString text = arguments.value(QStringLiteral("text")).toString();
        text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        text = text.trimmed();
        if (text.isEmpty() || text.size() > 65536 || text.contains(QLatin1Char('\r')))
            return {false, source, QStringLiteral("invalid_text"),
                    QStringLiteral("List item text must contain at most 65536 characters."), {}};

        int anchorIndex = nodeIndex;
        int listIndex = node.kind == NodeKind::List ? nodeIndex : node.parent;
        if (listIndex < 0 || document.nodes.at(listIndex).kind != NodeKind::List)
            return {false, source, QStringLiteral("invalid_action"),
                    QStringLiteral("The selected item is not inside a list."), {}};
        const Node &list = document.nodes.at(listIndex);
        if (node.kind == NodeKind::List) {
            for (int child : list.children) {
                if (document.nodes.at(child).kind == NodeKind::Item)
                    anchorIndex = child;
            }
            if (anchorIndex == nodeIndex)
                return {false, source, QStringLiteral("invalid_action"),
                        QStringLiteral("An empty Markdown list has no safe insertion anchor."), {}};
        }
        const Node &anchor = document.nodes.at(anchorIndex);
        const QByteArray anchorLine = lines.bytes(anchor.startLine);
        const auto anchorMatch = itemPattern.match(QString::fromUtf8(anchorLine));
        if (!anchorMatch.hasMatch())
            return {false, source, QStringLiteral("unsafe_rewrite"),
                    QStringLiteral("The list marker could not be preserved."), {}};
        const QByteArray newline = source.contains("\r\n") ? QByteArray("\r\n")
                                                            : QByteArray("\n");
        QByteArray marker = anchorMatch.captured(2).toUtf8();
        if (list.ordered)
            marker = "1.";
        const QByteArray task = arguments.value(QStringLiteral("task")).toBool()
                                    ? QByteArray("[ ] ") : QByteArray();
        QByteArray insertion;
        if (anchor.endByte > 0 && source.at(anchor.endByte - 1) != '\n')
            insertion += newline;
        const QByteArray prefix = anchorMatch.captured(1).toUtf8() + marker + " " + task;
        QStringList textLines = text.split(QLatin1Char('\n'));
        QByteArray formatted = textLines.takeFirst().toUtf8();
        const QByteArray continuationIndent(prefix.size(), ' ');
        for (const QString &line : textLines)
            formatted += newline + continuationIndent + line.toUtf8();
        insertion += prefix + formatted + newline;
        edits.append(QJsonObject{{QStringLiteral("startByte"), static_cast<double>(anchor.endByte)},
                      {QStringLiteral("endByte"), static_cast<double>(anchor.endByte)},
                      {QStringLiteral("text"), QString::fromUtf8(insertion)}});
    } else if (action == QStringLiteral("toggle_task") || action == QStringLiteral("task_set")) {
        if (node.kind != NodeKind::Item || !node.task)
            return {false, source, QStringLiteral("invalid_action"),
                    QStringLiteral("Only task-list items can change task state."), {}};
        if (action == QStringLiteral("task_set") &&
            (!arguments.contains(QStringLiteral("checked")) ||
             !arguments.value(QStringLiteral("checked")).isBool()))
            return {false, source, QStringLiteral("invalid_checked"),
                    QStringLiteral("task_set requires a Boolean checked value."), {}};
        QByteArray line = lines.bytes(node.startLine);
        const auto match = itemPattern.match(QString::fromUtf8(line));
        if (!match.hasMatch() || match.capturedStart(4) < 0)
            return {false, source, QStringLiteral("unsafe_rewrite"),
                    QStringLiteral("The task marker could not be located safely."), {}};
        const qsizetype offset = lines.start(node.startLine) + match.capturedStart(4);
        const bool checked = action == QStringLiteral("task_set")
                                 ? arguments.value(QStringLiteral("checked")).toBool()
                                 : !node.checked;
        edits.append(QJsonObject{{QStringLiteral("startByte"), static_cast<double>(offset)},
                      {QStringLiteral("endByte"), static_cast<double>(offset + 1)},
                      {QStringLiteral("text"), checked ? QStringLiteral("x") : QStringLiteral(" ")}});
    } else if (action == QStringLiteral("attachment_add")) {
        if (node.kind != NodeKind::Item)
            return {false, source, QStringLiteral("invalid_action"),
                    QStringLiteral("Images can only be associated with list items."), {}};
        const QString path = QDir::cleanPath(arguments.value(QStringLiteral("path")).toString());
        QString alt = arguments.value(QStringLiteral("alt")).toString().simplified();
        alt.replace(QLatin1Char('['), QLatin1Char('('));
        alt.replace(QLatin1Char(']'), QLatin1Char(')'));
        if (!validAttachmentPath(path))
            return {false, source, QStringLiteral("invalid_attachment"),
                    QStringLiteral("Attachment paths must be safe document-relative paths."), {}};
        if (alt.size() > 120)
            alt = alt.left(120);
        if (alt.isEmpty())
            alt = QStringLiteral("image");
        if (std::any_of(node.attachments.cbegin(), node.attachments.cend(),
                        [&](const Attachment &item) { return item.path == path; }))
            return {false, source, QStringLiteral("invalid_attachment"),
                    QStringLiteral("That image is already associated with this item."), {}};
        const QByteArray firstLineWithEnding = source.mid(
            lines.start(node.startLine), lines.end(node.startLine) - lines.start(node.startLine));
        const QByteArray newline = firstLineWithEnding.endsWith("\r\n") ? QByteArray("\r\n")
                                                                          : QByteArray("\n");
        QByteArray insertion;
        if (!firstLineWithEnding.endsWith('\n'))
            insertion += newline;
        const QByteArray firstLine = lines.bytes(node.startLine);
        const auto itemMatch = itemPattern.match(QString::fromUtf8(firstLine));
        const int continuationIndent = itemMatch.hasMatch()
                                           ? itemMatch.capturedEnd(3)
                                           : leadingSpaces(firstLine) + 2;
        insertion += QByteArray(continuationIndent, ' ') + "![" + alt.toUtf8() + "](" +
                     path.toUtf8() + ")" + newline;
        edits.append(QJsonObject{{QStringLiteral("startByte"), static_cast<double>(lines.end(node.startLine))},
                      {QStringLiteral("endByte"), static_cast<double>(lines.end(node.startLine))},
                      {QStringLiteral("text"), QString::fromUtf8(insertion)}});
    } else if (action == QStringLiteral("attachment_remove")) {
        if (node.kind != NodeKind::Item)
            return {false, source, QStringLiteral("invalid_action"),
                    QStringLiteral("Images can only be removed from list items."), {}};
        const QString path = QDir::cleanPath(arguments.value(QStringLiteral("path")).toString());
        const auto attachment = std::find_if(
            node.attachments.cbegin(), node.attachments.cend(),
            [&](const Attachment &item) { return item.path == path; });
        if (attachment == node.attachments.cend())
            return {false, source, QStringLiteral("unknown_attachment"),
                    QStringLiteral("That image is no longer associated with the selected item."), {}};
        edits.append(QJsonObject{{QStringLiteral("startByte"), static_cast<double>(attachment->startByte)},
                      {QStringLiteral("endByte"), static_cast<double>(attachment->endByte)},
                      {QStringLiteral("text"), QString()}});
    } else if (action == QStringLiteral("label_add") || action == QStringLiteral("label_remove")) {
        if (node.id.isEmpty() || node.metadataLine < 1)
            return {false, source, QStringLiteral("tracking_required"),
                    QStringLiteral("Enable tracking before changing labels."), {}};
        const QString label = arguments.value(QStringLiteral("label")).toString().trimmed().toLower();
        if (!validLabel(label))
            return {false, source, QStringLiteral("invalid_label"),
                    QStringLiteral("Labels must be lowercase slugs of at most 64 characters."), {}};
        QStringList labels = node.labels;
        if (action == QStringLiteral("label_add") && !labels.contains(label))
            labels.append(label);
        if (action == QStringLiteral("label_remove"))
            labels.removeAll(label);
        QByteArray replacement = metadataLine(lineIndent(lines.bytes(node.metadataLine)),
                                              kindName(node.kind), node.id, labels).toUtf8();
        if (node.metadataEnd > node.metadataStart && source.at(node.metadataEnd - 1) == '\n')
            replacement += '\n';
        edits.append(QJsonObject{{QStringLiteral("startByte"), static_cast<double>(node.metadataStart)},
                      {QStringLiteral("endByte"), static_cast<double>(node.metadataEnd)},
                      {QStringLiteral("text"), QString::fromUtf8(replacement)}});
    } else if (action == QStringLiteral("delete")) {
        edits.append(QJsonObject{{QStringLiteral("startByte"), static_cast<double>(node.startByte)},
                      {QStringLiteral("endByte"), static_cast<double>(node.endByte)},
                      {QStringLiteral("text"), QString()}});
    } else if (action == QStringLiteral("move_before") || action == QStringLiteral("move_after")) {
        const QString targetIdentity = arguments.value(QStringLiteral("target")).toString();
        const int targetIndex = document.findNode(targetIdentity);
        if (targetIndex < 0)
            return {false, source, QStringLiteral("unknown_id"),
                    QStringLiteral("The destination no longer exists."), {}};
        const Node &target = document.nodes.at(targetIndex);
        if (nodeIndex == targetIndex ||
            (target.startByte >= node.startByte && target.endByte <= node.endByte))
            return {false, source, QStringLiteral("invalid_destination"),
                    QStringLiteral("A node cannot be moved into itself."), {}};
        const bool nodeIsBlock = node.kind == NodeKind::List || node.kind == NodeKind::Block;
        const bool targetIsBlock = target.kind == NodeKind::List || target.kind == NodeKind::Block;
        const bool nodeNeedsBlockSpacing = node.kind != NodeKind::Item;
        const bool targetNeedsBlockSpacing = target.kind != NodeKind::Item;
        const auto isSectionBlock = [&](const Node &candidate) {
            return (candidate.kind == NodeKind::List || candidate.kind == NodeKind::Block) &&
                   (candidate.parent < 0 ||
                    document.nodes.at(candidate.parent).kind == NodeKind::Heading);
        };
        const bool blockMove = nodeIsBlock && targetIsBlock &&
                               (node.parent == target.parent ||
                                (isSectionBlock(node) && isSectionBlock(target)));
        if (node.kind != target.kind && !blockMove)
            return {false, source, QStringLiteral("invalid_destination"),
                    QStringLiteral("Move destinations must have the same structural kind."), {}};
        if (nodeIsBlock && targetIsBlock && !blockMove)
            return {false, source, QStringLiteral("invalid_destination"),
                    QStringLiteral("Blocks can move between sections only at the document level."), {}};
        if (node.kind == NodeKind::Heading && node.level != target.level)
            return {false, source, QStringLiteral("unsafe_rewrite"),
                    QStringLiteral("Move headings beside another heading of the same level."), {}};

        const auto trailingBlankEnd = [&](qsizetype end) {
            while (end < source.size()) {
                qsizetype lineEnd = source.indexOf('\n', end);
                if (lineEnd < 0)
                    lineEnd = source.size();
                const qsizetype next = lineEnd < source.size() ? lineEnd + 1 : lineEnd;
                if (!source.mid(end, next - end).trimmed().isEmpty())
                    break;
                end = next;
            }
            return end;
        };
        const qsizetype moveEnd = nodeNeedsBlockSpacing ? trailingBlankEnd(node.endByte)
                                                        : node.endByte;
        QByteArray block = source.mid(node.startByte, moveEnd - node.startByte);
        if (node.kind == NodeKind::Item) {
            const int delta = leadingSpaces(lines.bytes(target.startLine)) -
                              leadingSpaces(lines.bytes(node.startLine));
            bool shifted = false;
            block = shiftIndent(block, delta, &shifted);
            if (!shifted)
                return {false, source, QStringLiteral("unsafe_rewrite"),
                        QStringLiteral("The item indentation cannot be adapted safely."), {}};
        }
        const qsizetype targetEnd = targetNeedsBlockSpacing ? trailingBlankEnd(target.endByte)
                                                            : target.endByte;
        qsizetype insertion = action == QStringLiteral("move_before") ? target.startByte : targetEnd;
        if (nodeNeedsBlockSpacing) {
            const QByteArray newline = source.contains("\r\n") ? QByteArray("\r\n")
                                                                : QByteArray("\n");
            const QByteArray before = source.left(insertion);
            const bool blankBefore = before.endsWith(newline + newline);
            if (insertion > 0 && !blankBefore)
                block.prepend(newline);
            if (!block.endsWith(newline + newline))
                block.append(newline);
        }
        edits.append(QJsonObject{{QStringLiteral("startByte"), static_cast<double>(node.startByte)},
                      {QStringLiteral("endByte"), static_cast<double>(moveEnd)},
                      {QStringLiteral("text"), QString()}});
        edits.append(QJsonObject{{QStringLiteral("startByte"), static_cast<double>(insertion)},
                      {QStringLiteral("endByte"), static_cast<double>(insertion)},
                      {QStringLiteral("text"), QString::fromUtf8(block)}});
    } else if (action == QStringLiteral("indent") || action == QStringLiteral("outdent") ||
               ((action == QStringLiteral("promote") || action == QStringLiteral("demote")) &&
                node.kind == NodeKind::Item)) {
        if (node.kind != NodeKind::Item)
            return {false, source, QStringLiteral("invalid_action"),
                    QStringLiteral("Only list items can be indented or outdented."), {}};
        const QString scope = arguments.value(QStringLiteral("scope"))
                                  .toString(QStringLiteral("subtree"));
        if (scope != QStringLiteral("subtree"))
            return {false, source, QStringLiteral("invalid_scope"),
                    QStringLiteral("List items always move together with their child items."), {}};
        const bool demoting = action == QStringLiteral("indent") || action == QStringLiteral("demote");
        bool shifted = false;
        const QByteArray original = source.mid(node.startByte, node.endByte - node.startByte);
        const QByteArray replacement = shiftIndent(original, demoting ? 4 : -4, &shifted);
        if (!shifted)
            return {false, source, QStringLiteral("unsafe_rewrite"),
                    demoting
                        ? QStringLiteral("The selected item cannot be demoted one level safely.")
                        : QStringLiteral("The selected item cannot be promoted one level safely."), {}};
        edits.append(QJsonObject{{QStringLiteral("startByte"), static_cast<double>(node.startByte)},
                      {QStringLiteral("endByte"), static_cast<double>(node.endByte)},
                      {QStringLiteral("text"), QString::fromUtf8(replacement)}});
        if (!demoting && node.parent >= 0) {
            const Node &parentList = document.nodes.at(node.parent);
            const int itemCount = std::count_if(
                parentList.children.cbegin(), parentList.children.cend(), [&](const int childIndex) {
                    return document.nodes.at(childIndex).kind == NodeKind::Item;
                });
            if (itemCount == 1 && parentList.metadataLine > 0) {
                edits.append(QJsonObject{
                    {QStringLiteral("startByte"), static_cast<double>(parentList.metadataStart)},
                    {QStringLiteral("endByte"), static_cast<double>(parentList.metadataEnd)},
                    {QStringLiteral("text"), QString()},
                });
            }
        }
    } else if (action == QStringLiteral("promote") || action == QStringLiteral("demote")) {
        if (node.kind != NodeKind::Heading)
            return {false, source, QStringLiteral("invalid_action"),
                    QStringLiteral("Only headings and list items can be promoted or demoted."), {}};
        const QString scope = arguments.value(QStringLiteral("scope")).toString(QStringLiteral("self"));
        if (scope != QStringLiteral("self") && scope != QStringLiteral("subtree"))
            return {false, source, QStringLiteral("invalid_scope"),
                    QStringLiteral("Scope must be self or subtree."), {}};
        if (scope == QStringLiteral("self") && action == QStringLiteral("demote")) {
            int nearestDescendantLevel = 7;
            for (const Node &candidate : document.nodes) {
                if (candidate.kind == NodeKind::Heading &&
                    candidate.startLine > node.startLine && candidate.startLine <= node.endLine)
                    nearestDescendantLevel = std::min(nearestDescendantLevel, candidate.level);
            }
            if (node.level + 1 >= nearestDescendantLevel)
                return {false, source, QStringLiteral("unsafe_rewrite"),
                        QStringLiteral("A heading must stay above the headings in its section. Change the whole branch instead."), {}};
        }
        const qsizetype editEnd = scope == QStringLiteral("self")
                                      ? lines.end(node.startLine) : node.endByte;
        QByteArray block = source.mid(node.startByte, editEnd - node.startByte);
        const bool finalNewline = block.endsWith('\n');
        QList<QByteArray> blockLines = block.split('\n');
        if (finalNewline) blockLines.removeLast();
        bool changed = false;
        for (QByteArray &line : blockLines) {
            const auto match = atxHeadingPattern.match(QString::fromUtf8(line));
            if (!match.hasMatch())
                continue;
            QString hashes = match.captured(2);
            if ((action == QStringLiteral("promote") && hashes.size() == 1) ||
                (action == QStringLiteral("demote") && hashes.size() == 6))
                return {false, source, QStringLiteral("unsafe_rewrite"),
                        QStringLiteral("A heading would leave the supported level range."), {}};
            hashes = action == QStringLiteral("promote") ? hashes.chopped(1)
                                                          : hashes + QLatin1Char('#');
            line = (match.captured(1) + hashes + match.captured(3) + match.captured(4) +
                    match.captured(5)).toUtf8();
            changed = true;
        }
        if (!changed)
            return {false, source, QStringLiteral("unsafe_rewrite"),
                    QStringLiteral("Setext headings must be changed in source mode."), {}};
        QByteArray replacement = blockLines.join('\n');
        if (finalNewline) replacement += '\n';
        edits.append(QJsonObject{{QStringLiteral("startByte"), static_cast<double>(node.startByte)},
                      {QStringLiteral("endByte"), static_cast<double>(editEnd)},
                      {QStringLiteral("text"), QString::fromUtf8(replacement)}});
    } else if (action == QStringLiteral("set_heading_level")) {
        if (node.kind != NodeKind::Heading)
            return {false, source, QStringLiteral("invalid_action"),
                    QStringLiteral("Only headings have a heading level."), {}};
        const int requestedLevel = arguments.value(QStringLiteral("level")).toInt();
        if (requestedLevel < 1 || requestedLevel > 6)
            return {false, source, QStringLiteral("invalid_level"),
                    QStringLiteral("Heading levels run from 1 through 6."), {}};
        int nearestDescendantLevel = 7;
        for (const Node &candidate : document.nodes) {
            if (candidate.kind == NodeKind::Heading &&
                candidate.startLine > node.startLine && candidate.startLine <= node.endLine)
                nearestDescendantLevel = std::min(nearestDescendantLevel, candidate.level);
        }
        if (requestedLevel >= nearestDescendantLevel)
            return {false, source, QStringLiteral("unsafe_rewrite"),
                    QStringLiteral("A heading must stay above the headings in its section. Change the whole branch instead."), {}};
        QByteArray firstLine = lines.bytes(node.startLine);
        const auto match = atxHeadingPattern.match(QString::fromUtf8(firstLine));
        if (!match.hasMatch())
            return {false, source, QStringLiteral("unsafe_rewrite"),
                    QStringLiteral("Setext headings must be changed in source mode."), {}};
        QString replacement = match.captured(1) + QString(requestedLevel, QLatin1Char('#')) +
                              match.captured(3) + match.captured(4) + match.captured(5);
        if (lines.end(node.startLine) > lines.start(node.startLine) &&
            source.at(lines.end(node.startLine) - 1) == '\n')
            replacement += QLatin1Char('\n');
        edits.append(QJsonObject{{QStringLiteral("startByte"), static_cast<double>(lines.start(node.startLine))},
                                  {QStringLiteral("endByte"), static_cast<double>(lines.end(node.startLine))},
                                  {QStringLiteral("text"), replacement}});
    } else if (action == QStringLiteral("replace")) {
        QString markdown = arguments.value(QStringLiteral("markdown")).toString();
        if (markdown.contains(QChar::Null) || markdown.toUtf8().size() > 1024 * 1024)
            return {false, source, QStringLiteral("invalid_text"),
                    QStringLiteral("Inline Markdown must be smaller than 1 MiB and contain no null bytes."), {}};
        markdown.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        if (source.contains("\r\n"))
            markdown.replace(QLatin1Char('\n'), QStringLiteral("\r\n"));
        const QByteArray replacement = markdown.toUtf8();
        const Document candidate = parse(replacement);
        int candidateIndex = -1;
        if (node.kind == NodeKind::Item) {
            for (int index = 0; index < candidate.nodes.size(); ++index) {
                const Node &possible = candidate.nodes.at(index);
                if (possible.kind != NodeKind::Item || possible.parent < 0)
                    continue;
                const Node &parent = candidate.nodes.at(possible.parent);
                if (parent.kind != NodeKind::List || parent.parent >= 0)
                    continue;
                int rootItems = 0;
                for (int child : parent.children) {
                    if (candidate.nodes.at(child).kind == NodeKind::Item)
                        ++rootItems;
                }
                if (rootItems == 1) {
                    candidateIndex = index;
                    break;
                }
            }
        } else {
            for (int index = 0; index < candidate.nodes.size(); ++index) {
                const Node &possible = candidate.nodes.at(index);
                if (possible.kind == node.kind && possible.parent < 0) {
                    candidateIndex = index;
                    break;
                }
            }
        }
        if (candidateIndex < 0)
            return {false, source, QStringLiteral("invalid_structure"),
                    QStringLiteral("The edited Markdown must remain one structure of the same type."), {}};
        const Node &replacementNode = candidate.nodes.at(candidateIndex);
        if (node.kind == NodeKind::Heading && replacementNode.level != node.level)
            return {false, source, QStringLiteral("invalid_structure"),
                    QStringLiteral("Use the heading controls to change its level."), {}};
        qsizetype consumed = replacementNode.endByte;
        if (node.kind == NodeKind::Item && replacementNode.parent >= 0)
            consumed = candidate.nodes.at(replacementNode.parent).endByte;
        if (!replacement.mid(consumed).trimmed().isEmpty())
            return {false, source, QStringLiteral("invalid_structure"),
                    QStringLiteral("The edited Markdown would create more than one peer structure."), {}};
        edits.append(QJsonObject{{QStringLiteral("startByte"), static_cast<double>(node.startByte)},
                                  {QStringLiteral("endByte"), static_cast<double>(node.endByte)},
                                  {QStringLiteral("text"), markdown}});
    } else if (action == QStringLiteral("edit")) {
        const QString text = arguments.value(QStringLiteral("text")).toString();
        if (text.contains(QLatin1Char('\n')))
            return {false, source, QStringLiteral("invalid_text"),
                    QStringLiteral("Structural text edits must fit on one line."), {}};
        QByteArray firstLine = lines.bytes(node.startLine);
        QString replacement;
        if (node.kind == NodeKind::Item) {
            const auto match = itemPattern.match(QString::fromUtf8(firstLine));
            if (!match.hasMatch())
                return {false, source, QStringLiteral("unsafe_rewrite"),
                        QStringLiteral("The list marker could not be preserved."), {}};
            replacement = match.captured(1) + match.captured(2) + match.captured(3);
            if (!match.captured(4).isNull())
                replacement += QStringLiteral("[") + match.captured(4) + QStringLiteral("]") + match.captured(5);
            replacement += text;
        } else if (node.kind == NodeKind::Heading) {
            const auto match = atxHeadingPattern.match(QString::fromUtf8(firstLine));
            if (!match.hasMatch())
                return {false, source, QStringLiteral("unsafe_rewrite"),
                        QStringLiteral("Setext headings must be edited in source mode."), {}};
            replacement = match.captured(1) + match.captured(2) + match.captured(3) + text;
        } else {
            return {false, source, QStringLiteral("invalid_action"),
                    QStringLiteral("Edit headings and list items through this command."), {}};
        }
        if (lines.end(node.startLine) > lines.start(node.startLine) &&
            source.at(lines.end(node.startLine) - 1) == '\n')
            replacement += QLatin1Char('\n');
        edits.append(QJsonObject{{QStringLiteral("startByte"), static_cast<double>(lines.start(node.startLine))},
                      {QStringLiteral("endByte"), static_cast<double>(lines.end(node.startLine))},
                      {QStringLiteral("text"), replacement}});
    } else {
        return {false, source, QStringLiteral("unknown_action"),
                QStringLiteral("The requested structural action is not supported."), {}};
    }

    EditResult result = applyEdits(source, edits);
    if (!result.ok)
        return result;
    const bool renumberItems = action == QStringLiteral("item_add") ||
        (node.kind == NodeKind::Item &&
         (action == QStringLiteral("delete") || action == QStringLiteral("move_before") ||
          action == QStringLiteral("move_after") || action == QStringLiteral("promote") ||
          action == QStringLiteral("demote") || action == QStringLiteral("indent") ||
          action == QStringLiteral("outdent")));
    if (renumberItems) {
        const QByteArray renumbered = renumberOrderedLists(
            result.source, affectedOrderedOrdinal, affectedOrderedStart);
        if (renumbered != result.source) {
            result.source = renumbered;
            qsizetype commonPrefix = 0;
            while (commonPrefix < source.size() && commonPrefix < result.source.size() &&
                   source.at(commonPrefix) == result.source.at(commonPrefix))
                ++commonPrefix;
            qsizetype commonSuffix = 0;
            while (commonSuffix < source.size() - commonPrefix &&
                   commonSuffix < result.source.size() - commonPrefix &&
                   source.at(source.size() - commonSuffix - 1) ==
                       result.source.at(result.source.size() - commonSuffix - 1))
                ++commonSuffix;
            result.edits = QJsonArray{QJsonObject{
                {QStringLiteral("startByte"), static_cast<double>(commonPrefix)},
                {QStringLiteral("endByte"), static_cast<double>(source.size() - commonSuffix)},
                {QStringLiteral("text"), QString::fromUtf8(result.source.mid(
                     commonPrefix, result.source.size() - commonPrefix - commonSuffix))},
            }};
        }
    }
    const Document reparsed = parse(result.source);
    if (action == QStringLiteral("move_before") || action == QStringLiteral("move_after")) {
        const auto countKind = [](const Document &value, NodeKind kind) {
            return std::count_if(value.nodes.cbegin(), value.nodes.cend(),
                                 [kind](const Node &candidate) { return candidate.kind == kind; });
        };
        for (const NodeKind kind : {NodeKind::Heading, NodeKind::List,
                                    NodeKind::Item, NodeKind::Block}) {
            if (countKind(document, kind) != countKind(reparsed, kind))
                return {false, source, QStringLiteral("unsafe_rewrite"),
                        QStringLiteral("The move would change the Markdown structure."), {}};
        }
    }
    if (!node.id.isEmpty() && action != QStringLiteral("delete") &&
        reparsed.findNode(node.id) < 0)
        return {false, source, QStringLiteral("unsafe_rewrite"),
                QStringLiteral("The edit would detach the node from its persistent identity."), {}};
    return result;
}

} // namespace Mustermark
