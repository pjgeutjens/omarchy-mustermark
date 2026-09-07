#include "documentengine.h"
#include "documentsession.h"
#include "filedocument.h"

#include <QFile>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>

using namespace Mustermark;

class DocumentEngineTest : public QObject {
    Q_OBJECT

private slots:
    void linkedHeadingRecovery();
    void sectionContract();
    void externalIdentityContract();
    void unchangedParseIsByteExact();
    void parsesHeadingAndNestedLists();
    void parsesLegacyIdentityMetadata();
    void sessionIdentityLeavesFrontMatterUntouched();
    void taskToggleTouchesOneByte();
    void taskSetIsIdempotent();
    void associatesImagesWithItemSubtree();
    void addsSectionsListsAndItems();
    void addsMultilineItems();
    void rendersSafeEnrichedHtml();
    void labelsStayInSession();
    void movesWholeItemSubtree();
    void renumbersOrderedListsAfterStructuralChanges();
    void keepsTrailingParagraphOutsideMovedItem();
    void movesWholeListBlock();
    void movesBlockAcrossHeadingSections();
    void movesHeadingBranchesWithoutMergingBoundaries();
    void replacesStructureMarkdown();
    void setsOnlySelectedHeadingLevel();
    void promotesOnlySelectedHeading();
    void changesWholeHeadingBranchLevel();
    void keepsHeadingAboveItsDescendants();
    void promotesAndDemotesListItems();
    void preservesSessionIdentityAcrossListLevelChanges();
    void rejectsHeadingBranchBeyondLevelSix();
    void rejectsStaleRevision();
    void reportsDuplicateLegacyIds();
    void untrackRemovesOnlyMetadata();
    void atomicWriteRejectsExternalChange();
    void sessionTrackingLeavesSourceClean();
    void sessionMoveKeepsTargetIdentity();
    void sessionIdentitySurvivesSourceTyping();
    void sessionInvalidatesUnrelatedReplacement();
    void sessionMoveKeepsDuplicateSubtreeIdentities();
    void snapshotsItemsSectionsAndAttachments();
    void parsesAndMutatesThousandLines();
};

void DocumentEngineTest::linkedHeadingRecovery() {
    QTemporaryDir dir;
    const QString path = dir.filePath("tasks.muster.md");
    const QByteArray source("# First\n\n- task\n\n# Second\n\n- other\n");
    QVERIFY(FileDocument::writeAtomic(path, source).ok);
    DocumentSession session;
    QVERIFY(!FileDocument::readLinked(path, session).ok);
    QVERIFY(FileDocument::readLinked(path, session, true).ok);
    const QString documentId = session.document().documentId;
    const QString headingId = session.document().nodes[0].durableId;
    QVERIFY(!headingId.isEmpty());
    const QByteArray metadata = FileDocument::read(path + ".mustermark.json").source;
    DocumentSession restarted;
    QVERIFY(FileDocument::readLinked(path, restarted).ok);
    QCOMPARE(restarted.document().nodes[0].durableId, headingId);
    QCOMPARE(restarted.document().documentId, documentId);
    QCOMPARE(FileDocument::read(path + ".mustermark.json").source, metadata);
    const auto before = session.document().revision;
    QVERIFY(session.apply(before, "edit", session.document().nodes[0].sessionId,
                          {{"text", "Renamed"}}).ok);
    QVERIFY(FileDocument::writeAtomic(path, session.document().source, before, &session).ok);
    const auto changedMetadata = FileDocument::read(path + ".mustermark.json").source;
    QVERIFY(FileDocument::readLinked(path, restarted).ok);
    QCOMPARE(restarted.document().nodes[0].durableId, headingId);
    QVERIFY(!FileDocument::writeAtomic(path, source, before, &session).ok);

    // Simulate a crash after source replacement but before metadata finalization.
    auto prepared = QJsonDocument::fromJson(metadata).object();
    prepared.insert("prepared", QJsonDocument::fromJson(changedMetadata).object());
    QVERIFY(FileDocument::writeRaw(path + ".mustermark.json", QJsonDocument(prepared).toJson()).ok);
    QVERIFY(FileDocument::readLinked(path, restarted).ok);
    QCOMPARE(restarted.document().nodes[0].durableId, headingId);
    // Old bytes roll back preparation; a third version pauses recovery.
    QVERIFY(FileDocument::writeRaw(path + ".mustermark.json", QJsonDocument(prepared).toJson()).ok);
    QVERIFY(FileDocument::writeRaw(path, "# Third version\n").ok);
    QVERIFY(!FileDocument::readLinked(path, restarted).ok);
    QVERIFY(FileDocument::writeRaw(path, source).ok);
    QVERIFY(FileDocument::readLinked(path, restarted).ok);
    QCOMPARE(restarted.document().nodes[0].durableId, headingId);
    QVERIFY(FileDocument::writeRaw(path, "# External rename\n\n- task\n").ok);
    QVERIFY(FileDocument::readLinked(path, restarted).ok);
    QVERIFY(restarted.document().nodes[0].durableId != headingId);
    QVERIFY(QFile::remove(path + ".mustermark.json"));
    QVERIFY(!FileDocument::writeAtomic(path, restarted.document().source,
                                      restarted.document().revision, &restarted).ok);
    const auto duplicates = dir.filePath("duplicates.muster.md");
    const QByteArray repeated("# Same\n\n- task\n\n# Same\n\n- task\n");
    QVERIFY(FileDocument::writeRaw(duplicates, repeated).ok);
    QVERIFY(FileDocument::readLinked(duplicates, restarted, true).ok);
    const auto firstId = restarted.document().nodes[0].durableId;
    DocumentSession duplicateRestart;
    QVERIFY(FileDocument::readLinked(duplicates, duplicateRestart).ok);
    QCOMPARE(duplicateRestart.document().nodes[0].durableId, firstId);
    QVERIFY(FileDocument::writeRaw(duplicates, repeated + "\nExternal change.\n").ok);
    QVERIFY(FileDocument::readLinked(duplicates, duplicateRestart).ok);
    QVERIFY(duplicateRestart.document().nodes[0].durableId != firstId);
    auto corrupt = QJsonDocument::fromJson(FileDocument::read(duplicates + ".mustermark.json").source).object();
    auto entries = corrupt.value("nodes").toArray();
    auto second = entries[1].toObject();
    second.insert("id", entries[0].toObject().value("id")); entries[1] = second;
    corrupt.insert("nodes", entries);
    QVERIFY(FileDocument::writeRaw(duplicates + ".mustermark.json", QJsonDocument(corrupt).toJson()).ok);
    QVERIFY(!FileDocument::readLinked(duplicates, duplicateRestart).ok);
}

void DocumentEngineTest::sectionContract() {
    DocumentSession session;
    session.setSource("- outside\n\n# Parent\n\n## Same\n\nA paragraph.\n\n"
                      "- [ ] first\n  - [x] nested\n\nAnother paragraph.\n\n"
                      "1. ordered\n\n#### Same\n\n- child section\n\n"
                      "# Other\n\n###### Same\n\n- last\n");
    const auto payload = session.document().toJson();
    const auto sections = payload.value("sections").toArray();
    QCOMPARE(sections.size(), 3);
    QCOMPARE(payload.value("unsectionedLists").toArray().size(), 1);
    const auto first = sections[0].toObject();
    QCOMPARE(first.value("path").toArray(), QJsonArray({"Parent", "Same"}));
    QCOMPARE(first.value("lists").toArray().size(), 2);
    QCOMPARE(first.value("items").toArray().size(), 2);
    QCOMPARE(sections[1].toObject().value("path").toArray(),
             QJsonArray({"Parent", "Same", "Same"}));
    QTemporaryDir directory;
    const auto snapshot = session.snapshot(directory.filePath("test.muster.md"),
                                           first.value("node").toString(), "section");
    QVERIFY(snapshot.value("ok").toBool());
    QCOMPARE(snapshot.value("units").toArray().size(), 2);
}

