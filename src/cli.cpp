#include "cli.h"

#include "documentengine.h"
#include "documentsession.h"
#include "filedocument.h"
#include "httpserver.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSharedPointer>
#include <QTextStream>

using namespace Mustermark;

namespace {

void printJson(const QJsonObject &object) {
    QTextStream(stdout) << QJsonDocument(object).toJson(QJsonDocument::Compact) << Qt::endl;
}

QJsonObject errorObject(const QString &code, const QString &message) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), code},
                                                   {QStringLiteral("message"), message}}}};
}

QJsonObject inspectPath(const QString &path, DocumentEngine &engine) {
    Q_UNUSED(engine)
    const FileResult file = FileDocument::read(path);
    if (!file.ok)
        return errorObject(QStringLiteral("read_failed"), file.error);
    DocumentSession session;
    session.setSource(file.source);
    QJsonObject result = session.document().toJson();
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("path"), QFileInfo(path).absoluteFilePath());
    return result;
}

QJsonObject snapshotPath(const QString &path, const QString &node, const QString &scope) {
    const FileResult file = FileDocument::read(path);
    if (!file.ok)
        return errorObject(QStringLiteral("read_failed"), file.error);
    DocumentSession session;
    session.setSource(file.source);
    return session.snapshot(QFileInfo(path).absoluteFilePath(), node, scope);
}

QJsonObject mutatePath(const QString &path, const QString &operation,
                       const QJsonObject &params, DocumentEngine &engine) {
    const FileResult file = FileDocument::read(path);
    if (!file.ok)
        return errorObject(QStringLiteral("read_failed"), file.error);
    const QString before = DocumentEngine::revisionFor(file.source);
    EditResult edit;
    if (operation == QStringLiteral("strip_metadata"))
        edit = engine.untrack(file.source);
    else
        edit = engine.apply(file.source, params.value(QStringLiteral("baseRevision")).toString(before),
                            operation, params.value(QStringLiteral("node")).toString(), params);
    if (!edit.ok)
        return errorObject(edit.errorCode, edit.errorMessage);
    const FileResult written = FileDocument::writeAtomic(path, edit.source, before);
    if (!written.ok)
        return errorObject(written.error == QStringLiteral("stale_revision")
                               ? QStringLiteral("stale_revision") : QStringLiteral("write_failed"),
                           written.error);
    QJsonObject result = engine.parse(edit.source).toJson();
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("path"), QFileInfo(path).absoluteFilePath());
    result.insert(QStringLiteral("edits"), edit.edits);
    return result;
}

QJsonObject sessionPayload(const QString &path, const DocumentSession &session,
                           const QJsonArray &edits = {}) {
    QJsonObject result = session.document().toJson();
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("path"), QFileInfo(path).absoluteFilePath());
    result.insert(QStringLiteral("source"), QString::fromUtf8(session.document().source));
    result.insert(QStringLiteral("html"), session.document().renderedHtml);
    result.insert(QStringLiteral("edits"), edits);
    result.insert(QStringLiteral("externalIdBindingVersion"), 1);
    result.insert(QStringLiteral("identityScope"), QStringLiteral("session"));
    result.insert(QStringLiteral("externalIdScope"), session.document().documentId.isEmpty() ? "session" : "document");
    return result;
}

