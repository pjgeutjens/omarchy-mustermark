#include "documentengine.h"
#include "filedocument.h"

#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>

using namespace Mustermark;

class DocumentEngineTest : public QObject {
    Q_OBJECT

private slots:
    void unchangedParseIsByteExact();
    void parsesHeadingAndNestedLists();
    void trackingAddsPortableIdentity();
    void frontMatterStaysFirst();
    void taskToggleTouchesOneByte();
    void labelsRequireTracking();
    void movesWholeItemSubtree();
    void setsOnlySelectedHeadingLevel();
    void promotesOnlySelectedHeading();
    void changesWholeHeadingBranchLevel();
    void promotesAndDemotesListItems();
    void rejectsHeadingBranchBeyondLevelSix();
    void rejectsStaleRevision();
    void repairsDuplicateIds();
    void untrackRemovesOnlyMetadata();
    void atomicWriteRejectsExternalChange();
};

void DocumentEngineTest::unchangedParseIsByteExact() {
    const QByteArray source = "# Title\r\n\r\n- [ ] one\r\n- two";
    DocumentEngine engine;
    const Document document = engine.parse(source);
    QCOMPARE(document.source, source);
    QCOMPARE(document.revision, DocumentEngine::revisionFor(source));
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

void DocumentEngineTest::trackingAddsPortableIdentity() {
    const QByteArray source = "# Tasks\n\n- [ ] first\n- second\n";
    DocumentEngine engine;
    const EditResult tracked = engine.track(source);
    QVERIFY2(tracked.ok, qPrintable(tracked.errorMessage));
    QVERIFY(tracked.source.contains("<!-- mustermark:tracking version=1 -->"));
    QVERIFY(tracked.source.contains("<!-- mustermark:list id=lst_"));
    QCOMPARE(tracked.source.count("<!-- mustermark:item id=itm_"), 2);
    const Document document = engine.parse(tracked.source);
    QVERIFY(document.tracked);
    for (const Node &node : document.nodes) {
        if (node.kind == NodeKind::List || node.kind == NodeKind::Item)
            QVERIFY2(!node.id.isEmpty(), qPrintable(node.text));
    }
    QVERIFY(document.diagnostics.isEmpty());
}

void DocumentEngineTest::frontMatterStaysFirst() {
    const QByteArray source = "---\ntitle: Test\n---\n# Heading\n\n- item\n";
    DocumentEngine engine;
    const EditResult tracked = engine.track(source);
    QVERIFY(tracked.ok);
    QVERIFY(tracked.source.startsWith("---\ntitle: Test\n---\n<!-- mustermark:tracking"));
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

void DocumentEngineTest::labelsRequireTracking() {
    const QByteArray source = "- item\n";
    DocumentEngine engine;
    Document document = engine.parse(source);
    const Node &plain = *std::find_if(document.nodes.begin(), document.nodes.end(), [](const Node &node) {
        return node.kind == NodeKind::Item;
    });
    EditResult result = engine.apply(source, document.revision, QStringLiteral("label_add"), plain.ref,
                                     {{QStringLiteral("label"), QStringLiteral("next")}});
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("tracking_required"));

    const EditResult tracked = engine.track(source);
    document = engine.parse(tracked.source);
    const Node &item = *std::find_if(document.nodes.begin(), document.nodes.end(), [](const Node &node) {
        return node.kind == NodeKind::Item;
    });
    result = engine.apply(tracked.source, document.revision, QStringLiteral("label_add"), item.id,
                          {{QStringLiteral("label"), QStringLiteral("project/mustermark")}});
    QVERIFY2(result.ok, qPrintable(result.errorMessage));
    QVERIFY(result.source.contains("labels=project/mustermark"));
}

void DocumentEngineTest::movesWholeItemSubtree() {
    const QByteArray source =
        "- first\n"
        "    - child\n"
        "- second\n";
    DocumentEngine engine;
    const EditResult tracked = engine.track(source);
    QVERIFY(tracked.ok);
    const Document document = engine.parse(tracked.source);
    QVector<Node> topItems;
    for (const Node &node : document.nodes) {
        if (node.kind == NodeKind::Item && node.depth == 1)
            topItems.append(node);
    }
    QCOMPARE(topItems.size(), 2);
    const QString firstId = topItems.at(0).id;
    const EditResult moved = engine.apply(tracked.source, document.revision,
                                          QStringLiteral("move_after"), firstId,
                                          {{QStringLiteral("target"), topItems.at(1).id}});
    QVERIFY2(moved.ok, qPrintable(moved.errorMessage));
    const Document after = engine.parse(moved.source);
    const int firstPosition = moved.source.indexOf("- first");
    const int secondPosition = moved.source.indexOf("- second");
    QVERIFY(firstPosition > secondPosition);
    QVERIFY(moved.source.indexOf("- child", firstPosition) > firstPosition);
    QVERIFY(after.findNode(firstId) >= 0);
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
             QByteArray("- parent\n- child\n        - grandchild\n"));

    changed = engine.apply(
        nested, document.revision, QStringLiteral("promote"), nestedChild.ref,
        {{QStringLiteral("scope"), QStringLiteral("subtree")}});
    QVERIFY2(changed.ok, qPrintable(changed.errorMessage));
    QCOMPARE(changed.source,
             QByteArray("- parent\n- child\n    - grandchild\n"));

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
             QByteArray("- parent\n    - child\n    - grandchild\n"));

    changed = engine.apply(
        siblings, document.revision, QStringLiteral("demote"), topChild.ref,
        {{QStringLiteral("scope"), QStringLiteral("subtree")}});
    QVERIFY2(changed.ok, qPrintable(changed.errorMessage));
    QCOMPARE(changed.source,
             QByteArray("- parent\n    - child\n        - grandchild\n"));
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

void DocumentEngineTest::repairsDuplicateIds() {
    const QByteArray source =
        "<!-- mustermark:tracking version=1 -->\n\n"
        "<!-- mustermark:list id=lst_same -->\n"
        "- first\n"
        "  <!-- mustermark:item id=itm_same -->\n"
        "- second\n"
        "  <!-- mustermark:item id=itm_same -->\n";
    DocumentEngine engine;
    QVERIFY(!engine.parse(source).diagnostics.isEmpty());
    const EditResult repaired = engine.repair(source);
    QVERIFY(repaired.ok);
    QVERIFY(engine.parse(repaired.source).diagnostics.isEmpty());
}

void DocumentEngineTest::untrackRemovesOnlyMetadata() {
    const QByteArray source = "# Tasks\n\n- [ ] first\n- second\n";
    DocumentEngine engine;
    const EditResult tracked = engine.track(source);
    const EditResult plain = engine.untrack(tracked.source);
    QVERIFY(plain.ok);
    QCOMPARE(plain.source, source);
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