void DocumentEngineTest::externalIdentityContract() {
    DocumentSession session;
    const QByteArray source("# Tasks\n\n- [ ] same\n- [ ] same\n- [ ] last\n");
    session.setSource(source);
    QStringList ids;
    QStringList fingerprints;
    for (const Node &node : session.document().nodes) {
        if (node.kind == NodeKind::Item) {
            ids.append(node.sessionId);
            fingerprints.append(node.fingerprint);
        }
    }
    QCOMPARE(fingerprints[0], fingerprints[1]);
    QVERIFY(ids[0] != ids[1]);
    auto bind = [&](const QString &value, const QString &node = QString()) {
        return session.externalBinding(session.document().revision, "feed-the-flock", value, node);
    };
    auto code = [](const QJsonObject &result) {
        return result.value("error").toObject().value("code").toString();
    };
    QVERIFY(bind("a", ids[0]).value("ok").toBool());
    QVERIFY(bind("b", ids[1]).value("ok").toBool());
    QVERIFY(bind("a", ids[0]).value("ok").toBool());
    QCOMPARE(code(bind("a", ids[1])), "external_id_collision");
    QCOMPARE(code(session.externalBinding("stale", "feed-the-flock", "c", ids[2])),
             "stale_revision");
    QCOMPARE(code(bind("c")), "unknown_external_id");
    QVERIFY(session.externalBinding(session.document().revision, "another-tool", "a", ids[1])
                .value("ok").toBool());
    session.setSource(source);
    QCOMPARE(bind("a").value("node").toString(), ids[0]);
    QVERIFY(session.apply(session.document().revision, "move_after", ids[0],
                          {{"target", ids[2]}}).ok);
    QCOMPARE(bind("a").value("node").toString(), ids[0]);
    QVERIFY(session.apply(session.document().revision, "delete", ids[0]).ok);
    QCOMPARE(code(bind("a")), "external_id_retired");
    QCOMPARE(code(bind("a", ids[2])), "external_id_retired");
    QVERIFY(bind("last", ids[2]).value("ok").toBool());
    session.setSource("# Tasks\n\n- completely unrelated replacement\n");
    QCOMPARE(code(bind("last")), "external_id_retired");
    DocumentSession restarted;
    restarted.setSource(source);
    QCOMPARE(code(restarted.externalBinding(restarted.document().revision, "feed-the-flock", "a")),
             "unknown_external_id");

    DocumentSession ambiguous;
    ambiguous.setSource(source);
    const auto items = ambiguous.document().toJson().value("sections").toArray()[0]
                           .toObject().value("items").toArray();
    QVERIFY(ambiguous.externalBinding(ambiguous.document().revision, "tool", "duplicate",
                                     items[0].toString()).value("ok").toBool());
    ambiguous.setSource(source + "\nAn external edit.\n");
    QCOMPARE(code(ambiguous.externalBinding(ambiguous.document().revision, "tool", "duplicate")),
             "external_id_retired");
}

void DocumentEngineTest::parsesAndMutatesThousandLines() {
    QByteArray source("# Large document\n\n");
    source.reserve(26000);
    for (int index = 0; index < 998; ++index)
        source += "- [ ] item " + QByteArray::number(index) + '\n';

    QElapsedTimer timer;
    timer.start();
    DocumentSession session;
    session.setSource(source);
    const qint64 parseMilliseconds = timer.elapsed();
    QCOMPARE(session.document().source.count('\n'), 1000);
    QVERIFY(session.document().nodes.size() >= 1000);

    const Node &middle = *std::find_if(
        session.document().nodes.cbegin(), session.document().nodes.cend(),
        [](const Node &node) {
            return node.kind == NodeKind::Item && node.text == QStringLiteral("item 500");
        });
    QVERIFY(!middle.sessionId.isEmpty());
    timer.restart();
    const EditResult changed = session.apply(
        session.document().revision, QStringLiteral("task_set"), middle.sessionId,
        {{QStringLiteral("checked"), true}});
    const qint64 mutationMilliseconds = timer.elapsed();
    QVERIFY2(changed.ok, qPrintable(changed.errorMessage));
    QVERIFY(session.document().source.contains("- [x] item 500"));
    qInfo().nospace() << "1,000-line parse=" << parseMilliseconds
                      << "ms mutation=" << mutationMilliseconds << "ms";
}

void DocumentEngineTest::unchangedParseIsByteExact() {
    const QByteArray source = "# Title\r\n\r\n- [ ] one\r\n- two";
    DocumentEngine engine;
    const Document document = engine.parse(source);
    QCOMPARE(document.source, source);
    QCOMPARE(document.revision, DocumentEngine::revisionFor(source));
}

void DocumentEngineTest::sessionTrackingLeavesSourceClean() {
    const QByteArray source =
        "# Tasks\n\n"
        "- [ ] repeated\n"
        "- [ ] repeated\n";
    DocumentSession session;
    session.setSource(source);
    QVERIFY(session.tracking());
    const EditResult started = session.startTracking();
    QVERIFY2(started.ok, qPrintable(started.errorMessage));
    QCOMPARE(started.source, source);
    QVERIFY(!started.source.contains("mustermark:"));

    QVector<Node> items;
    for (const Node &node : session.document().nodes) {
        if (node.kind == NodeKind::Item)
            items.append(node);
    }
    QCOMPARE(items.size(), 2);
    QVERIFY(!items.at(0).sessionId.isEmpty());
    QVERIFY(items.at(0).sessionId != items.at(1).sessionId);
    QCOMPARE(items.at(0).fingerprint, items.at(1).fingerprint);

    const EditResult labelled = session.apply(
        session.document().revision, QStringLiteral("label_add"), items.at(0).sessionId,
        {{QStringLiteral("label"), QStringLiteral("demo/first")}});
    QVERIFY2(labelled.ok, qPrintable(labelled.errorMessage));
    QCOMPARE(session.document().source, source);
    QCOMPARE(session.document().nodes.at(session.document().findNode(items.at(0).sessionId)).labels,
             QStringList{QStringLiteral("demo/first")});

    const EditResult stopped = session.stopTracking();
    QVERIFY2(stopped.ok, qPrintable(stopped.errorMessage));
    QCOMPARE(stopped.source, source);
    for (const Node &node : session.document().nodes) {
        QVERIFY(!node.sessionId.isEmpty());
    }
    QCOMPARE(session.document().nodes.at(session.document().findNode(items.at(0).sessionId)).labels,
             QStringList{QStringLiteral("demo/first")});
}

void DocumentEngineTest::sessionMoveKeepsTargetIdentity() {
    const QByteArray source =
        "- same\n"
        "- other\n"
        "- same\n";
    DocumentSession session;
    session.setSource(source);
    session.startTracking();

    QVector<Node> sameItems;
    Node other;
    for (const Node &node : session.document().nodes) {
        if (node.kind != NodeKind::Item)
            continue;
        if (node.text == QStringLiteral("same"))
            sameItems.append(node);
        else if (node.text == QStringLiteral("other"))
            other = node;
    }
    QCOMPARE(sameItems.size(), 2);
    const QString movedIdentity = sameItems.at(0).sessionId;
    const EditResult moved = session.apply(
        session.document().revision, QStringLiteral("move_after"), movedIdentity,
        {{QStringLiteral("target"), other.sessionId}});
    QVERIFY2(moved.ok, qPrintable(moved.errorMessage));
    QCOMPARE(moved.source, QByteArray("- other\n- same\n- same\n"));
    QVERIFY(!moved.source.contains("mustermark:"));
    const int movedIndex = session.document().findNode(movedIdentity);
    QVERIFY(movedIndex >= 0);
    QCOMPARE(session.document().nodes.at(movedIndex).startLine, 2);
}

