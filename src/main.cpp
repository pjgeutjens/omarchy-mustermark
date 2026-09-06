#include "cli.h"
#include "documentcontroller.h"
#include "httpserver.h"
#include "markdownhighlighter.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QTextStream>

#include <memory>

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

    std::unique_ptr<Mustermark::DocumentHttpServer> documentServer;
    QString stylePath;
    for (const QString &argument : application.arguments().mid(2)) {
        if (argument.startsWith(QStringLiteral("--html-style=")))
            stylePath = argument.mid(QStringLiteral("--html-style=").size());
    }
    for (const QString &argument : application.arguments().mid(2)) {
        if (argument != QStringLiteral("--serve") && !argument.startsWith(QStringLiteral("--serve=")))
            continue;
        bool validPort = true;
        const QString portText = argument.section(QLatin1Char('='), 1, 1);
        const uint port = portText.isEmpty() ? 0 : portText.toUInt(&validPort);
        if (!validPort || port > 65535 || controller.filePath().isEmpty()) {
            QTextStream(stderr) << "mustermark: --serve requires an open file and a port from 0 to 65535"
                                << Qt::endl;
            return 64;
        }
        documentServer = std::make_unique<Mustermark::DocumentHttpServer>(&controller, stylePath);
        if (!documentServer->listen(static_cast<quint16>(port))) {
            QTextStream(stderr) << "mustermark: " << documentServer->errorString() << Qt::endl;
            return 1;
        }
        const QJsonObject ready{
            {QStringLiteral("ok"), true},
            {QStringLiteral("url"), QStringLiteral("http://127.0.0.1:%1/").arg(documentServer->port())},
            {QStringLiteral("token"), documentServer->token()},
            {QStringLiteral("path"), controller.filePath()},
            {QStringLiteral("style"), stylePath},
        };
        QTextStream(stdout) << QJsonDocument(ready).toJson(QJsonDocument::Compact) << Qt::endl;
        break;
    }

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("documentController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("markdownHighlighter"), &highlighter);
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    if (engine.rootObjects().isEmpty())
        return 1;
    return application.exec();
}
