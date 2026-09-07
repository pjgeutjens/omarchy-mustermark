#include "documentcontroller.h"
#include "httpserver.h"
#include "markdownhighlighter.h"
#include "filedocument.h"

#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QClipboard>
#include <QImage>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>

class QmlUiTest : public QObject {
    Q_OBJECT

private slots:
    void visualModeSharesWindowAndSelection();
    void linkedGuiKeepsHeadingIdentity();
    void startsInModeForDocumentKind();
    void entersInsertAtSelectedStructure();
    void continuesListWhileTyping();
    void opensMatchingSiblingsTransactionally();
    void appendsAndPrependsCurrentList();
    void numbersAppendedOrderedItems();
    void deletesSelectedListItem();
    void deletesAndUndoesSelectedStructures();
    void footerFitsNarrowWindow();
    void blocksHeadingFromOvertakingChild();
    void typesSelectsAndReorganises();
    void selectsAndMovesWholeList();
    void opensRecentWithKeyboard();
    void attachesAndRemovesClipboardImage();
    void resolvesConflictAndRestoresRecovery();
    void undoKeepsUnsavedRecovery();
};

void QmlUiTest::visualModeSharesWindowAndSelection() {
    QTemporaryDir dir;
    const auto path = dir.filePath("visual.md");
    QVERIFY(Mustermark::FileDocument::writeAtomic(path, "# Heading\n\n- first\n- second\n").ok);
    DocumentController controller;
    QVERIFY(controller.loadFile(QUrl::fromLocalFile(path)));
    Mustermark::DocumentHttpServer server(&controller);
    QVERIFY(server.listen());
    MarkdownHighlighter highlighter;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("documentController", &controller);
    engine.rootContext()->setContextProperty("markdownHighlighter", &highlighter);
    engine.rootContext()->setContextProperty("previewUrl", QUrl(QString("http://127.0.0.1:%1/#preview").arg(server.port())));
    engine.load(QUrl("qrc:/qml/Main.qml"));
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    QVERIFY(window);
    QTest::qWait(50);
    QVERIFY(QMetaObject::invokeMethod(window, "selectAtPosition", Q_ARG(QVariant, controller.source().indexOf("second")), Q_ARG(QVariant, false)));
    QTest::keyClick(window, Qt::Key_V, Qt::ShiftModifier);
    QTRY_VERIFY(window->property("visualMode").toBool());
    auto *pane = window->findChild<QObject *>("previewWindow");
    QVERIFY(pane);
    QTRY_VERIFY_WITH_TIMEOUT(pane->property("selectionReady").toBool(), 5000);
    QCOMPARE(pane->property("selectedLine").toInt(), 4);
    QVERIFY(window->grabWindow().save("/tmp/mustermark-visual-mode-test.png"));
    QCOMPARE(engine.rootObjects().size(), 1);
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!window->property("visualMode").toBool());
    QCOMPARE(window->property("selectedNode").toMap().value("text").toString(), QString("second"));
    QTest::keyClick(window, Qt::Key_I);
    QTRY_VERIFY(!window->property("commandMode").toBool());
    auto *source = window->findChild<QQuickItem *>("sourceArea");
    QCOMPARE(source->property("cursorPosition").toInt(), controller.source().indexOf("second"));
}

void QmlUiTest::linkedGuiKeepsHeadingIdentity() {
    QTemporaryDir directory;
    const auto path = directory.filePath("linked.muster.md");
    QVERIFY(Mustermark::FileDocument::writeAtomic(path, "# Work\n\n- task\n").ok);
    Mustermark::DocumentSession api;
    QVERIFY(Mustermark::FileDocument::readLinked(path, api, true).ok);
    const auto id = api.document().nodes[0].durableId;
    DocumentController controller;
    QVERIFY(controller.loadFile(QUrl::fromLocalFile(path)));
    const auto heading = controller.nodes()[0].toMap().value("id").toString();
    QVERIFY(controller.applyAction("edit", heading, {}, {}, "Renamed"));
    QVERIFY(Mustermark::FileDocument::readLinked(path, api).ok);
    QCOMPARE(api.document().nodes[0].durableId, id);
    controller.beginSourceEdit();
    controller.updateSource("# Renamed with draft\n\n- task\n");
    QVERIFY(Mustermark::FileDocument::readLinked(path, api).ok);
    QVERIFY(!api.document().source.contains("draft"));
    controller.endSourceEdit();
    QVERIFY(controller.save());
    QVERIFY(Mustermark::FileDocument::readLinked(path, api).ok);
    QCOMPARE(api.document().nodes[0].durableId, id);
    const auto copy = directory.filePath("copy.muster.md");
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(copy)));
    QVERIFY(controller.save());
    Mustermark::DocumentSession copied;
    QVERIFY(Mustermark::FileDocument::readLinked(copy, copied, true).ok);
    QVERIFY(copied.document().documentId != api.document().documentId);
}

void QmlUiTest::startsInModeForDocumentKind() {
    {
        DocumentController controller;
        MarkdownHighlighter highlighter;
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("documentController"), &controller);
        engine.rootContext()->setContextProperty(QStringLiteral("markdownHighlighter"), &highlighter);
        engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
        QCOMPARE(engine.rootObjects().size(), 1);
        auto *root = engine.rootObjects().constFirst();
        QTRY_VERIFY(!root->property("commandMode").toBool());
    }

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("existing.md"));
    QFile document(path);
    QVERIFY(document.open(QIODevice::WriteOnly));
    QCOMPARE(document.write("# Existing\n\n- one\n"), qint64(18));
    document.close();

    DocumentController controller;
    QVERIFY(controller.loadFile(QUrl::fromLocalFile(path)));
    MarkdownHighlighter highlighter;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("documentController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("markdownHighlighter"), &highlighter);
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    QCOMPARE(engine.rootObjects().size(), 1);
    auto *root = engine.rootObjects().constFirst();
    QTRY_VERIFY(root->property("commandMode").toBool());
    QTRY_COMPARE(root->property("selectedIndex").toInt(), 0);
    QCOMPARE(root->property("selectedNode").toMap().value(QStringLiteral("text")).toString(),
             QStringLiteral("Existing"));
}