void DocumentEngineTest::sessionIdentitySurvivesSourceTyping() {
    DocumentSession session;
    session.setSource("# Notes\n\n- [ ] draft wording\n");
    session.startTracking();
    const auto item = std::find_if(
        session.document().nodes.cbegin(), session.document().nodes.cend(),
        [](const Node &node) { return node.kind == NodeKind::Item; });
    QVERIFY(item != session.document().nodes.cend());
    const QString identity = item->sessionId;
    QVERIFY(session.apply(
        session.document().revision, QStringLiteral("label_add"), identity,
        {{QStringLiteral("label"), QStringLiteral("draft")}}).ok);

    session.setSource("# Notes\n\n- [ ] final wording\n");
    const int changedIndex = session.document().findNode(identity);
    QVERIFY(changedIndex >= 0);
    QCOMPARE(session.document().nodes.at(changedIndex).text, QStringLiteral("final wording"));
    QCOMPARE(session.document().nodes.at(changedIndex).labels,
             QStringList{QStringLiteral("draft")});
}

void DocumentEngineTest::sessionInvalidatesUnrelatedReplacement() {
    DocumentSession session;
    session.setSource("# Notes\n\n- [ ] draft wording\n");
    const auto item = std::find_if(
        session.document().nodes.cbegin(), session.document().nodes.cend(),
        [](const Node &node) { return node.kind == NodeKind::Item; });
    QVERIFY(item != session.document().nodes.cend());
    const QString identity = item->sessionId;

    session.setSource("# Notes\n\n- [ ] book flights\n");
    QCOMPARE(session.document().findNode(identity), -1);
    QVERIFY(session.document().invalidatedIds.contains(identity));
    const auto replacement = std::find_if(
        session.document().nodes.cbegin(), session.document().nodes.cend(),
        [](const Node &node) { return node.kind == NodeKind::Item; });
    QVERIFY(replacement != session.document().nodes.cend());
    QVERIFY(replacement->sessionId != identity);

    const EditResult stale = session.apply(
        session.document().revision, QStringLiteral("toggle_task"), identity);
    QVERIFY(!stale.ok);
    QCOMPARE(stale.errorCode, QStringLiteral("unknown_id"));
    QCOMPARE(session.document().source, QByteArray("# Notes\n\n- [ ] book flights\n"));
}

void DocumentEngineTest::sessionMoveKeepsDuplicateSubtreeIdentities() {
    DocumentSession session;
    session.setSource(
        "- same\n"
        "    - child\n"
        "- same\n"
        "    - child\n"
        "- other\n");

    QVector<Node> topItems;
    for (const Node &node : session.document().nodes)
        if (node.kind == NodeKind::Item && node.depth == 1)
            topItems.append(node);
    QCOMPARE(topItems.size(), 3);
    const QString movedId = topItems.at(0).sessionId;
    const int movedIndex = session.document().findNode(movedId);
    QVERIFY(movedIndex >= 0);
    const auto child = std::find_if(
        session.document().nodes.cbegin(), session.document().nodes.cend(),
        [&](const Node &node) {
            return node.kind == NodeKind::Item && node.depth > 1 &&
                   node.startByte >= session.document().nodes.at(movedIndex).startByte &&
                   node.endByte <= session.document().nodes.at(movedIndex).endByte;
        });
    QVERIFY(child != session.document().nodes.cend());
    const QString childId = child->sessionId;

    const EditResult moved = session.apply(
        session.document().revision, QStringLiteral("move_after"), movedId,
        {{QStringLiteral("target"), topItems.at(2).sessionId}});
    QVERIFY2(moved.ok, qPrintable(moved.errorMessage));
    const int newRoot = session.document().findNode(movedId);
    const int newChild = session.document().findNode(childId);
    QVERIFY(newRoot >= 0);
    QVERIFY(newChild >= 0);
    QVERIFY(session.document().nodes.at(newChild).startByte >=
            session.document().nodes.at(newRoot).startByte);
    QVERIFY(session.document().nodes.at(newChild).endByte <=
            session.document().nodes.at(newRoot).endByte);
}

void DocumentEngineTest::snapshotsItemsSectionsAndAttachments() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString documentPath = directory.filePath(QStringLiteral("notes.md"));
    QVERIFY(QDir().mkpath(directory.filePath(QStringLiteral("notes.assets"))));
    QFile image(directory.filePath(QStringLiteral("notes.assets/pixel.png")));
    QVERIFY(image.open(QIODevice::WriteOnly));
    QCOMPARE(image.write("pixel-bytes"), qint64(11));
    image.close();

    const QByteArray source =
        "# Inbox\n\n"
        "- [ ] first item\n"
        "  continued text\n"
        "  ![pixel](notes.assets/pixel.png)\n"
        "    - nested detail\n"
        "- [x] second item\n\n"
        "# Later\n\n"
        "- outside\n";
    QFile document(documentPath);
    QVERIFY(document.open(QIODevice::WriteOnly));
    QCOMPARE(document.write(source), source.size());
    document.close();

    DocumentSession session;
    session.setSource(source);
    Node heading;
    Node first;
    for (const Node &node : session.document().nodes) {
        if (node.kind == NodeKind::Heading && node.text == QStringLiteral("Inbox"))
            heading = node;
        if (node.kind == NodeKind::Item && node.text == QStringLiteral("first item"))
            first = node;
    }
    QVERIFY(!heading.sessionId.isEmpty());
    QVERIFY(!first.sessionId.isEmpty());

    const QJsonObject itemSnapshot = session.snapshot(documentPath, first.sessionId);
    QVERIFY2(itemSnapshot.value(QStringLiteral("ok")).toBool(),
             qPrintable(QJsonDocument(itemSnapshot).toJson()));
    QCOMPARE(itemSnapshot.value(QStringLiteral("snapshotVersion")).toInt(), 1);
    const QJsonArray itemUnits = itemSnapshot.value(QStringLiteral("units")).toArray();
    QCOMPARE(itemUnits.size(), 1);
    const QJsonObject itemUnit = itemUnits.at(0).toObject();
    QVERIFY(itemUnit.value(QStringLiteral("markdown")).toString().contains(
        QStringLiteral("- nested detail")));
    QCOMPARE(itemUnit.value(QStringLiteral("text")).toString(),
             QStringLiteral("first item\nnested detail"));
    const QJsonObject attachment = itemUnit.value(QStringLiteral("attachments"))
                                       .toArray().at(0).toObject();
    QCOMPARE(attachment.value(QStringLiteral("relativePath")).toString(),
             QStringLiteral("notes.assets/pixel.png"));
    QCOMPARE(attachment.value(QStringLiteral("mimeType")).toString(),
             QStringLiteral("image/png"));
    QVERIFY(attachment.value(QStringLiteral("sha256")).toString().startsWith(
        QStringLiteral("sha256:")));

    const QJsonObject sectionSnapshot = session.snapshot(
        documentPath, heading.sessionId, QStringLiteral("section"));
    QVERIFY2(sectionSnapshot.value(QStringLiteral("ok")).toBool(),
             qPrintable(QJsonDocument(sectionSnapshot).toJson()));
    const QJsonArray sectionUnits = sectionSnapshot.value(QStringLiteral("units")).toArray();
    QCOMPARE(sectionUnits.size(), 2);
    QCOMPARE(sectionUnits.at(0).toObject().value(QStringLiteral("text")).toString(),
             QStringLiteral("first item\nnested detail"));
    QCOMPARE(sectionUnits.at(1).toObject().value(QStringLiteral("text")).toString(),
             QStringLiteral("second item"));

    QVERIFY(QFile::remove(directory.filePath(QStringLiteral("notes.assets/pixel.png"))));
    const QJsonObject missing = session.snapshot(documentPath, first.sessionId);
    QVERIFY(!missing.value(QStringLiteral("ok")).toBool());
    QCOMPARE(missing.value(QStringLiteral("error")).toObject()
                 .value(QStringLiteral("code")).toString(),
             QStringLiteral("missing_attachment"));

    QTemporaryDir outside;
    QVERIFY(outside.isValid());
    const QString outsidePath = outside.filePath(QStringLiteral("outside.png"));
    QFile outsideImage(outsidePath);
    QVERIFY(outsideImage.open(QIODevice::WriteOnly));
    QCOMPARE(outsideImage.write("outside"), qint64(7));
    outsideImage.close();
    const QString attachmentPath = directory.filePath(QStringLiteral("notes.assets/pixel.png"));
    QVERIFY(QFile::link(outsidePath, attachmentPath));
    const QJsonObject linked = session.snapshot(documentPath, first.sessionId);
    QVERIFY(!linked.value(QStringLiteral("ok")).toBool());
    QCOMPARE(linked.value(QStringLiteral("error")).toObject()
                 .value(QStringLiteral("code")).toString(),
             QStringLiteral("unsafe_attachment"));
    QVERIFY(QFile::remove(attachmentPath));

    QFile oversized(attachmentPath);
    QVERIFY(oversized.open(QIODevice::WriteOnly));
    QVERIFY(oversized.resize(8 * 1024 * 1024 + 1));
    oversized.close();
    const QJsonObject tooLarge = session.snapshot(documentPath, first.sessionId);
    QVERIFY(!tooLarge.value(QStringLiteral("ok")).toBool());
    QCOMPARE(tooLarge.value(QStringLiteral("error")).toObject()
                 .value(QStringLiteral("code")).toString(),
             QStringLiteral("attachment_too_large"));
    QCOMPARE(session.document().source, source);
}

