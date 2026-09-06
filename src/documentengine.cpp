#include "documentengine.h"

#include <cmark-gfm-core-extensions.h>
#include <cmark-gfm-extension_api.h>
#include <cmark-gfm.h>

#include <QCryptographicHash>
#include <QHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>

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

QString newId(const QString &prefix) {
    return prefix + QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
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
        QStringLiteral(R"(<(h[1-6]|ul|ol|li)\b([^>]*\bdata-sourcepos="(\d+):\d+-(\d+):\d+"[^>]*)>)"));
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
                                            ? NodeKind::List : NodeKind::Heading;
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

int frontMatterEnd(const Lines &lines) {
    if (lines.count() == 0 || lines.bytes(1).trimmed() != "---")
        return 0;
    for (int line = 2; line <= lines.count(); ++line) {
        const QByteArray value = lines.bytes(line).trimmed();
        if (value == "---" || value == "...")
            return line;
    }
    return 0;
}

bool validLabel(const QString &label) {
    static const QRegularExpression pattern(QStringLiteral(R"(^[a-z0-9][a-z0-9_.\/-]{0,63}$)"));
    return pattern.match(label).hasMatch() && !label.contains(QStringLiteral("--"));
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
            nodes.at(index).ref == identity)
            return index;
    }
    return -1;
}