void QmlUiTest::entersInsertAtSelectedStructure() {
    DocumentController controller;
    controller.updateSource(QStringLiteral("# Heading\n\n- first\n- second\n"));
    MarkdownHighlighter highlighter;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("documentController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("markdownHighlighter"), &highlighter);
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    QCOMPARE(engine.rootObjects().size(), 1);

    auto *root = engine.rootObjects().constFirst();
    auto *window = qobject_cast<QQuickWindow *>(root);
    QVERIFY(window);
    auto *source = root->findChild<QQuickItem *>(QStringLiteral("sourceArea"));
    QVERIFY(source);
    QTest::qWait(50);
    source->forceActiveFocus();
    source->setProperty("cursorPosition",
                        source->property("text").toString().indexOf(QStringLiteral("first")));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(root->property("commandMode").toBool());
    QTRY_COMPARE(root->property("selectedNode").toMap().value(QStringLiteral("text")).toString(),
                 QStringLiteral("first"));

    QTest::keyClick(window, Qt::Key_J);
    QTRY_COMPARE(root->property("selectedNode").toMap().value(QStringLiteral("text")).toString(),
                 QStringLiteral("second"));
    QTest::keyClick(window, Qt::Key_I);
    QTRY_VERIFY(!root->property("commandMode").toBool());
    QCOMPARE(source->property("cursorPosition").toInt(),
             source->property("text").toString().indexOf(QStringLiteral("second")));

    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(root->property("commandMode").toBool());
    QTest::keyClick(window, Qt::Key_K);
    QTest::keyClick(window, Qt::Key_K);
    QTest::keyClick(window, Qt::Key_K);
    QTRY_COMPARE(root->property("selectedNode").toMap().value(QStringLiteral("kind")).toString(),
                 QStringLiteral("heading"));
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_VERIFY(!root->property("commandMode").toBool());
    QCOMPARE(source->property("cursorPosition").toInt(),
             source->property("text").toString().indexOf(QStringLiteral("Heading")));
}

void QmlUiTest::appendsAndPrependsCurrentList() {
    DocumentController controller;
    controller.updateSource(QStringLiteral(
        "# Section\n\n- one\n- two\n\nParagraph after list.\n"));
    MarkdownHighlighter highlighter;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("documentController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("markdownHighlighter"), &highlighter);
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    QCOMPARE(engine.rootObjects().size(), 1);

    auto *root = engine.rootObjects().constFirst();
    auto *window = qobject_cast<QQuickWindow *>(root);
    QVERIFY(window);
    auto *source = root->findChild<QQuickItem *>(QStringLiteral("sourceArea"));
    QVERIFY(source);
    const auto typeText = [window](const QString &text) {
        for (const QChar character : text)
            QTest::keyClick(window, character.toLatin1());
    };

    QTest::qWait(50);
    source->setProperty("cursorPosition",
                        source->property("text").toString().indexOf(QStringLiteral("one")));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(root->property("commandMode").toBool());
    QCOMPARE(root->property("selectedNode").toMap().value(QStringLiteral("kind")).toString(),
             QStringLiteral("item"));

    QTest::keyClick(window, Qt::Key_A);
    QTRY_VERIFY(!root->property("commandMode").toBool());
    QVERIFY2(source->property("text").toString().contains(
                 QStringLiteral("- two\n- \n\nParagraph after list.")),
             qPrintable(source->property("text").toString()));
    typeText(QStringLiteral("three"));
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_VERIFY(root->property("commandMode").toBool());
    QTest::qWait(200);
    QVERIFY2(controller.source().indexOf(QStringLiteral("- three")) >
                 controller.source().indexOf(QStringLiteral("- two")),
             qPrintable(controller.source()));
    QVERIFY2(controller.source().contains(
                 QStringLiteral("- two\n- three\n\nParagraph after list.")),
             qPrintable(controller.source()));

    QTest::keyClick(window, Qt::Key_Return, Qt::ControlModifier);
    QTRY_VERIFY(!root->property("commandMode").toBool());
    typeText(QStringLiteral("four"));
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_VERIFY(root->property("commandMode").toBool());
    QTRY_VERIFY(controller.source().indexOf(QStringLiteral("- four")) >
                controller.source().indexOf(QStringLiteral("- three")));
    QVERIFY2(controller.source().contains(
                 QStringLiteral("- three\n- four\n\nParagraph after list.")),
             qPrintable(controller.source()));

    QTest::keyClick(window, Qt::Key_Left);
    QTRY_COMPARE(root->property("selectedNode").toMap().value(QStringLiteral("kind")).toString(),
                 QStringLiteral("list"));
    QTest::keyClick(window, Qt::Key_A, Qt::ShiftModifier);
    QTRY_VERIFY(!root->property("commandMode").toBool());
    typeText(QStringLiteral("zero"));
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_VERIFY(root->property("commandMode").toBool());
    QTRY_VERIFY(controller.source().indexOf(QStringLiteral("- zero")) <
                controller.source().indexOf(QStringLiteral("- one")));
}

void QmlUiTest::numbersAppendedOrderedItems() {
    DocumentController controller;
    controller.updateSource(QStringLiteral(
        "# Section\n\n"
        "1. Review the captured result\n"
        "   ![Review reference](example.png)\n"
        "2. Add delivery notes\n"
        "3. Approve or return the item\n\n"
        "Paragraph after list.\n"));
    MarkdownHighlighter highlighter;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("documentController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("markdownHighlighter"), &highlighter);
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    QCOMPARE(engine.rootObjects().size(), 1);

    auto *root = engine.rootObjects().constFirst();
    auto *window = qobject_cast<QQuickWindow *>(root);
    QVERIFY(window);
    auto *source = root->findChild<QQuickItem *>(QStringLiteral("sourceArea"));
    QVERIFY(source);
    QTest::qWait(50);
    source->setProperty("cursorPosition",
                        source->property("text").toString().indexOf(
                            QStringLiteral("Review the captured result")));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_COMPARE(root->property("selectedNode").toMap().value(QStringLiteral("kind")).toString(),
                 QStringLiteral("item"));

    QTest::keyClick(window, Qt::Key_A);
    QTRY_VERIFY(!root->property("commandMode").toBool());
    QVERIFY2(source->property("text").toString().contains(
                 QStringLiteral("3. Approve or return the item\n4. \n\nParagraph after list.")),
             qPrintable(source->property("text").toString()));
    for (const QChar character : QStringLiteral("Final"))
        QTest::keyClick(window, character.toLatin1());
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_VERIFY(root->property("commandMode").toBool());
    QTRY_VERIFY(controller.source().contains(
        QStringLiteral("3. Approve or return the item\n4. Final\n\nParagraph after list.")));

    QTest::keyClick(window, Qt::Key_A, Qt::ShiftModifier);
    QTRY_VERIFY(!root->property("commandMode").toBool());
    for (const QChar character : QStringLiteral("First"))
        QTest::keyClick(window, character.toLatin1());
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_VERIFY(root->property("commandMode").toBool());
    QVERIFY2(controller.source().contains(
                 QStringLiteral("1. First\n"
                                "2. Review the captured result\n"
                                "   ![Review reference](example.png)\n"
                                "3. Add delivery notes\n"
                                "4. Approve or return the item\n"
                                "5. Final\n\nParagraph after list.")),
             qPrintable(controller.source()));
}

void QmlUiTest::blocksHeadingFromOvertakingChild() {
    DocumentController controller;
    const QString original = QStringLiteral("## Parent\n\n### Child\n\nBody.\n");
    controller.updateSource(original);
    MarkdownHighlighter highlighter;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("documentController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("markdownHighlighter"), &highlighter);
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    QCOMPARE(engine.rootObjects().size(), 1);
    auto *root = engine.rootObjects().constFirst();
    auto *window = qobject_cast<QQuickWindow *>(root);
    QVERIFY(window);
    auto *source = root->findChild<QQuickItem *>(QStringLiteral("sourceArea"));
    QVERIFY(source);
    QTest::qWait(50);
    source->forceActiveFocus();
    source->setProperty("cursorPosition",
                        source->property("text").toString().indexOf(QStringLiteral("Parent")));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_COMPARE(root->property("selectedNode").toMap().value(QStringLiteral("text")).toString(),
                 QStringLiteral("Parent"));

    QTest::keyClick(window, Qt::Key_L, Qt::ShiftModifier);
    QTest::qWait(50);
    QCOMPARE(controller.source(), original);

    QTest::keyClick(window, Qt::Key_L, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_COMPARE(controller.source(),
                 QStringLiteral("### Parent\n\n#### Child\n\nBody.\n"));
}

void QmlUiTest::deletesSelectedListItem() {
    DocumentController controller;
    const QString original = QStringLiteral(
        "# Section\n\n- keep\n- remove\n  - child\n- after\n");
    controller.updateSource(original);
    MarkdownHighlighter highlighter;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("documentController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("markdownHighlighter"), &highlighter);
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    QCOMPARE(engine.rootObjects().size(), 1);

    auto *root = engine.rootObjects().constFirst();
    auto *window = qobject_cast<QQuickWindow *>(root);
    QVERIFY(window);
    auto *source = root->findChild<QQuickItem *>(QStringLiteral("sourceArea"));
    QVERIFY(source);
    QTest::qWait(50);
    source->setProperty("cursorPosition",
                        source->property("text").toString().indexOf(QStringLiteral("remove")));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_COMPARE(root->property("selectedNode").toMap().value(QStringLiteral("text")).toString(),
                 QStringLiteral("remove"));

    QTest::keyClick(window, Qt::Key_D);
    QTRY_VERIFY(!controller.source().contains(QStringLiteral("remove")));
    QVERIFY(!controller.source().contains(QStringLiteral("child")));
    QVERIFY(controller.source().contains(QStringLiteral("keep")));
    QVERIFY(controller.source().contains(QStringLiteral("after")));
    QVERIFY(controller.canUndo());

    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    QTRY_COMPARE(controller.source(), original);
}

void QmlUiTest::deletesAndUndoesSelectedStructures() {
    DocumentController controller;
    const QString original = QStringLiteral(
        "# Keep\n\n"
        "Remove this paragraph.\n\n"
        "- first item\n"
        "- second item\n\n"
        "## Remove section\n\n"
        "Section body.\n\n"
        "- section child\n\n"
        "## Keep section\n\n"
        "Keep this body.\n");
    controller.updateSource(original);
    MarkdownHighlighter highlighter;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("documentController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("markdownHighlighter"), &highlighter);
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    QCOMPARE(engine.rootObjects().size(), 1);

    auto *root = engine.rootObjects().constFirst();
    auto *window = qobject_cast<QQuickWindow *>(root);
    QVERIFY(window);
    auto *source = root->findChild<QQuickItem *>(QStringLiteral("sourceArea"));
    QVERIFY(source);
    QTest::qWait(50);

    const auto selectFromSource = [&](const QString &text) {
        if (root->property("commandMode").toBool())
            QVERIFY(QMetaObject::invokeMethod(root, "enterInsert",
                                              Q_ARG(QVariant, QVariant(0))));
        QTRY_VERIFY(!root->property("commandMode").toBool());
        source->forceActiveFocus();
        const int position = source->property("text").toString().indexOf(text);
        QVERIFY(position >= 0);
        source->setProperty("cursorPosition", position);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(root->property("commandMode").toBool());
    };

    selectFromSource(QStringLiteral("Remove this paragraph"));
    QTRY_COMPARE(root->property("selectedNode").toMap().value(QStringLiteral("kind")).toString(),
                 QStringLiteral("block"));
    QTest::keyClick(window, Qt::Key_D);
    QTRY_VERIFY(!controller.source().contains(QStringLiteral("Remove this paragraph")));
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    QTRY_COMPARE(controller.source(), original);

    selectFromSource(QStringLiteral("first item"));
    QTRY_COMPARE(root->property("selectedNode").toMap().value(QStringLiteral("kind")).toString(),
                 QStringLiteral("item"));
    QTest::keyClick(window, Qt::Key_Left);
    QTRY_COMPARE(root->property("selectedNode").toMap().value(QStringLiteral("kind")).toString(),
                 QStringLiteral("list"));
    QTest::keyClick(window, Qt::Key_Delete);
    QTRY_VERIFY(!controller.source().contains(QStringLiteral("first item")));
    QVERIFY(!controller.source().contains(QStringLiteral("second item")));
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    QTRY_COMPARE(controller.source(), original);

    selectFromSource(QStringLiteral("Remove section"));
    QTRY_COMPARE(root->property("selectedNode").toMap().value(QStringLiteral("kind")).toString(),
                 QStringLiteral("heading"));
    QTest::keyClick(window, Qt::Key_D);
    QTRY_VERIFY(!controller.source().contains(QStringLiteral("Remove section")));
    QVERIFY(!controller.source().contains(QStringLiteral("Section body")));
    QVERIFY(!controller.source().contains(QStringLiteral("section child")));
    QVERIFY(controller.source().contains(QStringLiteral("Keep section")));
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    QTRY_COMPARE(controller.source(), original);
}

void QmlUiTest::footerFitsNarrowWindow() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("a-document-with-a-long-name.md"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray contents = "# A document with a long name\n\nBody.\n";
    QCOMPARE(file.write(contents), contents.size());
    file.close();

    DocumentController controller;
    controller.discardRecovery();
    QVERIFY(controller.loadFile(QUrl::fromLocalFile(path)));
    MarkdownHighlighter highlighter;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("documentController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("markdownHighlighter"), &highlighter);
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    QCOMPARE(engine.rootObjects().size(), 1);

    auto *root = engine.rootObjects().constFirst();
    auto *window = qobject_cast<QQuickWindow *>(root);
    QVERIFY(window);
    window->setWidth(320);
    window->setHeight(240);
    QTest::qWait(50);

    auto *footer = root->findChild<QQuickItem *>(QStringLiteral("editorFooter"));
    auto *summary = root->findChild<QQuickItem *>(QStringLiteral("documentSummaryLabel"));
    auto *visual = root->findChild<QQuickItem *>(QStringLiteral("visualAction"));
    auto *cursor = root->findChild<QQuickItem *>(QStringLiteral("cursorPositionLabel"));
    QVERIFY(footer);
    QVERIFY(summary);
    QVERIFY(visual);
    QVERIFY(cursor);
    QVERIFY(summary->property("text").toString().startsWith(path + QStringLiteral(" · ")));
    for (QQuickItem *item : {summary, visual, cursor}) {
        const QRectF bounds = item->mapRectToItem(footer, item->boundingRect());
        QVERIFY2(bounds.left() >= -0.5 && bounds.right() <= footer->width() + 0.5,
                 qPrintable(QStringLiteral("%1 outside footer: %2..%3 of %4")
                                .arg(item->objectName())
                                .arg(bounds.left()).arg(bounds.right()).arg(footer->width())));
    }
}

void QmlUiTest::undoKeepsUnsavedRecovery() {
    DocumentController controller;
    controller.newDocument();
    controller.discardRecovery();
    controller.updateSource(QStringLiteral("# Unsaved\n\n- [ ] first\n- [ ] second\n"));
    const QString original = controller.source();
    const QVariantList nodes = controller.nodes();
    const auto item = std::find_if(
        nodes.cbegin(), nodes.cend(), [](const QVariant &value) {
            return value.toMap().value(QStringLiteral("text")) == QStringLiteral("first");
        });
    QVERIFY(item != nodes.cend());
    QVERIFY(controller.applyAction(QStringLiteral("toggle_task"),
                                   item->toMap().value(QStringLiteral("ref")).toString()));
    QVERIFY(controller.canUndo());
    const QString checked = controller.source();
    controller.beginSourceEdit();
    controller.updateSource(checked + QStringLiteral("- typed in Insert mode\n"));
    controller.endSourceEdit();
    QVERIFY(controller.undoDocumentChange());
    QCOMPARE(controller.source(), checked);
    QVERIFY(controller.undoDocumentChange());
    QCOMPARE(controller.source(), original);
    QVERIFY(controller.modified());
    DocumentController recovered;
    recovered.newDocument();
    QVERIFY(recovered.recoveryAvailable());
    QVERIFY(recovered.restoreRecovery());
    QCOMPARE(recovered.source(), original);
    recovered.discardRecovery();
}

void QmlUiTest::resolvesConflictAndRestoresRecovery() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("conflict.md"));
    const QString copyPath = directory.filePath(QStringLiteral("local-copy.md"));
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("# Original\n"), qint64(11));
    }

    DocumentController controller;
    QVERIFY(controller.loadFile(QUrl::fromLocalFile(path)));
    controller.updateSource(QStringLiteral("# Local draft\n"));
    QVERIFY(controller.modified());
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write("# External edit\n"), qint64(16));
    }
    controller.checkExternalChange();
    QVERIFY(controller.conflict());
    QVERIFY(!controller.save());
    QVERIFY(controller.saveCopy(QUrl::fromLocalFile(copyPath)));
    QFile copy(copyPath);
    QVERIFY(copy.open(QIODevice::ReadOnly));
    QCOMPARE(copy.readAll(), QByteArray("# Local draft\n"));

    DocumentController recovered;
    QVERIFY(recovered.loadFile(QUrl::fromLocalFile(path)));
    QVERIFY(recovered.recoveryAvailable());
    QVERIFY(recovered.restoreRecovery());
    QVERIFY(recovered.modified());
    QCOMPARE(recovered.source(), QStringLiteral("# Local draft\n"));
    QFile disk(path);
    QVERIFY(disk.open(QIODevice::ReadOnly));
    QCOMPARE(disk.readAll(), QByteArray("# External edit\n"));

    QVERIFY(controller.reloadFromDisk());
    QVERIFY(!controller.conflict());
    QVERIFY(!controller.modified());
    QCOMPARE(controller.source(), QStringLiteral("# External edit\n"));
    QVERIFY(!controller.recoveryAvailable());
}

void QmlUiTest::continuesListWhileTyping() {
    DocumentController controller;
    controller.updateSource(QStringLiteral("- [x] done\n7) numbered\n"));
    MarkdownHighlighter highlighter;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("documentController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("markdownHighlighter"), &highlighter);
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    QCOMPARE(engine.rootObjects().size(), 1);

    auto *root = engine.rootObjects().constFirst();
    auto *window = qobject_cast<QQuickWindow *>(root);
    QVERIFY(window);
    auto *source = root->findChild<QQuickItem *>(QStringLiteral("sourceArea"));
    QVERIFY(source);
    auto *modeLabel = root->findChild<QQuickItem *>(QStringLiteral("modeLabel"));
    QVERIFY(modeLabel);
    QCOMPARE(modeLabel->property("color").value<QColor>(),
             controller.theme().value(QStringLiteral("selectionForeground")).value<QColor>());
    source->forceActiveFocus();
    QTest::qWait(20);

    QString text = source->property("text").toString();
    source->setProperty("cursorPosition", text.indexOf(QLatin1Char('\n')));
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_VERIFY(source->property("text").toString().contains(
        QStringLiteral("- [x] done\n- [ ] \n7) numbered")));

    text = source->property("text").toString();
    const int orderedEnd = text.indexOf(QLatin1Char('\n'), text.indexOf(QStringLiteral("7)")));
    source->setProperty("cursorPosition", orderedEnd);
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_VERIFY(source->property("text").toString().contains(
        QStringLiteral("7) numbered\n8) ")));
}

void QmlUiTest::opensMatchingSiblingsTransactionally() {
    DocumentController controller;
    const QString original = QStringLiteral(
        "# Alpha\n\n"
        "## Child\n\n"
        "- [x] finished\n"
        "  continued\n\n"
        "# Beta\n\n"
        "7) numbered\n");
    controller.updateSource(original);
    MarkdownHighlighter highlighter;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("documentController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("markdownHighlighter"), &highlighter);
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    QCOMPARE(engine.rootObjects().size(), 1);

    auto *root = engine.rootObjects().constFirst();
    auto *window = qobject_cast<QQuickWindow *>(root);
    QVERIFY(window);
    auto *source = root->findChild<QQuickItem *>(QStringLiteral("sourceArea"));
    QVERIFY(source);
    const auto typeText = [window](const QString &text) {
        for (const QChar character : text) {
            if (character == QLatin1Char(' ')) {
                QTest::keyClick(window, Qt::Key_Space);
                continue;
            }
            const bool upper = character.isUpper();
            const int key = Qt::Key_A + character.toLower().unicode() - QLatin1Char('a').unicode();
            QTest::keyClick(window, static_cast<Qt::Key>(key),
                            upper ? Qt::ShiftModifier : Qt::NoModifier);
        }
    };
    source->forceActiveFocus();
    QTest::qWait(50);

    source->setProperty("cursorPosition", original.indexOf(QStringLiteral("finished")));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(root->property("commandMode").toBool());
    QTest::keyClick(window, Qt::Key_O);
    QTRY_VERIFY(!root->property("commandMode").toBool());
    QVERIFY(root->property("provisionalInsert").isValid());
    typeText(QStringLiteral("new task"));
    QTest::keyClick(window, Qt::Key_Return, Qt::ShiftModifier);
    typeText(QStringLiteral("detail"));
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_VERIFY(root->property("commandMode").toBool());
    QTRY_VERIFY(controller.source().contains(
        QStringLiteral("- [x] finished\n  continued\n- [ ] new task\n      detail\n")));

    QString text = source->property("text").toString();
    source->setProperty("cursorPosition", text.indexOf(QStringLiteral("numbered")));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(root->property("commandMode").toBool());
    QTest::keyClick(window, Qt::Key_O, Qt::ShiftModifier);
    QTRY_VERIFY(!root->property("commandMode").toBool());
    typeText(QStringLiteral("discard me"));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(root->property("commandMode").toBool());
    QVERIFY(!controller.source().contains(QStringLiteral("discard me")));
    QVERIFY(controller.source().contains(QStringLiteral("7) numbered")));

    text = source->property("text").toString();
    QTest::keyClick(window, Qt::Key_I);
    QTRY_VERIFY(!root->property("commandMode").toBool());
    source->setProperty("cursorPosition", text.indexOf(QStringLiteral("Alpha")));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(root->property("commandMode").toBool());
    QTest::keyClick(window, Qt::Key_O);
    QTRY_VERIFY(!root->property("commandMode").toBool());
    typeText(QStringLiteral("After alpha"));
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_VERIFY(root->property("commandMode").toBool());
    const QString accepted = controller.source();
    const int child = accepted.indexOf(QStringLiteral("## Child"));
    const int afterAlpha = accepted.indexOf(QStringLiteral("# after alpha"));
    const int beta = accepted.indexOf(QStringLiteral("# Beta"));
    QVERIFY(child >= 0);
    QVERIFY2(afterAlpha > child, qPrintable(accepted));
    QVERIFY2(beta > afterAlpha, qPrintable(accepted));
}

void QmlUiTest::typesSelectsAndReorganises() {
    DocumentController controller;
    controller.updateSource(QStringLiteral(
        "# Alpha\n\n"
        "## Child\n\n"
        "- [ ] one\n"
        "- [ ] two\n"));
    MarkdownHighlighter highlighter;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("documentController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("markdownHighlighter"), &highlighter);
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    QCOMPARE(engine.rootObjects().size(), 1);

    auto *root = engine.rootObjects().constFirst();
    auto *window = qobject_cast<QQuickWindow *>(root);
    QVERIFY(window);
    auto *source = root->findChild<QQuickItem *>(QStringLiteral("sourceArea"));
    QVERIFY(source);
    auto *modeLabel = root->findChild<QQuickItem *>(QStringLiteral("modeLabel"));
    QVERIFY(modeLabel);

    QTest::qWait(50);
    QVERIFY(!root->property("commandMode").toBool());

    const QString initialText = source->property("text").toString();
    const int itemPosition = initialText.indexOf(QStringLiteral("one"));
    QVERIFY(itemPosition > 0);
    source->forceActiveFocus();
    QTest::qWait(20);
    source->setProperty("cursorPosition", itemPosition);
    QCOMPARE(source->property("cursorPosition").toInt(), itemPosition);

    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(root->property("commandMode").toBool());
    QCOMPARE(modeLabel->property("color").value<QColor>(),
             controller.theme().value(QStringLiteral("background")).value<QColor>());
    QVERIFY2(root->property("selectedIndex").toInt() >= 0,
             qPrintable(QStringLiteral("cursor=%1 nodes=%2")
                            .arg(source->property("cursorPosition").toInt())
                            .arg(controller.nodes().size())));
    const int itemIndex = root->property("selectedIndex").toInt();
    const QPoint hoverPoint = source->mapToScene(QPointF(100, 35)).toPoint();
    QTest::mouseMove(window, hoverPoint);
    QTest::qWait(30);
    QCOMPARE(root->property("selectedIndex").toInt(), itemIndex);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, hoverPoint);
    QTRY_VERIFY(root->property("commandMode").toBool());
    QTRY_VERIFY(root->property("selectedIndex").toInt() != itemIndex);
    root->setProperty("selectedIndex", itemIndex);
    QTest::keyClick(window, Qt::Key_Question);
    QTRY_VERIFY(root->property("keyHelpVisible").toBool());
    QCOMPARE(root->property("firstKeyHelpBinding").toString(), QStringLiteral("Space"));
    const QImage keyHelpFrame = window->grabWindow();
    QVERIFY(!keyHelpFrame.isNull());
    QVERIFY(keyHelpFrame.save(QStringLiteral("/tmp/mustermark-keybindings-test.png")));
    QTest::keyClick(window, Qt::Key_Question);
    QTRY_VERIFY(!root->property("keyHelpVisible").toBool());
    root->setProperty("selectedIndex", itemIndex);
    const QImage structuralFrame = window->grabWindow();
    QVERIFY(!structuralFrame.isNull());
    QVERIFY(structuralFrame.save(QStringLiteral("/tmp/mustermark-structural-test.png")));

    const QString beforeMove = controller.source();
    QTest::keyClick(window, Qt::Key_J, Qt::ShiftModifier);
    QTRY_VERIFY(controller.source().indexOf(QStringLiteral("one")) >
                controller.source().indexOf(QStringLiteral("two")));
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    QTRY_COMPARE(controller.source(), beforeMove);
    QTRY_COMPARE(root->property("selectedNode").toMap().value(QStringLiteral("text")).toString(),
                 QStringLiteral("one"));
    QTest::keyClick(window, Qt::Key_J, Qt::ShiftModifier);
    QTRY_VERIFY(controller.source().indexOf(QStringLiteral("one")) >
                controller.source().indexOf(QStringLiteral("two")));

    const int selectedAfterMove = root->property("selectedIndex").toInt();
    QVERIFY(selectedAfterMove >= 0 && selectedAfterMove < controller.nodes().size());
    const QVariantMap selectedMap = controller.nodes().at(selectedAfterMove).toMap();
    QVERIFY2(selectedMap.value(QStringLiteral("task")).toBool(),
             qPrintable(QStringLiteral("selected kind=%1 text=%2 index=%3")
                            .arg(selectedMap.value(QStringLiteral("kind")).toString())
                            .arg(selectedMap.value(QStringLiteral("text")).toString())
                            .arg(selectedAfterMove)));

    QTest::keyClick(window, Qt::Key_Space);
    QTest::qWait(50);
    QVERIFY2(controller.source().contains(QStringLiteral("- [x] one")),
             qPrintable(controller.status() + QLatin1Char('\n') + controller.source()));

    QTest::keyClick(window, Qt::Key_I);
    QTRY_VERIFY(!root->property("commandMode").toBool());
    source->forceActiveFocus();
    source->setProperty("cursorPosition", source->property("length").toInt());
    QTest::keyClick(window, Qt::Key_T);
    QTest::keyClick(window, Qt::Key_Y);
    QTest::keyClick(window, Qt::Key_P);
    QTest::keyClick(window, Qt::Key_E);
    QTest::keyClick(window, Qt::Key_D);
    QTRY_VERIFY(source->property("text").toString().endsWith(QStringLiteral("typed")));

    source->setProperty("cursorPosition",
                        source->property("text").toString().indexOf(QStringLiteral("Child")));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(root->property("commandMode").toBool());
    const QImage headingFrame = window->grabWindow();
    QVERIFY(!headingFrame.isNull());
    QVERIFY(headingFrame.save(QStringLiteral("/tmp/mustermark-heading-test.png")));

    const QString beforeHeadingLevel = controller.source();
    QVERIFY(QMetaObject::invokeMethod(root, "setSelectedHeadingLevel",
                                      Q_ARG(QVariant, QVariant(3))));
    QTRY_VERIFY(controller.source().contains(QStringLiteral("### Child")));
    QTRY_COMPARE(root->property("selectedNode").toMap().value(QStringLiteral("text")).toString(),
                 QStringLiteral("Child"));
    QTRY_COMPARE(root->property("firstKeyHelpBinding").toString(), QStringLiteral("Ctrl+Z"));
    QVERIFY(QMetaObject::invokeMethod(root, "setSelectedHeadingLevel",
                                      Q_ARG(QVariant, QVariant(4))));
    QTRY_VERIFY(controller.source().contains(QStringLiteral("#### Child")));
    QTRY_COMPARE(root->property("selectedNode").toMap().value(QStringLiteral("text")).toString(),
                 QStringLiteral("Child"));
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    QTRY_VERIFY(controller.source().contains(QStringLiteral("### Child")));
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    QTRY_COMPARE(controller.source(), beforeHeadingLevel);
    QTRY_VERIFY(root->property("commandMode").toBool());
    QTRY_COMPARE(root->property("selectedNode").toMap().value(QStringLiteral("text")).toString(),
                 QStringLiteral("Child"));

    QTest::keyClick(window, Qt::Key_K);
    QTRY_COMPARE(root->property("selectedNode").toMap().value(QStringLiteral("text")).toString(),
                 QStringLiteral("Alpha"));
    QTest::keyClick(window, Qt::Key_L, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_VERIFY(controller.source().startsWith(QStringLiteral("## Alpha")));
    QVERIFY(controller.source().contains(QStringLiteral("### Child")));
}

void QmlUiTest::selectsAndMovesWholeList() {
    DocumentController controller;
    controller.updateSource(QStringLiteral(
        "# Section\n\n"
        "Intro paragraph.\n\n"
        "- one\n"
        "- two\n"));
    MarkdownHighlighter highlighter;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("documentController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("markdownHighlighter"), &highlighter);
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    QCOMPARE(engine.rootObjects().size(), 1);

    auto *root = engine.rootObjects().constFirst();
    auto *window = qobject_cast<QQuickWindow *>(root);
    QVERIFY(window);
    auto *source = root->findChild<QQuickItem *>(QStringLiteral("sourceArea"));
    QVERIFY(source);
    source->forceActiveFocus();
    QTest::qWait(50);
    source->setProperty("cursorPosition",
                        source->property("text").toString().indexOf(QStringLiteral("one")));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(root->property("commandMode").toBool());
    QCOMPARE(root->property("selectedNode").toMap().value(QStringLiteral("kind")).toString(),
             QStringLiteral("item"));

    QTest::keyClick(window, Qt::Key_Left);
    QTRY_COMPARE(root->property("selectedNode").toMap()
                     .value(QStringLiteral("kind")).toString(), QStringLiteral("list"));
    QTest::keyClick(window, Qt::Key_K, Qt::ShiftModifier);
    QTRY_VERIFY(controller.source().indexOf(QStringLiteral("- one")) <
                controller.source().indexOf(QStringLiteral("Intro paragraph.")));
}

void QmlUiTest::opensRecentWithKeyboard() {
    QSettings settings;
    settings.remove(QStringLiteral("recentFiles"));
    settings.sync();

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString firstPath = directory.filePath(QStringLiteral("first.md"));
    const QString secondPath = directory.filePath(QStringLiteral("second.md"));
    QFile first(firstPath);
    QVERIFY(first.open(QIODevice::WriteOnly));
    QCOMPARE(first.write("# First\n"), 8);
    first.close();
    QFile second(secondPath);
    QVERIFY(second.open(QIODevice::WriteOnly));
    QCOMPARE(second.write("# Second\n"), 9);
    second.close();

    DocumentController controller;
    QVERIFY(controller.loadFile(QUrl::fromLocalFile(firstPath)));
    QVERIFY(controller.loadFile(QUrl::fromLocalFile(secondPath)));
    QCOMPARE(controller.recentFiles().size(), 2);
    QCOMPARE(controller.recentFiles().at(0).toMap().value(QStringLiteral("path")).toString(),
             QFileInfo(secondPath).canonicalFilePath());

    DocumentController restored;
    QCOMPARE(restored.recentFiles().size(), 2);
    MarkdownHighlighter highlighter;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("documentController"), &restored);
    engine.rootContext()->setContextProperty(QStringLiteral("markdownHighlighter"), &highlighter);
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    QCOMPARE(engine.rootObjects().size(), 1);

    auto *root = engine.rootObjects().constFirst();
    auto *window = qobject_cast<QQuickWindow *>(root);
    QVERIFY(window);
    QTest::qWait(50);
    QTest::keyClick(window, Qt::Key_O, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_VERIFY(root->property("recentVisible").toBool());
    const QImage recentFrame = window->grabWindow();
    QVERIFY(!recentFrame.isNull());
    QVERIFY(recentFrame.save(QStringLiteral("/tmp/mustermark-recent-test.png")));

    auto *recentList = root->findChild<QQuickItem *>(QStringLiteral("recentList"));
    QVERIFY(recentList);
    QCOMPARE(recentList->property("currentIndex").toInt(), 0);
    QTest::keyClick(window, Qt::Key_Down);
    QCOMPARE(recentList->property("currentIndex").toInt(), 1);
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_VERIFY(!root->property("recentVisible").toBool());
    QCOMPARE(restored.filePath(), QFileInfo(firstPath).canonicalFilePath());
    QTRY_VERIFY(root->property("commandMode").toBool());
    QCOMPARE(root->property("selectedNode").toMap().value(QStringLiteral("text")).toString(),
             QStringLiteral("First"));

    settings.remove(QStringLiteral("recentFiles"));
    settings.sync();
}

void QmlUiTest::attachesAndRemovesClipboardImage() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("image-notes.md"));
    QFile document(path);
    QVERIFY(document.open(QIODevice::WriteOnly));
    QVERIFY(document.write("# Images\n\n- [ ] describe this\n- [ ] second item\n") > 0);
    document.close();

    DocumentController controller;
    QVERIFY(controller.loadFile(QUrl::fromLocalFile(path)));
    QVERIFY(controller.enableTracking());
    const QVariantList initialNodes = controller.nodes();
    const auto itemIterator = std::find_if(
        initialNodes.cbegin(), initialNodes.cend(), [](const QVariant &value) {
            return value.toMap().value(QStringLiteral("kind")) == QStringLiteral("item");
        });
    QVERIFY(itemIterator != initialNodes.cend());
    const QVariantMap item = itemIterator->toMap();
    const QString identity = item.value(QStringLiteral("identity")).toString();
    QVERIFY(!identity.isEmpty());

    QImage image(4, 3, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(QStringLiteral("#cc6688")));
    QGuiApplication::clipboard()->setImage(image);

    MarkdownHighlighter highlighter;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("documentController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("markdownHighlighter"), &highlighter);
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    QCOMPARE(engine.rootObjects().size(), 1);

    auto *root = engine.rootObjects().constFirst();
    auto *window = qobject_cast<QQuickWindow *>(root);
    QVERIFY(window);
    auto *source = root->findChild<QQuickItem *>(QStringLiteral("sourceArea"));
    QVERIFY(source);
    QTRY_VERIFY(root->property("commandMode").toBool());
    QVERIFY(QMetaObject::invokeMethod(root, "enterInsert", Q_ARG(QVariant, QVariant(0))));
    QTRY_VERIFY(!root->property("commandMode").toBool());
    source->forceActiveFocus();
    QTest::qWait(50);
    source->setProperty("cursorPosition",
                        source->property("text").toString().indexOf(QStringLiteral("describe")));
    QTest::keyClick(window, Qt::Key_V, Qt::ControlModifier);
    QTRY_VERIFY(controller.source().contains(
        QStringLiteral("![clipboard image](image-notes.assets/")));
    QTest::keyClick(window, Qt::Key_V, Qt::ControlModifier);
    QTRY_VERIFY(controller.source().count(QStringLiteral("![clipboard image](image-notes.assets/")) == 2);

    const QVariantList updatedNodes = controller.nodes();
    const auto attachedIterator = std::find_if(
        updatedNodes.cbegin(), updatedNodes.cend(), [](const QVariant &value) {
            return value.toMap().value(QStringLiteral("kind")) == QStringLiteral("item");
        });
    QVERIFY(attachedIterator != updatedNodes.cend());
    const QVariantMap attached = attachedIterator->toMap();
    const QVariantList attachments = attached.value(QStringLiteral("attachments")).toList();
    QCOMPARE(attachments.size(), 2);
    QCOMPARE(controller.renderedHtml().count(QStringLiteral("class=\"mm-attachment\"")), 2);
    QCOMPARE(controller.renderedHtml().count(QStringLiteral("class=\"mm-attachment-icon\"")), 2);
    QCOMPARE(controller.previewPage().count(QStringLiteral("mm-attachment-preview")) >= 3, true);
    QVERIFY(controller.previewPage().contains(QStringLiteral(".mm-attachment.mm-open")));
    QVERIFY(!controller.previewPage().contains(QStringLiteral(".mm-attachment:hover")));
    const QString firstRelative = attachments.at(0).toMap().value(QStringLiteral("path")).toString();
    const QString secondRelative = attachments.at(1).toMap().value(QStringLiteral("path")).toString();
    QVERIFY(attachments.at(0).toMap().value(QStringLiteral("available")).toBool());
    QVERIFY(attachments.at(1).toMap().value(QStringLiteral("available")).toBool());
    QVERIFY(QFileInfo(directory.filePath(firstRelative)).isFile());
    QVERIFY(QFileInfo(directory.filePath(secondRelative)).isFile());

    QGuiApplication::clipboard()->setText(QStringLiteral(" pasted"));
    source->setProperty("cursorPosition",
                        source->property("text").toString().indexOf(QStringLiteral("describe this")) +
                            QStringLiteral("describe this").size());
    QTest::keyClick(window, Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_VERIFY(source->property("text").toString().contains(
        QStringLiteral("describe this pasted")));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(root->property("commandMode").toBool());
    QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_VERIFY(root->property("attachmentsVisible").toBool());
    const int selectedItem = root->property("selectedIndex").toInt();
    QCOMPARE(root->property("attachmentIndex").toInt(), 0);
    QTest::keyClick(window, Qt::Key_J);
    QTRY_COMPARE(root->property("attachmentIndex").toInt(), 1);
    QCOMPARE(root->property("selectedIndex").toInt(), selectedItem);
    QTest::keyClick(window, Qt::Key_K);
    QTRY_COMPARE(root->property("attachmentIndex").toInt(), 0);
    QCOMPARE(root->property("selectedIndex").toInt(), selectedItem);
    const QImage attachmentFrame = window->grabWindow();
    QVERIFY(!attachmentFrame.isNull());
    QVERIFY(attachmentFrame.save(QStringLiteral("/tmp/mustermark-attachments-test.png")));

    QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_VERIFY(!root->property("attachmentsVisible").toBool());
    QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_VERIFY(root->property("attachmentsVisible").toBool());
    QTest::keyClick(window, Qt::Key_D);
    QTRY_VERIFY(root->property("attachmentsVisible").toBool());
    QTRY_COMPARE(root->property("selectedNode").toMap()
                     .value(QStringLiteral("attachments")).toList().size(), 1);
    QVERIFY(!QFileInfo(directory.filePath(firstRelative)).exists());
    QTest::keyClick(window, Qt::Key_D);
    QTRY_VERIFY(!root->property("attachmentsVisible").toBool());
    QVERIFY(!controller.source().contains(firstRelative));
    QVERIFY(!controller.source().contains(secondRelative));
    QVERIFY(!QFileInfo(directory.filePath(secondRelative)).exists());
}

int main(int argc, char **argv) {
    QGuiApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("MustermarkTests"));
    QCoreApplication::setApplicationName(QStringLiteral("mustermark-qml-tests"));
    QmlUiTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_qml.moc"
