#include "cli.h"

#include "documentengine.h"
#include "filedocument.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
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
    const FileResult file = FileDocument::read(path);
    if (!file.ok)
        return errorObject(QStringLiteral("read_failed"), file.error);
    QJsonObject result = engine.parse(file.source).toJson();
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("path"), QFileInfo(path).absoluteFilePath());
    return result;
}

QJsonObject mutatePath(const QString &path, const QString &operation,
                       const QJsonObject &params, DocumentEngine &engine) {
    const FileResult file = FileDocument::read(path);
    if (!file.ok)
        return errorObject(QStringLiteral("read_failed"), file.error);
    const QString before = DocumentEngine::revisionFor(file.source);
    EditResult edit;
    if (operation == QStringLiteral("track"))
        edit = engine.track(file.source);
    else if (operation == QStringLiteral("repair"))
        edit = engine.repair(file.source);
    else if (operation == QStringLiteral("untrack"))
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

QJsonObject handleRequest(const QJsonObject &request, DocumentEngine &engine) {
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
    } else if (method == QStringLiteral("document.inspect") ||
               method == QStringLiteral("document.validate")) {
        payload = inspectPath(path, engine);
    } else if (method == QStringLiteral("document.track")) {
        payload = mutatePath(path, QStringLiteral("track"), params, engine);
    } else if (method == QStringLiteral("document.repair")) {
        payload = mutatePath(path, QStringLiteral("repair"), params, engine);
    } else if (method == QStringLiteral("document.untrack")) {
        payload = mutatePath(path, QStringLiteral("untrack"), params, engine);
    } else if (method == QStringLiteral("document.apply")) {
        payload = mutatePath(path, params.value(QStringLiteral("action")).toString(), params, engine);
    } else {
        return {{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                {QStringLiteral("id"), id},
                {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), -32601},
                                                       {QStringLiteral("message"), QStringLiteral("Method not found")}}}};
    }
    return {{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), id},
            {QStringLiteral("result"), payload}};
}

int runStdio(DocumentEngine &engine) {
    QTextStream input(stdin);
    QTextStream output(stdout);
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
            response = handleRequest(parsed.object(), engine);
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
        QStringLiteral("inspect"), QStringLiteral("validate"), QStringLiteral("track"),
        QStringLiteral("repair"), QStringLiteral("untrack"), QStringLiteral("apply"),
        QStringLiteral("api"), QStringLiteral("--help"), QStringLiteral("--version")};
    return commands.contains(arguments.at(1));
}

int Cli::run(const QStringList &arguments) {
    DocumentEngine engine;
    const QString command = arguments.value(1);
    if (command == QStringLiteral("--version")) {
        QTextStream(stdout) << "mustermark 0.1.0" << Qt::endl;
        return 0;
    }
    if (command == QStringLiteral("--help") || arguments.size() < 3) {
        QTextStream(stdout)
            << "Usage:\n"
            << "  mustermark FILE.md\n"
            << "  mustermark inspect|validate|track|repair|untrack FILE.md\n"
            << "  mustermark apply FILE.md ACTION NODE [--scope=self|subtree] [--target=ID] [--label=LABEL] [--text=TEXT] [--level=N]\n"
            << "  mustermark api --stdio\n";
        return command == QStringLiteral("--help") ? 0 : 64;
    }
    if (command == QStringLiteral("api"))
        return arguments.contains(QStringLiteral("--stdio")) ? runStdio(engine) : 64;

    const QString path = arguments.value(2);
    QJsonObject result;
    if (command == QStringLiteral("inspect") || command == QStringLiteral("validate")) {
        result = inspectPath(path, engine);
    } else if (command == QStringLiteral("track") || command == QStringLiteral("repair") ||
               command == QStringLiteral("untrack")) {
        result = mutatePath(path, command, {}, engine);
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