void DocumentEngineTest::parsesHeadingAndNestedLists() {
    const QByteArray source =
        "# Plan\n\n"
        "- [ ] parent\n"
        "    1. child\n"
        "    2. second\n\n"
        "## Notes\n\nParagraph.\n";
    DocumentEngine engine;
    const Document document = engine.parse(source);
    QCOMPARE(document.nodes.at(0).kind, NodeKind::Heading);
    QCOMPARE(std::count_if(document.nodes.begin(), document.nodes.end(), [](const Node &node) {
        return node.kind == NodeKind::List;
    }), 2);
    QCOMPARE(std::count_if(document.nodes.begin(), document.nodes.end(), [](const Node &node) {
        return node.kind == NodeKind::Item;
    }), 3);
    const auto parent = std::find_if(document.nodes.begin(), document.nodes.end(), [](const Node &node) {
        return node.kind == NodeKind::Item && node.text == QStringLiteral("parent");
    });
    QVERIFY(parent != document.nodes.end());
    QVERIFY(parent->task);
    QVERIFY(!parent->checked);
}

void DocumentEngineTest::parsesLegacyIdentityMetadata() {
    const QByteArray source =
        "<!-- mustermark:tracking version=1 -->\n\n"
        "# Tasks\n\n"
        "<!-- mustermark:list id=lst_legacy -->\n"
        "- [ ] first\n"
        "  <!-- mustermark:item id=itm_first -->\n"
        "- second\n"
        "  <!-- mustermark:item id=itm_second -->\n";
    DocumentEngine engine;
    const Document document = engine.parse(source);
    QVERIFY(document.tracked);
    QVERIFY(document.findNode(QStringLiteral("lst_legacy")) >= 0);
    QVERIFY(document.findNode(QStringLiteral("itm_first")) >= 0);
    QVERIFY(document.findNode(QStringLiteral("itm_second")) >= 0);
    QVERIFY(document.diagnostics.isEmpty());
}

void DocumentEngineTest::sessionIdentityLeavesFrontMatterUntouched() {
    const QByteArray source = "---\ntitle: Test\n---\n# Heading\n\n- item\n";
    DocumentSession session;
    session.setSource(source);
    QCOMPARE(session.document().source, source);
    QVERIFY(!session.document().source.contains("mustermark:"));
    QVERIFY(std::all_of(session.document().nodes.cbegin(), session.document().nodes.cend(),
                        [](const Node &node) { return !node.sessionId.isEmpty(); }));
}

void DocumentEngineTest::taskToggleTouchesOneByte() {
    const QByteArray source = "- [ ] first\n";
    DocumentEngine engine;
    const Document document = engine.parse(source);
    const Node &item = *std::find_if(document.nodes.begin(), document.nodes.end(), [](const Node &node) {
        return node.kind == NodeKind::Item;
    });
    const EditResult changed = engine.apply(source, document.revision, QStringLiteral("toggle_task"), item.ref);
    QVERIFY(changed.ok);
    QCOMPARE(changed.source, QByteArray("- [x] first\n"));
    QCOMPARE(changed.edits.size(), 1);
}

void DocumentEngineTest::taskSetIsIdempotent() {
    const QByteArray source = "- [ ] first\n";
    DocumentEngine engine;
    Document document = engine.parse(source);
    const Node &item = *std::find_if(document.nodes.begin(), document.nodes.end(), [](const Node &node) {
        return node.kind == NodeKind::Item;
    });
    EditResult changed = engine.apply(
        source, document.revision, QStringLiteral("task_set"), item.ref,
        {{QStringLiteral("checked"), true}});
    QVERIFY2(changed.ok, qPrintable(changed.errorMessage));
    QCOMPARE(changed.source, QByteArray("- [x] first\n"));

    document = engine.parse(changed.source);
    const Node &checkedItem = *std::find_if(
        document.nodes.begin(), document.nodes.end(), [](const Node &node) {
            return node.kind == NodeKind::Item;
        });
    changed = engine.apply(
        changed.source, document.revision, QStringLiteral("task_set"), checkedItem.ref,
        {{QStringLiteral("checked"), true}});
    QVERIFY2(changed.ok, qPrintable(changed.errorMessage));
    QCOMPARE(changed.source, QByteArray("- [x] first\n"));
}

