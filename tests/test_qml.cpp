#include "documentcontroller.h"
#include "markdownhighlighter.h"

#include <QGuiApplication>
#include <QImage>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QtTest>

class QmlUiTest : public QObject {
    Q_OBJECT

private slots:
    void continuesListWhileTyping();
    void typesSelectsAndReorganises();
};

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
    QVERIFY2(root->property("selectedIndex").toInt() >= 0,
             qPrintable(QStringLiteral("cursor=%1 nodes=%2")
                            .arg(source->property("cursorPosition").toInt())
                            .arg(controller.nodes().size())));
    const QImage structuralFrame = window->grabWindow();
    QVERIFY(!structuralFrame.isNull());
    QVERIFY(structuralFrame.save(QStringLiteral("/tmp/mustermark-structural-test.png")));

    auto *moveDown = root->findChild<QQuickItem *>(QStringLiteral("moveDownAction"));
    QVERIFY(moveDown);
    QVERIFY(moveDown->property("visible").toBool());
    QVERIFY(QMetaObject::invokeMethod(moveDown, "triggered"));
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

    auto *taskAction = root->findChild<QQuickItem *>(QStringLiteral("taskAction"));
    QVERIFY(taskAction);
    QTRY_VERIFY(taskAction->property("visible").toBool());
    QVERIFY(QMetaObject::invokeMethod(taskAction, "triggered"));
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

    source->setProperty("cursorPosition", 2);
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(root->property("commandMode").toBool());
    const QImage headingFrame = window->grabWindow();
    QVERIFY(!headingFrame.isNull());
    QVERIFY(headingFrame.save(QStringLiteral("/tmp/mustermark-heading-test.png")));
    QTest::keyClick(window, Qt::Key_L, Qt::ShiftModifier);
    QTRY_VERIFY(controller.source().startsWith(QStringLiteral("## Alpha")));
    QVERIFY(controller.source().contains(QStringLiteral("### Child")));
}

int main(int argc, char **argv) {
    QGuiApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("mustermark-qml-tests"));
    QmlUiTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_qml.moc"
