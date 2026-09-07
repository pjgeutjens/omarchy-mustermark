#include "filedocument.h"
#include "documentsession.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QSet>
#include <QUuid>

namespace Mustermark {
namespace {
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString canonical(const QString &path) { return QFileInfo(path).canonicalFilePath(); }
QString sidecar(const QString &path) { return path + QStringLiteral(".mustermark.json"); }
FileResult fail(const QString &error) { return {false, {}, error}; }

QJsonArray headings(const Document &doc, bool legacy = false) {
    QJsonArray result;
    for (const Node &node : doc.nodes) {
        if (node.kind != NodeKind::Heading && (legacy || node.kind != NodeKind::Item)) continue;
        QStringList path;
        for (int parent = node.parent; parent >= 0; parent = doc.nodes.at(parent).parent)
            if (doc.nodes.at(parent).kind == NodeKind::Heading || (!legacy && doc.nodes.at(parent).kind == NodeKind::Item))
                path.prepend(doc.nodes.at(parent).text);
        result.append(QJsonObject{{"start", qint64(node.startByte)}, {"text", node.text},
            {"kind", DocumentEngine::kindName(node.kind)},
            {"context", QJsonArray::fromStringList(path)}, {"id", node.durableId}});
    }
    return result;
}

QJsonArray records(const QJsonObject &meta) {
    auto result = meta.value(meta.value("version").toInt() == 1 ? "headings" : "nodes").toArray();
    if (meta.value("version").toInt() == 1) {
        for (int i = 0; i < result.size(); ++i) {
            auto node = result[i].toObject(); node.insert("kind", "heading"); result[i] = node;
        }
    }
    return result;
}

bool valid(const QJsonObject &meta) {
    const int version = meta.value("version").toInt();
    if ((version != 1 && version != 2) ||
        QUuid(meta.value("documentId").toString()).isNull() ||
        meta.value("generation").toInteger() < 1 || !meta.value(version == 1 ? "headings" : "nodes").isArray() ||
        !meta.value("retired").isArray()) return false;
    const QByteArray source = QByteArray::fromBase64(meta.value("source").toString().toLatin1());
    if (DocumentEngine::revisionFor(source) != meta.value("revision").toString()) return false;
    const auto parsed = headings(DocumentEngine().parse(source), version == 1);
    const auto stored = records(meta);
    if (parsed.size() != stored.size()) return false;
    QSet<QString> ids;
    for (const auto &value : meta.value("retired").toArray()) {
        if (QUuid(value.toString()).isNull() || ids.contains(value.toString())) return false;
        ids.insert(value.toString());
    }
    for (int i = 0; i < parsed.size(); ++i) {
        const auto entry = stored[i].toObject();
        const auto expected = parsed[i].toObject();
        const auto id = entry.value("id").toString();
        if (QUuid(id).isNull() || ids.contains(id) || entry.value("start") != expected.value("start") ||
            entry.value("text") != expected.value("text") ||
            entry.value("kind") != expected.value("kind") ||
            entry.value("context") != expected.value("context")) return false;
        ids.insert(id);
    }
    if (version == 2) {
        if (!meta.value("bindings").isObject()) return false;
        const auto namespaces = meta.value("bindings").toObject();
        for (const auto &values : namespaces) {
            if (!values.isObject()) return false;
            for (const auto &target : values.toObject())
                if (!ids.contains(target.toString())) return false;
        }
    }
    return true;
}

FileResult loadMetadata(const QString &path, const QByteArray &source, QJsonObject &meta) {
    if (QFileInfo(sidecar(path)).isSymLink() || QFileInfo(sidecar(path)).size() > 16 * 1024 * 1024)
        return fail("unsafe_identity_file");
    auto file = FileDocument::read(sidecar(path), 16 * 1024 * 1024);
    if (!file.ok) return file;
    meta = QJsonDocument::fromJson(file.source).object();
    if (!valid(meta)) return fail("invalid_identity_file");
    if (meta.contains("prepared")) {
        const auto prepared = meta.value("prepared").toObject();
        if (!valid(prepared) || prepared.contains("prepared") ||
            prepared.value("documentId") != meta.value("documentId") ||
            prepared.value("generation").toInteger() != meta.value("generation").toInteger() + 1)
            return fail("invalid_identity_transaction");
        const auto revision = DocumentEngine::revisionFor(source);
        if (revision == prepared.value("revision").toString()) meta = prepared;
        else if (revision == meta.value("revision").toString()) meta.remove("prepared");
        else return fail("identity_recovery_conflict");
        return FileDocument::writeRaw(sidecar(path), QJsonDocument(meta).toJson());
    }
    return {true, source, {}};
}

QJsonObject reconcile(const QJsonObject &old, const Document &doc, bool controlled) {
    QJsonArray next = headings(doc);
    const QJsonArray previous = records(old);
    QSet<QString> live, used;
    for (const auto &entry : previous) live.insert(entry.toObject().value("id").toString());
    const bool same = old.value("revision").toString() == doc.revision;
    for (int i = 0; i < next.size(); ++i) {
        auto entry = next[i].toObject();
        QString id;
        const QString hint = entry.value("id").toString();
        if (controlled && live.contains(hint) && !used.contains(hint)) id = hint;
        else if (same) {
            for (const auto &value : previous) {
                const auto candidate = value.toObject();
                if (candidate.value("start") == entry.value("start") && candidate.value("kind") == entry.value("kind")) {
                    id = candidate.value("id").toString(); break;
                }
            }
        }
        else if (!controlled) {
            // Require uniqueness on both sides. Context can distinguish equal labels.
            for (bool context : {true, false}) {
                int oldCount = 0, newCount = 0;
                QString candidate;
                auto matches = [&](const QJsonObject &other) {
                    return other.value("kind") == entry.value("kind") && other.value("text") == entry.value("text") &&
                        (!context || other.value("context") == entry.value("context"));
                };
                for (const auto &value : previous) if (matches(value.toObject())) {
                    ++oldCount; candidate = value.toObject().value("id").toString();
                }
                for (const auto &value : next) if (matches(value.toObject())) ++newCount;
                if (oldCount == 1 && newCount == 1 && !used.contains(candidate)) { id = candidate; break; }
            }
        }
        if (id.isEmpty()) id = uuid();
        used.insert(id); entry.insert("id", id); next[i] = entry;
    }
    QJsonArray retired = old.value("retired").toArray();
    for (const auto &id : live) if (!used.contains(id)) retired.append(id);
    return {{"version", 2}, {"documentId", old.value("documentId").toString(uuid())},
        {"generation", old.value("generation").toInteger() + 1},
        {"revision", doc.revision}, {"source", QString::fromLatin1(doc.source.toBase64())},
        {"nodes", next}, {"retired", retired},
        {"bindings", controlled ? doc.externalBindings : old.value("bindings").toObject()}};
}

void adopt(DocumentSession &session, const QJsonObject &meta) {
    QHash<qsizetype, QString> map;
    for (const auto &value : records(meta)) {
        auto entry = value.toObject();
        map.insert(entry.value("start").toInteger(), entry.value("id").toString());
    }
    session.setHeadingIdentity(meta.value("documentId").toString(),
                               meta.value("generation").toInteger(), map, meta.value("bindings").toObject());
}
}

FileResult FileDocument::readLinked(const QString &input, DocumentSession &session, bool initialize) {
    const QString path = canonical(input);
    if (path.isEmpty() || !QFileInfo(path).isFile() || QFileInfo(path).size() > 2 * 1024 * 1024)
        return fail("invalid_linked_source");
    const bool exists = QFileInfo::exists(sidecar(path));
    if (!exists && !initialize) return fail("identity_file_missing");
    if (initialize && !path.endsWith(".md", Qt::CaseInsensitive)
        && !path.endsWith(".markdown", Qt::CaseInsensitive)) return fail("markdown_suffix_required");
    QLockFile lock(sidecar(path) + ".lock");
    if (!lock.tryLock(1000)) return fail("identity_locked");
    auto file = read(path, 2 * 1024 * 1024);
    if (!file.ok) return file;
    QJsonObject meta;
    if (QFileInfo::exists(sidecar(path))) {
        auto loaded = loadMetadata(path, file.source, meta);
        if (!loaded.ok) return loaded;
    }
    session.setSource(file.source);
    if (meta.isEmpty() || meta.value("version").toInt() != 2 || meta.value("revision").toString() != session.document().revision) {
        meta = reconcile(meta, session.document(), false);
        const auto check = read(path, 2 * 1024 * 1024);
        if (!check.ok || check.source != file.source) return fail("stale_revision");
        auto saved = writeRaw(sidecar(path), QJsonDocument(meta).toJson());
        if (!saved.ok) return saved;
    }
    adopt(session, meta);
    return file;
}

FileResult FileDocument::writeAtomic(const QString &input, const QByteArray &source,
                                    const QString &expectedRevision, DocumentSession *session) {
    const QString path = canonical(input);
    if (path.isEmpty() || !QFileInfo::exists(sidecar(path))) {
        if (session && !session->document().documentId.isEmpty()) return fail("identity_file_missing");
        return writeRaw(input, source, expectedRevision);
    }
    if (source.size() > 2 * 1024 * 1024) return fail("linked_source_too_large");
    QLockFile lock(sidecar(path) + ".lock");
    if (!lock.tryLock(1000)) return fail("identity_locked");
    const auto file = read(path, 2 * 1024 * 1024);
    if (!file.ok) return file;
    if (expectedRevision.isEmpty() || DocumentEngine::revisionFor(file.source) != expectedRevision)
        return fail("stale_revision");
    QJsonObject old;
    auto loaded = loadMetadata(path, file.source, old);
    if (!loaded.ok) return loaded;
    if (old.value("revision").toString() != expectedRevision) return fail("identity_refresh_required");
    if (session && (session->document().documentId != old.value("documentId").toString() ||
        session->document().identityGeneration != old.value("generation").toInteger()))
        return fail("stale_identity_generation");
    const Document document = session ? session->document() : DocumentEngine().parse(source);
    if (document.source != source) return fail("identity_source_mismatch");
    auto next = reconcile(old, document, session != nullptr);
    auto prepared = old; prepared.insert("prepared", next);
    auto saved = writeRaw(sidecar(path), QJsonDocument(prepared).toJson());
    if (!saved.ok) return saved;
    auto written = source == file.source ? file : writeRaw(path, source, expectedRevision);
    if (!written.ok) return written;
    saved = writeRaw(sidecar(path), QJsonDocument(next).toJson());
    if (!saved.ok) return saved;
    if (session) adopt(*session, next);
    return written;
}
} // namespace Mustermark