void DocumentEngineTest::associatesImagesWithItemSubtree() {
    const QByteArray source = "- first\n- second\n";
    DocumentEngine engine;
    Document document = engine.parse(source);
    const Node &first = *std::find_if(
        document.nodes.cbegin(), document.nodes.cend(), [](const Node &node) {
            return node.kind == NodeKind::Item && node.text == QStringLiteral("first");
        });
    EditResult changed = engine.apply(
        source, document.revision, QStringLiteral("attachment_add"), first.ref,
        {{QStringLiteral("path"), QStringLiteral("document.assets/pixel.png")},
         {QStringLiteral("alt"), QStringLiteral("sample image")}});
    QVERIFY2(changed.ok, qPrintable(changed.errorMessage));
    QCOMPARE(changed.source,
             QByteArray("- first\n  ![sample image](document.assets/pixel.png)\n- second\n"));

    document = engine.parse(changed.source);
    const Node &attached = *std::find_if(
        document.nodes.cbegin(), document.nodes.cend(), [](const Node &node) {
            return node.kind == NodeKind::Item && node.text == QStringLiteral("first");
        });
    QCOMPARE(attached.attachments.size(), 1);
    QCOMPARE(attached.attachments.constFirst().path,
             QStringLiteral("document.assets/pixel.png"));
    QCOMPARE(attached.attachments.constFirst().alt, QStringLiteral("sample image"));

    const Node &second = *std::find_if(
        document.nodes.cbegin(), document.nodes.cend(), [](const Node &node) {
            return node.kind == NodeKind::Item && node.text == QStringLiteral("second");
        });
    changed = engine.apply(
        changed.source, document.revision, QStringLiteral("move_after"), attached.ref,
        {{QStringLiteral("target"), second.ref}});
    QVERIFY2(changed.ok, qPrintable(changed.errorMessage));
    QCOMPARE(changed.source,
             QByteArray("- second\n- first\n  ![sample image](document.assets/pixel.png)\n"));

    document = engine.parse(changed.source);
    const Node &moved = *std::find_if(
        document.nodes.cbegin(), document.nodes.cend(), [](const Node &node) {
            return node.kind == NodeKind::Item && node.text == QStringLiteral("first");
        });
    changed = engine.apply(
        changed.source, document.revision, QStringLiteral("attachment_remove"), moved.ref,
        {{QStringLiteral("path"), QStringLiteral("document.assets/pixel.png")}});
    QVERIFY2(changed.ok, qPrintable(changed.errorMessage));
    QCOMPARE(changed.source, QByteArray("- second\n- first\n"));

    const QByteArray orderedSource = "1. first\n2. second\n";
    document = engine.parse(orderedSource);
    const Node &orderedFirst = *std::find_if(
        document.nodes.cbegin(), document.nodes.cend(), [](const Node &node) {
            return node.kind == NodeKind::Item && node.text == QStringLiteral("first");
        });
    changed = engine.apply(
        orderedSource, document.revision, QStringLiteral("attachment_add"), orderedFirst.ref,
        {{QStringLiteral("path"), QStringLiteral("document.assets/ordered.png")},
         {QStringLiteral("alt"), QStringLiteral("ordered image")}});
    QVERIFY2(changed.ok, qPrintable(changed.errorMessage));
    QCOMPARE(changed.source,
             QByteArray("1. first\n   ![ordered image](document.assets/ordered.png)\n2. second\n"));

    document = engine.parse(changed.source);
    const Node &orderedAttached = *std::find_if(
        document.nodes.cbegin(), document.nodes.cend(), [](const Node &node) {
            return node.kind == NodeKind::Item && node.text == QStringLiteral("first");
        });
    QCOMPARE(orderedAttached.attachments.size(), 1);
    QCOMPARE(orderedAttached.attachments.constFirst().path,
             QStringLiteral("document.assets/ordered.png"));

    document = engine.parse(source);
    changed = engine.apply(
        source, document.revision, QStringLiteral("attachment_add"), first.ref,
        {{QStringLiteral("path"), QStringLiteral("../outside.png")}});
    QVERIFY(!changed.ok);
    QCOMPARE(changed.errorCode, QStringLiteral("invalid_attachment"));
}

void DocumentEngineTest::addsSectionsListsAndItems() {
    DocumentEngine engine;
    QByteArray source =
        "# Board\n\n"
        "## First\n\n"
        "- alpha\n\n"
        "## Second\n\n"
        "Notes.\n";
    Document document = engine.parse(source);
    const Node &firstSection = *std::find_if(
        document.nodes.cbegin(), document.nodes.cend(), [](const Node &node) {
            return node.kind == NodeKind::Heading && node.text == QStringLiteral("First");
        });
    EditResult changed = engine.apply(
        source, document.revision, QStringLiteral("section_add"), firstSection.ref,
        {{QStringLiteral("text"), QStringLiteral("New section")}});
    QVERIFY2(changed.ok, qPrintable(changed.errorMessage));
    QCOMPARE(changed.source,
             QByteArray("# Board\n\n"
                        "## First\n\n"
                        "- alpha\n\n"
                        "## New section\n\n"
                        "## Second\n\n"
                        "Notes.\n"));

    source = changed.source;
    document = engine.parse(source);
    const Node &newSection = *std::find_if(
        document.nodes.cbegin(), document.nodes.cend(), [](const Node &node) {
            return node.kind == NodeKind::Heading && node.text == QStringLiteral("New section");
        });
    changed = engine.apply(
        source, document.revision, QStringLiteral("list_add"), newSection.ref,
        {{QStringLiteral("text"), QStringLiteral("first task")},
         {QStringLiteral("task"), true}});
    QVERIFY2(changed.ok, qPrintable(changed.errorMessage));
    QVERIFY(changed.source.contains("## New section\n\n- [ ] first task\n\n## Second"));

    source = changed.source;
    document = engine.parse(source);
    const Node &newList = *std::find_if(
        document.nodes.cbegin(), document.nodes.cend(), [&](const Node &node) {
            return node.kind == NodeKind::List && node.parent >= 0 &&
                   document.nodes.at(node.parent).text == QStringLiteral("New section");
        });
    changed = engine.apply(
        source, document.revision, QStringLiteral("item_add"), newList.ref,
        {{QStringLiteral("text"), QStringLiteral("second task")},
         {QStringLiteral("task"), true}});
    QVERIFY2(changed.ok, qPrintable(changed.errorMessage));
    QVERIFY(changed.source.contains("- [ ] first task\n- [ ] second task\n"));

    source = changed.source;
    document = engine.parse(source);
    const Node &secondTask = *std::find_if(
        document.nodes.cbegin(), document.nodes.cend(), [](const Node &node) {
            return node.kind == NodeKind::Item && node.text == QStringLiteral("second task");
        });
    changed = engine.apply(
        source, document.revision, QStringLiteral("item_add"), secondTask.ref,
        {{QStringLiteral("text"), QStringLiteral("plain item")}});
    QVERIFY2(changed.ok, qPrintable(changed.errorMessage));
    QVERIFY(changed.source.contains("- [ ] second task\n- plain item\n"));

    document = engine.parse(changed.source);
    const Node &section = *std::find_if(
        document.nodes.cbegin(), document.nodes.cend(), [](const Node &node) {
            return node.kind == NodeKind::Heading && node.text == QStringLiteral("New section");
        });
    const EditResult invalid = engine.apply(
        changed.source, document.revision, QStringLiteral("section_add"), section.ref,
        {{QStringLiteral("text"), QStringLiteral("bad\nheading")}});
    QVERIFY(!invalid.ok);
    QCOMPARE(invalid.errorCode, QStringLiteral("invalid_text"));
}

void DocumentEngineTest::addsMultilineItems() {
    const QByteArray source = "# Inbox\n\n- [ ] existing\n";
    DocumentEngine engine;
    const Document document = engine.parse(source);
    const auto item = std::find_if(document.nodes.cbegin(), document.nodes.cend(),
                                   [](const Node &node) { return node.kind == NodeKind::Item; });
    QVERIFY(item != document.nodes.cend());
    const EditResult changed = engine.apply(
        source, document.revision, QStringLiteral("item_add"), item->ref,
        {{QStringLiteral("text"), QStringLiteral("dictated first line\nsecond line")},
         {QStringLiteral("task"), true}});
    QVERIFY2(changed.ok, qPrintable(changed.errorMessage));
    QCOMPARE(changed.source,
             QByteArray("# Inbox\n\n- [ ] existing\n- [ ] dictated first line\n      second line\n"));
}

void DocumentEngineTest::rendersSafeEnrichedHtml() {
    const QByteArray source =
        "# Title\n\n"
        "<script>alert('no')</script>\n\n"
        "- [ ] first\n";
    DocumentSession session;
    session.setSource(source);
    Document document = session.document();
    const Node &item = *std::find_if(document.nodes.begin(), document.nodes.end(), [](const Node &node) {
        return node.kind == NodeKind::Item;
    });
    const QString itemId = item.sessionId;
    const EditResult labelled = session.apply(
        document.revision, QStringLiteral("label_add"), itemId,
        {{QStringLiteral("label"), QStringLiteral("demo/ready")}});
    QVERIFY2(labelled.ok, qPrintable(labelled.errorMessage));
    document = session.document();
    QVERIFY(document.renderedHtml.contains(QStringLiteral("data-mm-id=\"") + itemId));
    QVERIFY(document.renderedHtml.contains(QStringLiteral("data-mm-labels=\"demo/ready\"")));
    QVERIFY(document.renderedHtml.contains(QStringLiteral("data-mm-task=\"true\"")));
    QVERIFY(!document.renderedHtml.contains(QStringLiteral("<script")));
    QVERIFY(!document.renderedHtml.contains(QStringLiteral("raw HTML omitted")));

}

