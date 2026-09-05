#include "cli.h"
#include "documentcontroller.h"
#include "markdownhighlighter.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>

int main(int argc, char *argv[]) {
    const QStringList earlyArguments = [&] {
        QStringList values;
        for (int index = 0; index < argc; ++index)
            values.append(QString::fromLocal8Bit(argv[index]));
        return values;
    }();

    if (Cli::isCommand(earlyArguments)) {
        QCoreApplication application(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("mustermark"));
        QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
        return Cli::run(application.arguments());
    }

    QGuiApplication application(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("Mustermark"));
    QGuiApplication::setOrganizationName(QStringLiteral("Mustermark"));
    QGuiApplication::setDesktopFileName(QStringLiteral("mustermark"));

    DocumentController controller;
    MarkdownHighlighter highlighter;
    if (application.arguments().size() > 1)
        controller.loadFile(QUrl::fromLocalFile(application.arguments().at(1)));

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("documentController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("markdownHighlighter"), &highlighter);
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    if (engine.rootObjects().isEmpty())
        return 1;
    return application.exec();
}