QJsonObject handleRequest(
    const QJsonObject &request,
    QHash<QString, QSharedPointer<DocumentSession>> &sessions) {
    const QJsonValue id = request.value(QStringLiteral("id"));
    const QString version = request.value(QStringLiteral("jsonrpc")).toString();
    if (version != QStringLiteral("2.0")) {
        return {{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                {QStringLiteral("id"), id},
                {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), -32600},
                                                       {QStringLiteral("message"), QStringLiteral("Invalid Request")}}}};
    }
    const QString method = request.value(QStringLiteral("method")).toString();
    const QJsonObject params = request.value(QStringLiteral("params")).toObject();
    const QString path = params.value(QStringLiteral("path")).toString();
    QJsonObject payload;
    if (path.isEmpty()) {
        payload = errorObject(QStringLiteral("path_required"), QStringLiteral("params.path is required"));
    } else {
        const QString absolutePath = QFileInfo(path).absoluteFilePath();
        const bool linkedRequest = method.startsWith(QStringLiteral("document.link.")) ||
            QFileInfo::exists(QFileInfo(absolutePath).canonicalFilePath() + ".mustermark.json");
        if (linkedRequest && !QFileInfo(absolutePath).isFile())
            return {{"jsonrpc", "2.0"}, {"id", id},
                    {"result", errorObject("invalid_linked_source", "A regular source file is required.")}};
        const FileResult file = FileDocument::read(absolutePath, linkedRequest ? 2 * 1024 * 1024 : -1);
        if (!file.ok) {
            payload = errorObject(QStringLiteral("read_failed"), file.error);
        } else {
            auto session = sessions.value(absolutePath);
            if (!session) {
                session = QSharedPointer<DocumentSession>::create();
                sessions.insert(absolutePath, session);
            }
            session->setSource(file.source);

            const bool initialize = method == QStringLiteral("document.link.initialize");
            const bool linked = initialize || method == QStringLiteral("document.link.inspect") ||
                QFileInfo::exists(QFileInfo(absolutePath).canonicalFilePath() + QStringLiteral(".mustermark.json"));
            if (linked) {
                const auto loaded = FileDocument::readLinked(absolutePath, *session, initialize);
                if (!loaded.ok)
                    return {{"jsonrpc", "2.0"}, {"id", id},
                            {"result", errorObject(loaded.error, loaded.error)}};
                if (session->document().source != file.source)
                    return {{"jsonrpc", "2.0"}, {"id", id},
                            {"result", errorObject("stale_revision", "Source changed during inspection.")}};
            }

            EditResult edit{true, file.source, {}, {}, {}};
            if (method == QStringLiteral("document.inspect") ||
                method == QStringLiteral("document.link.initialize") ||
                method == QStringLiteral("document.link.inspect") ||
                method == QStringLiteral("document.validate")) {
                payload = sessionPayload(absolutePath, *session);
            } else if (method == QStringLiteral("document.track") ||
                       method == QStringLiteral("document.tracking_start")) {
                edit = session->startTracking();
            } else if (method == QStringLiteral("document.untrack") ||
                       method == QStringLiteral("document.tracking_stop")) {
                edit = session->stopTracking();
            } else if (method == QStringLiteral("document.apply")) {
                const QString revision = params.value(QStringLiteral("baseRevision")).toString();
                if (revision.isEmpty()) {
                    edit = {false, file.source, QStringLiteral("base_revision_required"),
                            QStringLiteral("baseRevision is required."), {}};
                } else {
                    edit = session->apply(revision,
                                          params.value(QStringLiteral("action")).toString(),
                                          params.value(QStringLiteral("node")).toString(), params);
                }
            } else if (method == QStringLiteral("document.snapshot")) {
                if (params.contains("baseRevision") && params.value("baseRevision").toString() != session->document().revision)
                    payload = errorObject("stale_revision", "The source changed before its snapshot.");
                else payload = session->snapshot(absolutePath,
                                            params.value(QStringLiteral("node")).toString(),
                                            params.value(QStringLiteral("scope")).toString(
                                                QStringLiteral("item")), params.value("includeAttachmentData").toBool());
            } else if (method == QStringLiteral("document.external_id.bind") ||
                       method == QStringLiteral("document.external_id.resolve")) {
                const QString node = params.value(QStringLiteral("node")).toString();
                if (method.endsWith(QStringLiteral(".bind")) && node.isEmpty())
                    payload = errorObject(QStringLiteral("node_required"),
                                          QStringLiteral("Binding requires a session node ID."));
                else
                    payload = session->externalBinding(
                        params.value(QStringLiteral("baseRevision")).toString(),
                        params.value(QStringLiteral("namespace")).toString(),
                        params.value(QStringLiteral("value")).toString(),
                        method.endsWith(QStringLiteral(".bind")) ? node : QString());
                if (method.endsWith(QStringLiteral(".bind")) && linked && payload.value("ok").toBool()) {
                    const auto saved = FileDocument::writeAtomic(absolutePath, session->document().source,
                                                                 session->document().revision, session.data());
                    if (!saved.ok) payload = errorObject(saved.error, saved.error);
                }
            } else {
                return {{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                        {QStringLiteral("id"), id},
                        {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), -32601},
                                                               {QStringLiteral("message"), QStringLiteral("Method not found")}}}};
            }

            if (payload.isEmpty()) {
                if (!edit.ok) {
                    payload = errorObject(edit.errorCode, edit.errorMessage);
                } else if (session->document().source != file.source) {
                    const FileResult written = FileDocument::writeAtomic(
                        absolutePath, session->document().source,
                        DocumentEngine::revisionFor(file.source), session.data());
                    if (!written.ok)
                        payload = errorObject(
                            written.error == QStringLiteral("stale_revision")
                                ? QStringLiteral("stale_revision")
                                : QStringLiteral("write_failed"),
                            written.error);
                }
                if (payload.isEmpty())
                    payload = sessionPayload(absolutePath, *session, edit.edits);
            }
        }
    }
    return {{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), id},
            {QStringLiteral("result"), payload}};
}

int runStdio() {
    QTextStream input(stdin);
    QTextStream output(stdout);
    QHash<QString, QSharedPointer<DocumentSession>> sessions;
    while (!input.atEnd()) {
        const QString line = input.readLine();
        if (line.trimmed().isEmpty())
            continue;
        QJsonParseError parseError;
        const QJsonDocument parsed = QJsonDocument::fromJson(line.toUtf8(), &parseError);
        QJsonObject response;
        if (parseError.error != QJsonParseError::NoError || !parsed.isObject()) {
            response = {{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                        {QStringLiteral("id"), QJsonValue::Null},
                        {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), -32700},
                                                               {QStringLiteral("message"), QStringLiteral("Parse error")}}}};
        } else {
            response = handleRequest(parsed.object(), sessions);
        }
        output << QJsonDocument(response).toJson(QJsonDocument::Compact) << Qt::endl;
    }
    return 0;
}