void DocumentEngineTest::labelsStayInSession() {
    const QByteArray source = "- item\n";
    DocumentSession session;
    session.setSource(source);
    Document document = session.document();
    const Node &item = *std::find_if(document.nodes.begin(), document.nodes.end(), [](const Node &node) {
        return node.kind == NodeKind::Item;
    });
    const EditResult result = session.apply(
        document.revision, QStringLiteral("label_add"), item.sessionId,
        {{QStringLiteral("label"), QStringLiteral("project/mustermark")}});
    QVERIFY2(result.ok, qPrintable(result.errorMessage));
    QCOMPARE(result.source, source);
    document = session.document();
    const int labelled = document.findNode(item.sessionId);
    QVERIFY(labelled >= 0);
    QVERIFY(document.nodes.at(labelled).labels.contains(QStringLiteral("project/mustermark")));
    QVERIFY(!document.source.contains("mustermark:"));
}

void DocumentEngineTest::movesWholeItemSubtree() {
    const QByteArray source =
        "- first\n"
        "    - child\n"
        "- second\n";
    DocumentEngine engine;
    const Document document = engine.parse(source);
    QVector<Node> topItems;
    for (const Node &node : document.nodes) {
        if (node.kind == NodeKind::Item && node.depth == 1)
            topItems.append(node);
    }
    QCOMPARE(topItems.size(), 2);
    const QString firstId = topItems.at(0).ref;
    const EditResult moved = engine.apply(source, document.revision,
                                          QStringLiteral("move_after"), firstId,
                                          {{QStringLiteral("target"), topItems.at(1).ref}});
    QVERIFY2(moved.ok, qPrintable(moved.errorMessage));
    const Document after = engine.parse(moved.source);
    const int firstPosition = moved.source.indexOf("- first");
    const int secondPosition = moved.source.indexOf("- second");
    QVERIFY(firstPosition > secondPosition);
    QVERIFY(moved.source.indexOf("- child", firstPosition) > firstPosition);
    QVERIFY(std::any_of(after.nodes.cbegin(), after.nodes.cend(), [](const Node &node) {
        return node.kind == NodeKind::Item && node.text == QStringLiteral("first");
    }));
}

void DocumentEngineTest::keepsTrailingParagraphOutsideMovedItem() {
    const QByteArray source =
        "- image item\n"
        "  ![sample](notes.assets/sample.png)\n"
        "- middle\n"
        "- last\n"
        "\n"
        "Outside the list.\n";
    DocumentEngine engine;
    Document document = engine.parse(source);
    QVector<Node> items;
    for (const Node &node : document.nodes) {
        if (node.kind == NodeKind::Item && node.depth == 1)
            items.append(node);
    }
    QCOMPARE(items.size(), 3);
    QCOMPARE(items.at(2).endLine, 4);

    const EditResult moved = engine.apply(
        source, document.revision, QStringLiteral("move_after"), items.at(0).ref,
        {{QStringLiteral("target"), items.at(2).ref}});
    QVERIFY2(moved.ok, qPrintable(moved.errorMessage));
    QCOMPARE(moved.source,
             QByteArray("- middle\n"
                        "- last\n"
                        "- image item\n"
                        "  ![sample](notes.assets/sample.png)\n"
                        "\n"
                        "Outside the list.\n"));

    const QByteArray lazyContinuation =
        "- image item\n"
        "  ![sample](notes.assets/sample.png)\n"
        "Outside the list.\n"
        "- last\n";
    document = engine.parse(lazyContinuation);
    const Node &imageItem = *std::find_if(
        document.nodes.begin(), document.nodes.end(), [](const Node &node) {
            return node.kind == NodeKind::Item && node.text == QStringLiteral("image item");
        });
    QCOMPARE(imageItem.endLine, 2);
}

void DocumentEngineTest::renumbersOrderedListsAfterStructuralChanges() {
    const QByteArray source =
        "# Ordered\n\n"
        "1. first\n"
        "2. second\n"
        "3. third\n";
    DocumentEngine engine;
    Document document = engine.parse(source);
    QVector<Node> items;
    for (const Node &node : document.nodes) {
        if (node.kind == NodeKind::Item && node.depth == 2)
            items.append(node);
    }
    QCOMPARE(items.size(), 3);

    EditResult changed = engine.apply(
        source, document.revision, QStringLiteral("move_after"), items.at(0).ref,
        {{QStringLiteral("target"), items.at(2).ref}});
    QVERIFY2(changed.ok, qPrintable(changed.errorMessage));
    QCOMPARE(changed.source,
             QByteArray("# Ordered\n\n1. second\n2. third\n3. first\n"));

    document = engine.parse(changed.source);
    const Node &third = *std::find_if(
        document.nodes.cbegin(), document.nodes.cend(), [](const Node &node) {
            return node.kind == NodeKind::Item && node.text == QStringLiteral("third");
        });
    changed = engine.apply(changed.source, document.revision, QStringLiteral("delete"),
                           third.ref);
    QVERIFY2(changed.ok, qPrintable(changed.errorMessage));
    QCOMPARE(changed.source, QByteArray("# Ordered\n\n1. second\n2. first\n"));

    document = engine.parse(changed.source);
    const Node &list = *std::find_if(
        document.nodes.cbegin(), document.nodes.cend(), [](const Node &node) {
            return node.kind == NodeKind::List && node.ordered;
        });
    changed = engine.apply(
        changed.source, document.revision, QStringLiteral("item_add"), list.ref,
        {{QStringLiteral("text"), QStringLiteral("fourth")}});
    QVERIFY2(changed.ok, qPrintable(changed.errorMessage));
    QCOMPARE(changed.source,
             QByteArray("# Ordered\n\n1. second\n2. first\n3. fourth\n"));

    const QByteArray numbered = "7) first\n8) second\n9) third\n";
    document = engine.parse(numbered);
    items.clear();
    for (const Node &node : document.nodes) {
        if (node.kind == NodeKind::Item && node.depth == 1)
            items.append(node);
    }
    QCOMPARE(items.size(), 3);
    changed = engine.apply(
        numbered, document.revision, QStringLiteral("move_after"), items.at(0).ref,
        {{QStringLiteral("target"), items.at(2).ref}});
    QVERIFY2(changed.ok, qPrintable(changed.errorMessage));
    QCOMPARE(changed.source, QByteArray("7) second\n8) third\n9) first\n"));
}

void DocumentEngineTest::movesWholeListBlock() {
    const QByteArray source =
        "# Section\n"
        "\n"
        "Intro paragraph.\n"
        "\n"
        "- one\n"
        "- two\n";
    DocumentEngine engine;
    const Document document = engine.parse(source);
    const Node &list = *std::find_if(
        document.nodes.begin(), document.nodes.end(), [](const Node &node) {
            return node.kind == NodeKind::List;
        });
    const Node &paragraph = *std::find_if(
        document.nodes.begin(), document.nodes.end(), [](const Node &node) {
            return node.kind == NodeKind::Block;
        });

    const EditResult moved = engine.apply(
        source, document.revision, QStringLiteral("move_before"), list.ref,
        {{QStringLiteral("target"), paragraph.ref}});
    QVERIFY2(moved.ok, qPrintable(moved.errorMessage));
    QCOMPARE(moved.source,
             QByteArray("# Section\n"
                        "\n"
                        "- one\n"
                        "- two\n"
                        "\n"
                        "Intro paragraph.\n"
                        "\n"));
}

