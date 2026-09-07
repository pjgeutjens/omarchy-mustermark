#include "cli.h"
#include "documentcontroller.h"
#include "httpserver.h"
#include "markdownhighlighter.h"
#include "previewipc.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrl>

#include <memory>

int main(int argc, char *argv[]) {
    const QStringList earlyArguments = [&] {
        QStringList values;
        for (int index = 0; index < argc; ++index)
            values.append(QString::fromLocal8Bit(argv[index]));
        return values;
    }();

    const bool internalPreviewMode = earlyArguments.value(1) == QStringLiteral("--preview");
    const bool appendMode = earlyArguments.value(1) == QStringLiteral("append");
    const int visualFlag = earlyArguments.indexOf(QStringLiteral("--visual"));
    const bool visualMode = internalPreviewMode || appendMode || visualFlag > 0;
    if (!visualMode && Cli::isCommand(earlyArguments)) {
        QCoreApplication application(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("mustermark"));
        QCoreApplication::setApplicationVersion(QStringLiteral("0.2.0"));
        return Cli::run(application.arguments());
    }

    QGuiApplication application(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("Mustermark"));
    QGuiApplication::setOrganizationName(QStringLiteral("Mustermark"));
    QGuiApplication::setDesktopFileName(QStringLiteral("mustermark"));

    const QString requestedArgument = internalPreviewMode || appendMode || visualFlag == 1
        ? application.arguments().value(2) : application.arguments().value(1);
    if (visualMode && (requestedArgument.isEmpty() || requestedArgument.startsWith("--"))) {
        QTextStream(stderr) << "mustermark: Visual Mode requires a Markdown file" << Qt::endl;
        return 64;
    }
    const QString requestedPath = requestedArgument.isEmpty() ? QString() : QFileInfo(requestedArgument).absoluteFilePath();
    const auto option = [&](const QString &name) {
        const QString prefix = "--" + name + '=';
        for (const QString &argument : application.arguments())
            if (argument.startsWith(prefix)) return argument.mid(prefix.size());
        return QString();
    };
    const QString appendSection = appendMode ? option("section") : QString();
    const QString appendKind = appendMode ? option("kind") : QString();
    if (appendMode && !appendKind.isEmpty() && appendKind != "task" && appendKind != "bullet") return 64;
    const QJsonObject request{{"version", 1}, {"type", appendMode ? "append" : "open"},
        {"path", requestedPath}, {"visual", visualMode}, {"section", appendSection}, {"kind", appendKind}};
    if (!requestedPath.isEmpty() && Mustermark::sendPreviewRequest(request, 250)) {
        QTextStream(stdout) << QJsonDocument(QJsonObject{{"ok", true}, {"activated", true}, {"path", requestedPath}}).toJson(QJsonDocument::Compact) << Qt::endl;
        return 0;
    }
    DocumentController controller;
    MarkdownHighlighter highlighter;
    if (!requestedPath.isEmpty() && !controller.loadFile(QUrl::fromLocalFile(requestedPath))) return 1;
    QString stylePath = option("html-style");
    if (stylePath.isEmpty()) {
        stylePath = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/mustermark/preview.css";
        if (!QFileInfo::exists(stylePath)) stylePath.clear();
    }
    bool serve = false;
    uint port = 0;
    for (const QString &arg : application.arguments()) {
        if (arg != "--serve" && !arg.startsWith("--serve=")) continue;
        serve = true;
        bool valid = true;
        const QString value = arg.section('=', 1, 1);
        port = value.isEmpty() ? 0 : value.toUInt(&valid);
        if (!valid || port > 65535 || controller.filePath().isEmpty()) return 64;
    }
    Mustermark::DocumentHttpServer documentServer(&controller, stylePath);
    if (!documentServer.listen(static_cast<quint16>(port))) {
        QTextStream(stderr) << documentServer.errorString() << Qt::endl;
        return 1;
    }
    const QUrl previewUrl(QString("http://127.0.0.1:%1/#preview").arg(documentServer.port()));
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("documentController", &controller);
    engine.rootContext()->setContextProperty("markdownHighlighter", &highlighter);
    engine.rootContext()->setContextProperty("previewUrl", previewUrl);
    engine.rootContext()->setContextProperty("initialVisualMode", visualMode);
    engine.load(QUrl("qrc:/qml/Main.qml"));
    if (engine.rootObjects().isEmpty()) return 1;
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    if (!window) return 1;
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    const auto bindServer = [&] {
        server.close();
        if (controller.filePath().isEmpty()) return;
        const QString name = Mustermark::previewServerName(controller.filePath());
        if (!server.listen(name)) {
            QLocalSocket probe;
            probe.connectToServer(name);
            if (!probe.waitForConnected(100)) {
                QLocalServer::removeServer(name);
                server.listen(name);
            }
        }
    };
    bindServer();
    QObject::connect(&controller, &DocumentController::filePathChanged, &server, bindServer);
    QObject::connect(&server, &QLocalServer::newConnection, &application, [&] {
        while (server.hasPendingConnections()) {
            auto *socket = server.nextPendingConnection();
            const auto read = [&, socket] {
                auto bytes = socket->property("buffer").toByteArray() + socket->readAll();
                if (bytes.size() > 16384) { socket->abort(); return; }
                if (!bytes.contains('\n')) { socket->setProperty("buffer", bytes); return; }
                const auto value = QJsonDocument::fromJson(bytes.left(bytes.indexOf('\n'))).object();
                const QString path = value.value("path").toString();
                if (path.isEmpty()) return;
                QMetaObject::invokeMethod(window, "acceptOpenRequest", Q_ARG(QVariant, path),
                    Q_ARG(QVariant, value.value("visual").toBool(true)),
                    Q_ARG(QVariant, value.value("type").toString() == "append"),
                    Q_ARG(QVariant, value.value("section").toString()), Q_ARG(QVariant, value.value("kind").toString()));
                window->show(); window->raise(); window->requestActivate();
                QTextStream(stdout) << QJsonDocument(QJsonObject{{"ok", true}, {"activated", true}, {"path", path}}).toJson(QJsonDocument::Compact) << Qt::endl;
                socket->disconnectFromServer();
            };
            QObject::connect(socket, &QLocalSocket::readyRead, socket, read);
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            read();
        }
    });
    if (appendMode) QMetaObject::invokeMethod(window, "requestAppend", Q_ARG(QVariant, appendSection), Q_ARG(QVariant, appendKind));
    if (serve || visualMode) {
        QTextStream(stdout) << QJsonDocument(QJsonObject{{"ok", true}, {"preview", visualMode},
            {"url", serve ? previewUrl.toString(QUrl::RemoveFragment) : previewUrl.toString()}, {"token", documentServer.token()},
            {"path", controller.filePath()}, {"style", stylePath}}).toJson(QJsonDocument::Compact) << Qt::endl;
    }
    return application.exec();
}