QString argumentValue(const QStringList &arguments, const QString &name) {
    const QString prefix = QStringLiteral("--") + name + QLatin1Char('=');
    for (const QString &argument : arguments) {
        if (argument.startsWith(prefix))
            return argument.mid(prefix.size());
    }
    return {};
}

} // namespace

bool Cli::isCommand(const QStringList &arguments) {
    if (arguments.size() < 2)
        return false;
    static const QStringList commands{
        QStringLiteral("inspect"), QStringLiteral("validate"), QStringLiteral("strip-metadata"),
        QStringLiteral("apply"), QStringLiteral("snapshot"),
        QStringLiteral("api"), QStringLiteral("serve"), QStringLiteral("--help"),
        QStringLiteral("--version")};
    return commands.contains(arguments.at(1));
}

int Cli::run(const QStringList &arguments) {
    DocumentEngine engine;
    const QString command = arguments.value(1);
    if (command == QStringLiteral("--version")) {
        QTextStream(stdout) << "mustermark 0.2.0" << Qt::endl;
        return 0;
    }
    if (command == QStringLiteral("--help") || arguments.size() < 3) {
        QTextStream(stdout)
            << "Usage:\n"
            << "  mustermark FILE.md [--visual] [--serve[=PORT]] [--html-style=FILE.css]\n"
            << "  mustermark --visual FILE.md\n"
            << "  mustermark append FILE.md [--section=NAME] [--kind=task|bullet]\n"
            << "  mustermark inspect|validate|strip-metadata FILE.md\n"
            << "  mustermark apply FILE.md ACTION NODE [--scope=self|subtree] [--target=REF] [--checked=true|false] [--text=TEXT] [--level=N]\n"
            << "  mustermark snapshot FILE.md NODE [--scope=item|section]\n"
            << "  mustermark serve FILE.md [--port=N] [--html-style=FILE.css]\n"
            << "  mustermark api --stdio\n";
        return command == QStringLiteral("--help") ? 0 : 64;
    }
    if (command == QStringLiteral("api"))
        return arguments.contains(QStringLiteral("--stdio")) ? runStdio() : 64;
    if (command == QStringLiteral("serve")) {
        bool validPort = true;
        const QString portValue = argumentValue(arguments, QStringLiteral("port"));
        const uint parsedPort = portValue.isEmpty() ? 0 : portValue.toUInt(&validPort);
        if (!validPort || parsedPort > 65535) {
            QTextStream(stderr) << "mustermark: port must be between 0 and 65535" << Qt::endl;
            return 64;
        }
        return serveDocument(arguments.at(2), static_cast<quint16>(parsedPort),
                             argumentValue(arguments, QStringLiteral("html-style")));
    }

    if (command == QStringLiteral("snapshot")) {
        if (arguments.size() < 4) {
            printJson(errorObject(QStringLiteral("usage"), QStringLiteral("NODE is required")));
            return 64;
        }
        const QJsonObject result = snapshotPath(
            arguments.at(2), arguments.at(3),
            argumentValue(arguments, QStringLiteral("scope")).isEmpty()
                ? QStringLiteral("item")
                : argumentValue(arguments, QStringLiteral("scope")));
        printJson(result);
        return result.value(QStringLiteral("ok")).toBool() ? 0 : 1;
    }

    const QString path = arguments.value(2);
    QJsonObject result;
    if (command == QStringLiteral("inspect") || command == QStringLiteral("validate")) {
        result = inspectPath(path, engine);
    } else if (command == QStringLiteral("strip-metadata")) {
        result = mutatePath(path, QStringLiteral("strip_metadata"), {}, engine);
    } else if (command == QStringLiteral("apply")) {
        if (arguments.size() < 5) {
            printJson(errorObject(QStringLiteral("usage"), QStringLiteral("ACTION and NODE are required")));
            return 64;
        }
        QJsonObject params{{QStringLiteral("node"), arguments.at(4)}};
        for (const QString &name : {QStringLiteral("target"), QStringLiteral("label"),
                                    QStringLiteral("text"), QStringLiteral("baseRevision"),
                                    QStringLiteral("scope")}) {
            const QString value = argumentValue(arguments, name);
            if (!value.isEmpty()) params.insert(name, value);
        }
        const QString level = argumentValue(arguments, QStringLiteral("level"));
        if (!level.isEmpty()) params.insert(QStringLiteral("level"), level.toInt());
        const QString checked = argumentValue(arguments, QStringLiteral("checked"));
        if (!checked.isEmpty()) params.insert(QStringLiteral("checked"), checked == QStringLiteral("true"));
        result = mutatePath(path, arguments.at(3), params, engine);
    } else {
        result = errorObject(QStringLiteral("unknown_command"), command);
    }
    printJson(result);
    if (!result.value(QStringLiteral("ok")).toBool())
        return 1;
    if (command == QStringLiteral("validate") &&
        !result.value(QStringLiteral("diagnostics")).toArray().isEmpty())
        return 2;
    return 0;
}