void DocumentEngineTest::movesBlockAcrossHeadingSections() {
    const QByteArray source =
        "# First\n"
        "\n"
        "First paragraph.\n"
        "\n"
        "## Second\n"
        "\n"
        "Second paragraph.\n";
    DocumentEngine engine;
    const Document document = engine.parse(source);
    QVector<Node> paragraphs;
    for (const Node &node : document.nodes) {
        if (node.kind == NodeKind::Block)
            paragraphs.append(node);
    }
    QCOMPARE(paragraphs.size(), 2);
    QVERIFY(paragraphs.at(0).parent != paragraphs.at(1).parent);

    const EditResult moved = engine.apply(
        source, document.revision, QStringLiteral("move_after"), paragraphs.at(0).ref,
        {{QStringLiteral("target"), paragraphs.at(1).ref}});
    QVERIFY2(moved.ok, qPrintable(moved.errorMessage));
    QCOMPARE(moved.source,
             QByteArray("# First\n"
                        "\n"
                        "## Second\n"
                        "\n"
                        "Second paragraph.\n"
                        "\n"
                        "First paragraph.\n"
                        "\n"));
    const Document changed = engine.parse(moved.source);
    const auto movedParagraph = std::find_if(
        changed.nodes.cbegin(), changed.nodes.cend(), [](const Node &node) {
            return node.kind == NodeKind::Block && node.text == QStringLiteral("First paragraph.");
        });
    QVERIFY(movedParagraph != changed.nodes.cend());
    QVERIFY(movedParagraph->parent >= 0);
    QCOMPARE(changed.nodes.at(movedParagraph->parent).text, QStringLiteral("Second"));
}

void DocumentEngineTest::movesHeadingBranchesWithoutMergingBoundaries() {
    const QByteArray source =
        "#### Heading branch\n"
        "\n"
        "##### Child heading\n"
        "\n"
        "Child contents.\n"
        "A final paragraph.\n"
        "#### Ready for Feed the Flock\n"
        "\n"
        "- [ ] Continue\n";
    DocumentEngine engine;
    const Document document = engine.parse(source);
    const auto branch = std::find_if(document.nodes.cbegin(), document.nodes.cend(),
        [](const Node &candidate) {
            return candidate.kind == NodeKind::Heading &&
                   candidate.text == QStringLiteral("Heading branch");
        });
    const auto ready = std::find_if(document.nodes.cbegin(), document.nodes.cend(),
        [](const Node &candidate) {
            return candidate.kind == NodeKind::Heading &&
                   candidate.text == QStringLiteral("Ready for Feed the Flock");
        });
    QVERIFY(branch != document.nodes.cend());
    QVERIFY(ready != document.nodes.cend());
    const EditResult moved = engine.apply(
        source, document.revision, QStringLiteral("move_before"), ready->ref,
        {{QStringLiteral("target"), branch->ref}});
    QVERIFY2(moved.ok, qPrintable(moved.errorMessage));
    QCOMPARE(moved.source, QByteArray(
        "#### Ready for Feed the Flock\n\n"
        "- [ ] Continue\n\n"
        "#### Heading branch\n\n"
        "##### Child heading\n\n"
        "Child contents.\n"
        "A final paragraph.\n"));
    const Document reparsed = engine.parse(moved.source);
    QCOMPARE(std::count_if(reparsed.nodes.cbegin(), reparsed.nodes.cend(),
        [](const Node &candidate) { return candidate.kind == NodeKind::Heading; }), 3);
}

void DocumentEngineTest::replacesStructureMarkdown() {
    const QByteArray source = "- parent\n  - child\n- target\n";
    DocumentEngine engine;
    const Document document = engine.parse(source);
    const auto parent = std::find_if(
        document.nodes.cbegin(), document.nodes.cend(), [](const Node &node) {
            return node.kind == NodeKind::Item && node.text == QStringLiteral("parent");
        });
    QVERIFY(parent != document.nodes.cend());
    const EditResult changed = engine.apply(
        source, document.revision, QStringLiteral("replace"), parent->ref,
        {{QStringLiteral("markdown"), QStringLiteral("- renamed\n  - new child\n")}});
    QVERIFY2(changed.ok, qPrintable(changed.errorMessage));
    QCOMPARE(changed.source, QByteArray("- renamed\n  - new child\n- target\n"));

    const EditResult peers = engine.apply(
        source, document.revision, QStringLiteral("replace"), parent->ref,
        {{QStringLiteral("markdown"), QStringLiteral("- first\n- second\n")}});
    QVERIFY(!peers.ok);
    QCOMPARE(peers.errorCode, QStringLiteral("invalid_structure"));

    const QByteArray headingSource = "## Section\n\nBody.\n\n# Next\n";
    const Document headings = engine.parse(headingSource);
    const EditResult headingChanged = engine.apply(
        headingSource, headings.revision, QStringLiteral("replace"), headings.nodes.at(0).ref,
        {{QStringLiteral("markdown"), QStringLiteral("## Renamed\n\nChanged body.\n\n")}});
    QVERIFY2(headingChanged.ok, qPrintable(headingChanged.errorMessage));
    QCOMPARE(headingChanged.source, QByteArray("## Renamed\n\nChanged body.\n\n# Next\n"));

    const EditResult wrongLevel = engine.apply(
        headingSource, headings.revision, QStringLiteral("replace"), headings.nodes.at(0).ref,
        {{QStringLiteral("markdown"), QStringLiteral("### Wrong level\n")}});
    QVERIFY(!wrongLevel.ok);
    QCOMPARE(wrongLevel.errorCode, QStringLiteral("invalid_structure"));
}

void DocumentEngineTest::setsOnlySelectedHeadingLevel() {
    const QByteArray source = "## Parent\n\n### Child\n\nBody.\n";
    DocumentEngine engine;
    const Document document = engine.parse(source);
    const Node &parent = document.nodes.at(0);
    const EditResult changed = engine.apply(
        source, document.revision, QStringLiteral("set_heading_level"), parent.ref,
        {{QStringLiteral("level"), 1}});
    QVERIFY2(changed.ok, qPrintable(changed.errorMessage));
    QCOMPARE(changed.source, QByteArray("# Parent\n\n### Child\n\nBody.\n"));
}

void DocumentEngineTest::promotesOnlySelectedHeading() {
    const QByteArray source = "## Parent\n\n### Child\n\nBody.\n";
    DocumentEngine engine;
    const Document document = engine.parse(source);
    const Node &parent = document.nodes.at(0);
    const EditResult changed = engine.apply(
        source, document.revision, QStringLiteral("promote"), parent.ref);
    QVERIFY2(changed.ok, qPrintable(changed.errorMessage));
    QCOMPARE(changed.source, QByteArray("# Parent\n\n### Child\n\nBody.\n"));
}

void DocumentEngineTest::changesWholeHeadingBranchLevel() {
    const QByteArray source = "## Parent\n\n### Child\n\nBody.\n";
    DocumentEngine engine;
    const Document document = engine.parse(source);
    const Node &parent = document.nodes.at(0);
    const EditResult changed = engine.apply(
        source, document.revision, QStringLiteral("demote"), parent.ref,
        {{QStringLiteral("scope"), QStringLiteral("subtree")}});
    QVERIFY2(changed.ok, qPrintable(changed.errorMessage));
    QCOMPARE(changed.source, QByteArray("### Parent\n\n#### Child\n\nBody.\n"));
}