QJsonObject Document::toJson() const {
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
        if (node.parent >= 0) {
            const Node &parentNode = nodes.at(node.parent);
            value.insert(QStringLiteral("parent"), externalIdentity(parentNode));
        }
        QJsonArray labels;
        for (const QString &label : node.labels)
            labels.append(label);
        value.insert(QStringLiteral("labels"), labels);
        nodeArray.append(value);
    }

    QJsonArray diagnosticArray;
    for (const Diagnostic &diagnostic : diagnostics)
        diagnosticArray.append(diagnosticJson(diagnostic));

    return {
        {QStringLiteral("apiVersion"), QStringLiteral("0.1")},
        {QStringLiteral("revision"), revision},
        {QStringLiteral("tracked"), tracked},
        {QStringLiteral("trackingVersion"), trackingVersion},
        {QStringLiteral("nodes"), nodeArray},
        {QStringLiteral("diagnostics"), diagnosticArray},
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

EditResult DocumentEngine::track(const QByteArray &source) const {
    const Document document = parse(source);
    const Lines lines(source);
    QJsonArray edits;

    if (!document.tracked) {
        const int frontMatter = frontMatterEnd(lines);
        const qsizetype offset = frontMatter > 0 ? lines.end(frontMatter) : 0;
        const QString marker = QStringLiteral("<!-- mustermark:tracking version=1 -->\n\n");
        edits.append(QJsonObject{{QStringLiteral("startByte"), static_cast<double>(offset)},
                                 {QStringLiteral("endByte"), static_cast<double>(offset)},
                                 {QStringLiteral("text"), marker}});
    }

    for (const Node &node : document.nodes) {
        if ((node.kind != NodeKind::List && node.kind != NodeKind::Item) || !node.id.isEmpty())
            continue;
        if (node.kind == NodeKind::List) {
            const QByteArray firstLine = lines.bytes(node.startLine);
            const QString comment = metadataLine(lineIndent(firstLine), QStringLiteral("list"),
                                                 newId(QStringLiteral("lst_")), {});
            edits.append(QJsonObject{{QStringLiteral("startByte"), static_cast<double>(lines.start(node.startLine))},
                          {QStringLiteral("endByte"), static_cast<double>(lines.start(node.startLine))},
                          {QStringLiteral("text"), comment + QLatin1Char('\n')}});
        } else {
            const QByteArray firstLine = lines.bytes(node.startLine);
            const QString indent = QString(leadingSpaces(firstLine) + 2, QLatin1Char(' '));
            const QString comment = metadataLine(indent, QStringLiteral("item"),
                                                 newId(QStringLiteral("itm_")), {});
            const qsizetype offset = lines.end(node.startLine);
            const bool endedWithNewline = offset > lines.start(node.startLine) && source.at(offset - 1) == '\n';
            const QString insertion = endedWithNewline ? comment + QLatin1Char('\n')
                                                       : QLatin1Char('\n') + comment;
            edits.append(QJsonObject{{QStringLiteral("startByte"), static_cast<double>(offset)},
                          {QStringLiteral("endByte"), static_cast<double>(offset)},
                          {QStringLiteral("text"), insertion}});
        }
    }

    return applyEdits(source, edits);
}

EditResult DocumentEngine::repair(const QByteArray &source) const {
    const Document document = parse(source);
    QSet<QString> seen;
    QJsonArray edits;
    const Lines lines(source);
    for (const Node &node : document.nodes) {
        if (node.id.isEmpty() || node.metadataLine < 1)
            continue;
        if (!seen.contains(node.id)) {
            seen.insert(node.id);
            continue;
        }
        const QString replacement = metadataLine(
            lineIndent(lines.bytes(node.metadataLine)), kindName(node.kind),
            newId(node.kind == NodeKind::List ? QStringLiteral("lst_") : QStringLiteral("itm_")),
            node.labels);
        QByteArray text = replacement.toUtf8();
        if (node.metadataEnd > node.metadataStart && source.at(node.metadataEnd - 1) == '\n')
            text += '\n';
        edits.append(QJsonObject{{QStringLiteral("startByte"), static_cast<double>(node.metadataStart)},
                      {QStringLiteral("endByte"), static_cast<double>(node.metadataEnd)},
                      {QStringLiteral("text"), QString::fromUtf8(text)}});
    }
    const EditResult deduplicated = applyEdits(source, edits);
    if (!deduplicated.ok)
        return deduplicated;
    return track(deduplicated.source);
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

    if (action == QStringLiteral("toggle_task") || action == QStringLiteral("task_set")) {
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
        if (nodeIndex == targetIndex || (target.startByte >= node.startByte && target.endByte <= node.endByte))
            return {false, source, QStringLiteral("invalid_destination"),
                    QStringLiteral("A node cannot be moved into itself."), {}};
        if (node.kind != target.kind)
            return {false, source, QStringLiteral("invalid_destination"),
                    QStringLiteral("Move destinations must have the same structural kind."), {}};
        if (node.kind == NodeKind::Heading && node.level != target.level)
            return {false, source, QStringLiteral("unsafe_rewrite"),
                    QStringLiteral("Move headings beside another heading of the same level."), {}};

        QByteArray block = source.mid(node.startByte, node.endByte - node.startByte);
        if (node.kind == NodeKind::Item) {
            const int delta = leadingSpaces(lines.bytes(target.startLine)) -
                              leadingSpaces(lines.bytes(node.startLine));
            bool shifted = false;
            block = shiftIndent(block, delta, &shifted);
            if (!shifted)
                return {false, source, QStringLiteral("unsafe_rewrite"),
                        QStringLiteral("The item indentation cannot be adapted safely."), {}};
        }
        qsizetype insertion = action == QStringLiteral("move_before") ? target.startByte : target.endByte;
        edits.append(QJsonObject{{QStringLiteral("startByte"), static_cast<double>(node.startByte)},
                      {QStringLiteral("endByte"), static_cast<double>(node.endByte)},
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
    if (document.tracked) {
        const EditResult completedTracking = track(result.source);
        if (!completedTracking.ok)
            return completedTracking;
        result.source = completedTracking.source;
        for (const QJsonValue &edit : completedTracking.edits)
            result.edits.append(edit);
    }
    const Document reparsed = parse(result.source);
    if (!node.id.isEmpty() && action != QStringLiteral("delete") &&
        reparsed.findNode(node.id) < 0)
        return {false, source, QStringLiteral("unsafe_rewrite"),
                QStringLiteral("The edit would detach the node from its persistent identity."), {}};
    return result;
}

} // namespace Mustermark