void DocumentEngineTest::keepsHeadingAboveItsDescendants() {
    const QByteArray source = "## Parent\n\n### Child\n\nBody.\n";
    DocumentEngine engine;
    Document document = engine.parse(source);
    const Node &parent = document.nodes.at(0);

    EditResult changed = engine.apply(
        source, document.revision, QStringLiteral("set_heading_level"), parent.ref,
        {{QStringLiteral("level"), 3}});
    QVERIFY(!changed.ok);
    QCOMPARE(changed.errorCode, QStringLiteral("unsafe_rewrite"));
    QCOMPARE(changed.source, source);

    changed = engine.apply(source, document.revision, QStringLiteral("demote"), parent.ref);
    QVERIFY(!changed.ok);
    QCOMPARE(changed.errorCode, QStringLiteral("unsafe_rewrite"));

    changed = engine.apply(
        source, document.revision, QStringLiteral("demote"), parent.ref,
        {{QStringLiteral("scope"), QStringLiteral("subtree")}});
    QVERIFY2(changed.ok, qPrintable(changed.errorMessage));
    QCOMPARE(changed.source, QByteArray("### Parent\n\n#### Child\n\nBody.\n"));

    const QByteArray spaced = "## Parent\n\n#### Child\n";
    document = engine.parse(spaced);
    changed = engine.apply(
        spaced, document.revision, QStringLiteral("set_heading_level"),
        document.nodes.at(0).ref, {{QStringLiteral("level"), 3}});
    QVERIFY2(changed.ok, qPrintable(changed.errorMessage));
    QCOMPARE(changed.source, QByteArray("### Parent\n\n#### Child\n"));
}

void DocumentEngineTest::promotesAndDemotesListItems() {
    const QByteArray nested =
        "- parent\n"
        "    - child\n"
        "        - grandchild\n";
    DocumentEngine engine;
    Document document = engine.parse(nested);
    const Node &nestedChild = *std::find_if(
        document.nodes.begin(), document.nodes.end(), [](const Node &node) {
            return node.kind == NodeKind::Item && node.text == QStringLiteral("child");
        });

    EditResult changed = engine.apply(
        nested, document.revision, QStringLiteral("promote"), nestedChild.ref);
    QVERIFY2(changed.ok, qPrintable(changed.errorMessage));
    QCOMPARE(changed.source,
             QByteArray("- parent\n- child\n    - grandchild\n"));

    const QByteArray nestedLeaf =
        "- parent\n"
        "    - child\n";
    document = engine.parse(nestedLeaf);
    const Node &leaf = *std::find_if(
        document.nodes.begin(), document.nodes.end(), [](const Node &node) {
            return node.kind == NodeKind::Item && node.text == QStringLiteral("child");
        });
    changed = engine.apply(
        nestedLeaf, document.revision, QStringLiteral("promote"), leaf.ref);
    QVERIFY2(changed.ok, qPrintable(changed.errorMessage));
    QCOMPARE(changed.source, QByteArray("- parent\n- child\n"));

    const QByteArray siblings =
        "- parent\n"
        "- child\n"
        "    - grandchild\n";
    document = engine.parse(siblings);
    const Node &topChild = *std::find_if(
        document.nodes.begin(), document.nodes.end(), [](const Node &node) {
            return node.kind == NodeKind::Item && node.text == QStringLiteral("child");
        });

    changed = engine.apply(
        siblings, document.revision, QStringLiteral("demote"), topChild.ref);
    QVERIFY2(changed.ok, qPrintable(changed.errorMessage));
    QCOMPARE(changed.source,
             QByteArray("- parent\n    - child\n        - grandchild\n"));

    changed = engine.apply(
        siblings, document.revision, QStringLiteral("demote"), topChild.ref,
        {{QStringLiteral("scope"), QStringLiteral("self")}});
    QVERIFY(!changed.ok);
    QCOMPARE(changed.errorCode, QStringLiteral("invalid_scope"));
}

void DocumentEngineTest::preservesSessionIdentityAcrossListLevelChanges() {
    const QByteArray source =
        "- [ ] first\n"
        "- [ ] second\n";
    DocumentSession session;
    session.setSource(source);
    Document document = session.document();
    const Node &second = *std::find_if(
        document.nodes.begin(), document.nodes.end(), [](const Node &node) {
            return node.kind == NodeKind::Item && node.text == QStringLiteral("second");
        });
    const QString secondId = second.sessionId;

    EditResult changed = session.apply(
        document.revision, QStringLiteral("demote"), secondId,
        {{QStringLiteral("scope"), QStringLiteral("subtree")}});
    QVERIFY2(changed.ok, qPrintable(changed.errorMessage));
    document = session.document();
    QVERIFY(document.diagnostics.isEmpty());
    QVERIFY(document.findNode(secondId) >= 0);

    changed = session.apply(
        document.revision, QStringLiteral("promote"), secondId,
        {{QStringLiteral("scope"), QStringLiteral("subtree")}});
    QVERIFY2(changed.ok, qPrintable(changed.errorMessage));
    document = session.document();
    QVERIFY(document.diagnostics.isEmpty());
    QVERIFY(document.findNode(secondId) >= 0);
    QCOMPARE(document.source, source);
    QVERIFY(!document.source.contains("mustermark:"));
}

void DocumentEngineTest::rejectsHeadingBranchBeyondLevelSix() {
    const QByteArray source = "##### Parent\n\n###### Child\n";
    DocumentEngine engine;
    const Document document = engine.parse(source);
    const Node &parent = document.nodes.at(0);
    const EditResult changed = engine.apply(
        source, document.revision, QStringLiteral("demote"), parent.ref,
        {{QStringLiteral("scope"), QStringLiteral("subtree")}});
    QVERIFY(!changed.ok);
    QCOMPARE(changed.errorCode, QStringLiteral("unsafe_rewrite"));
    QCOMPARE(changed.source, source);
}

void DocumentEngineTest::rejectsStaleRevision() {
    const QByteArray source = "- [ ] item\n";
    DocumentEngine engine;
    const Document document = engine.parse(source);
    const Node &item = *std::find_if(document.nodes.begin(), document.nodes.end(), [](const Node &node) {
        return node.kind == NodeKind::Item;
    });
    const EditResult result = engine.apply(source, QStringLiteral("sha256:wrong"),
                                           QStringLiteral("toggle_task"), item.ref);
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("stale_revision"));
}

void DocumentEngineTest::reportsDuplicateLegacyIds() {
    const QByteArray source =
        "<!-- mustermark:tracking version=1 -->\n\n"
        "<!-- mustermark:list id=lst_same -->\n"
        "- first\n"
        "  <!-- mustermark:item id=itm_same -->\n"
        "- second\n"
        "  <!-- mustermark:item id=itm_same -->\n";
    DocumentEngine engine;
    QVERIFY(!engine.parse(source).diagnostics.isEmpty());
    const EditResult removed = engine.untrack(source);
    QVERIFY2(removed.ok, qPrintable(removed.errorMessage));
    QVERIFY(engine.parse(removed.source).diagnostics.isEmpty());
}

void DocumentEngineTest::untrackRemovesOnlyMetadata() {
    const QByteArray plain = "# Tasks\n\n- [ ] first\n- second\n";
    const QByteArray source =
        "<!-- mustermark:tracking version=1 -->\n\n"
        "# Tasks\n\n"
        "<!-- mustermark:list id=lst_legacy -->\n"
        "- [ ] first\n"
        "  <!-- mustermark:item id=itm_first -->\n"
        "- second\n"
        "  <!-- mustermark:item id=itm_second -->\n";
    DocumentEngine engine;
    const EditResult removed = engine.untrack(source);
    QVERIFY(removed.ok);
    QCOMPARE(removed.source, plain);
}

void DocumentEngineTest::atomicWriteRejectsExternalChange() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("doc.md"));
    QVERIFY(FileDocument::writeAtomic(path, "one\n").ok);
    const QString firstRevision = DocumentEngine::revisionFor("one\n");
    QVERIFY(FileDocument::writeAtomic(path, "outside\n").ok);
    const FileResult rejected = FileDocument::writeAtomic(path, "ours\n", firstRevision);
    QVERIFY(!rejected.ok);
    QCOMPARE(rejected.error, QStringLiteral("stale_revision"));
    QCOMPARE(FileDocument::read(path).source, QByteArray("outside\n"));
}

QTEST_GUILESS_MAIN(DocumentEngineTest)
#include "test_documentengine.moc"
