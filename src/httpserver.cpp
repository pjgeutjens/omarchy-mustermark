#include "httpserver.h"

#include "documentcontroller.h"
#include "documentengine.h"
#include "documentsession.h"
#include "filedocument.h"

#include <QCoreApplication>
#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTextStream>
#include <QTcpSocket>
#include <QUuid>
#include <QUrl>
#include <QUrlQuery>

#include <utility>

namespace Mustermark {
namespace {

constexpr qsizetype maxRequestBytes = 9 * 1024 * 1024;
constexpr qsizetype maxImageBytes = 8 * 1024 * 1024;

QJsonObject errorObject(const QString &code, const QString &message) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), code},
                                                   {QStringLiteral("message"), message}}}};
}

QByteArray json(const QJsonObject &value) {
    return QJsonDocument(value).toJson(QJsonDocument::Compact);
}

QByteArray statusText(int status) {
    switch (status) {
    case 200: return QByteArrayLiteral("OK");
    case 400: return QByteArrayLiteral("Bad Request");
    case 401: return QByteArrayLiteral("Unauthorized");
    case 404: return QByteArrayLiteral("Not Found");
    case 405: return QByteArrayLiteral("Method Not Allowed");
    case 409: return QByteArrayLiteral("Conflict");
    case 413: return QByteArrayLiteral("Content Too Large");
    default: return QByteArrayLiteral("Internal Server Error");
    }
}

} // namespace

DocumentHttpServer::~DocumentHttpServer() {
    // Socket destruction emits disconnected; detach callbacks before member teardown.
    for (auto *socket : m_server.findChildren<QTcpSocket *>())
        QObject::disconnect(socket, nullptr, this, nullptr);
    m_server.close();
}

DocumentHttpServer::DocumentHttpServer(QString path, QString stylePath, QObject *parent)
    : QObject(parent), m_path(QFileInfo(path).absoluteFilePath()),
      m_stylePath(std::move(stylePath)),
      m_token(QUuid::createUuid().toString(QUuid::WithoutBraces)),
      m_ownedSession(std::make_unique<DocumentSession>()), m_session(m_ownedSession.get()) {
    connect(&m_server, &QTcpServer::newConnection, this,
            [this] { acceptConnection(); });
}

DocumentHttpServer::DocumentHttpServer(DocumentController *controller, QString stylePath,
                                       QObject *parent)
    : QObject(parent), m_path(controller ? controller->filePath() : QString()),
      m_stylePath(std::move(stylePath)),
      m_token(QUuid::createUuid().toString(QUuid::WithoutBraces)),
      m_controller(controller) {
    connect(&m_server, &QTcpServer::newConnection, this,
            [this] { acceptConnection(); });
    if (m_controller)
        connect(m_controller, &DocumentController::documentChanged,
                this, &DocumentHttpServer::broadcastChange);
    if (m_controller)
        connect(m_controller, &DocumentController::themeChanged,
                this, &DocumentHttpServer::broadcastChange);
}

bool DocumentHttpServer::listen(quint16 port) {
    if (!m_stylePath.isEmpty()) {
        QFile styleFile(m_stylePath);
        if (!styleFile.open(QIODevice::ReadOnly)) {
            m_errorString = QStringLiteral("cannot read style file: %1").arg(m_stylePath);
            return false;
        }
        if (styleFile.size() > 256 * 1024) {
            m_errorString = QStringLiteral("style files are limited to 256 KiB");
            return false;
        }
        m_style = styleFile.readAll();
    }
    return m_server.listen(QHostAddress::LocalHost, port);
}

quint16 DocumentHttpServer::port() const {
    return m_server.serverPort();
}

void DocumentHttpServer::acceptConnection() {
    while (QTcpSocket *socket = m_server.nextPendingConnection()) {
        socket->setProperty("requestBuffer", QByteArray());
        connect(socket, &QTcpSocket::readyRead, this,
                [this, socket] { readRequest(socket); });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }
}

void DocumentHttpServer::readRequest(QTcpSocket *socket) {
    if (socket->property("requestHandled").toBool())
        return;
    QByteArray buffer = socket->property("requestBuffer").toByteArray();
    buffer += socket->readAll();
    if (buffer.size() > maxRequestBytes) {
        socket->setProperty("requestHandled", true);
        sendResponse(socket, 413, QByteArrayLiteral("application/json"),
                     json(errorObject(QStringLiteral("request_too_large"),
                                      QStringLiteral("Requests are limited to 9 MiB."))));
        return;
    }
    socket->setProperty("requestBuffer", buffer);
    const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
    if (headerEnd < 0)
        return;

    const QList<QByteArray> lines = buffer.left(headerEnd).split('\n');
    if (lines.isEmpty())
        return;
    const QList<QByteArray> requestLine = lines.first().trimmed().split(' ');
    if (requestLine.size() != 3) {
        socket->setProperty("requestHandled", true);
        sendResponse(socket, 400, QByteArrayLiteral("application/json"),
                     json(errorObject(QStringLiteral("invalid_request"),
                                      QStringLiteral("The HTTP request line is invalid."))));
        return;
    }

    Request request;
    request.method = requestLine.at(0);
    request.target = requestLine.at(1);
    qsizetype contentLength = 0;
    for (int index = 1; index < lines.size(); ++index) {
        const QByteArray line = lines.at(index).trimmed();
        const qsizetype separator = line.indexOf(':');
        if (separator < 0)
            continue;
        const QByteArray name = line.left(separator).trimmed().toLower();
        const QByteArray value = line.mid(separator + 1).trimmed();
        if (name == QByteArrayLiteral("content-length"))
            contentLength = value.toLongLong();
        else if (name == QByteArrayLiteral("content-type"))
            request.contentType = value;
        else if (name == QByteArrayLiteral("x-mustermark-token"))
            request.token = value;
    }
    const qsizetype bodyStart = headerEnd + 4;
    if (contentLength < 0 || contentLength > maxRequestBytes) {
        socket->setProperty("requestHandled", true);
        sendResponse(socket, 413, QByteArrayLiteral("application/json"),
                     json(errorObject(QStringLiteral("request_too_large"),
                                      QStringLiteral("Requests are limited to 9 MiB."))));
        return;
    }
    if (buffer.size() - bodyStart < contentLength)
        return;
    request.body = buffer.mid(bodyStart, contentLength);
    socket->setProperty("requestHandled", true);
    handleRequest(socket, request);
}

void DocumentHttpServer::handleRequest(QTcpSocket *socket, const Request &request) {
    const QUrl requestUrl = QUrl::fromEncoded(request.target);
    const QString path = requestUrl.path();
    if (request.method == QByteArrayLiteral("GET") && path == QStringLiteral("/")) {
        sendResponse(socket, 200, QByteArrayLiteral("text/html; charset=utf-8"), page());
        return;
    }
    if (request.method == QByteArrayLiteral("GET") && path == QStringLiteral("/api/state")) {
        sendResponse(socket, 200, QByteArrayLiteral("application/json; charset=utf-8"), json(state()));
        return;
    }
    if (request.method == QByteArrayLiteral("GET") && path == QStringLiteral("/api/events")) {
        sendEventStream(socket);
        return;
    }
    if (request.method == QByteArrayLiteral("GET") && path == QStringLiteral("/api/view")) {
        const QJsonObject value = state();
        if (!value.value(QStringLiteral("ok")).toBool()) {
            sendResponse(socket, 500, QByteArrayLiteral("application/json; charset=utf-8"), json(value));
            return;
        }
        sendResponse(socket, 200, QByteArrayLiteral("text/html; charset=utf-8"),
                     value.value(QStringLiteral("html")).toString().toUtf8());
        return;
    }
    if (request.method == QByteArrayLiteral("GET") && path == QStringLiteral("/style.css")) {
        sendResponse(socket, 200, QByteArrayLiteral("text/css; charset=utf-8"), styleSheet());
        return;
    }
    if (request.method == QByteArrayLiteral("GET") && sendDocumentAsset(socket, path))
        return;
    if (path == QStringLiteral("/api/attachments")) {
        if (request.method != QByteArrayLiteral("POST")) {
            sendResponse(socket, 405, QByteArrayLiteral("application/json; charset=utf-8"),
                         json(errorObject(QStringLiteral("method_not_allowed"),
                                          QStringLiteral("Use POST for image attachments."))));
            return;
        }
        if (request.token != m_token.toUtf8()) {
            sendResponse(socket, 401, QByteArrayLiteral("application/json; charset=utf-8"),
                         json(errorObject(QStringLiteral("unauthorized"),
                                          QStringLiteral("The Mustermark token is missing or invalid."))));
            return;
        }
        const QUrlQuery query(requestUrl);
        const QString node = query.queryItemValue(QStringLiteral("node"), QUrl::FullyDecoded);
        const QString baseRevision = query.queryItemValue(QStringLiteral("baseRevision"),
                                                          QUrl::FullyDecoded);
        const QString fileName = query.queryItemValue(QStringLiteral("filename"),
                                                      QUrl::FullyDecoded);
        QJsonObject instruction{
            {QStringLiteral("action"), QStringLiteral("attachment_upload")},
            {QStringLiteral("node"), node},
            {QStringLiteral("filename"), fileName},
            {QStringLiteral("origin"), QStringLiteral("ui")},
        };
        QJsonObject result;
        if (!m_controller) {
            result = errorObject(QStringLiteral("visual_mode_required"),
                                 QStringLiteral("Image uploads require a running Visual Mode session."));
        } else if (request.body.isEmpty() || request.body.size() > maxImageBytes) {
            result = errorObject(QStringLiteral("invalid_image"),
                                 QStringLiteral("Images must be between 1 byte and 8 MiB."));
        } else {
            result = m_controller->attachApiImage(node, request.body,
                                                   QString::fromLatin1(request.contentType),
                                                   fileName, baseRevision);
        }
        recordInstruction(instruction, result);
        broadcastChange();
        const QString code = result.value(QStringLiteral("error")).toObject()
                                 .value(QStringLiteral("code")).toString();
        const int status = result.value(QStringLiteral("ok")).toBool()
                               ? 200 : code == QStringLiteral("stale_revision") ? 409 : 400;
        sendResponse(socket, status, QByteArrayLiteral("application/json; charset=utf-8"),
                     json(result));
        return;
    }
    if (path != QStringLiteral("/api/actions")) {
        sendResponse(socket, 404, QByteArrayLiteral("application/json; charset=utf-8"),
                     json(errorObject(QStringLiteral("not_found"), QStringLiteral("Endpoint not found."))));
        return;
    }
    if (request.method != QByteArrayLiteral("POST")) {
        sendResponse(socket, 405, QByteArrayLiteral("application/json; charset=utf-8"),
                     json(errorObject(QStringLiteral("method_not_allowed"),
                                      QStringLiteral("Use POST for actions."))));
        return;
    }
    if (request.token != m_token.toUtf8()) {
        sendResponse(socket, 401, QByteArrayLiteral("application/json; charset=utf-8"),
                     json(errorObject(QStringLiteral("unauthorized"),
                                      QStringLiteral("The Mustermark token is missing or invalid."))));
        return;
    }
    if (!request.contentType.startsWith(QByteArrayLiteral("application/json"))) {
        sendResponse(socket, 400, QByteArrayLiteral("application/json; charset=utf-8"),
                     json(errorObject(QStringLiteral("content_type"),
                                      QStringLiteral("Actions require application/json."))));
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument parsed = QJsonDocument::fromJson(request.body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !parsed.isObject()) {
        sendResponse(socket, 400, QByteArrayLiteral("application/json; charset=utf-8"),
                     json(errorObject(QStringLiteral("invalid_json"), parseError.errorString())));
        return;
    }
    const QJsonObject instruction = parsed.object();
    const QJsonObject result = applyInstruction(instruction);
    recordInstruction(instruction, result);
    broadcastChange();
    const QString code = result.value(QStringLiteral("error")).toObject()
                             .value(QStringLiteral("code")).toString();
    const int status = result.value(QStringLiteral("ok")).toBool()
                           ? 200 : code == QStringLiteral("stale_revision") ? 409 : 400;
    sendResponse(socket, status, QByteArrayLiteral("application/json; charset=utf-8"), json(result));
}

void DocumentHttpServer::sendEventStream(QTcpSocket *socket) {
    QByteArray response = QByteArrayLiteral(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream; charset=utf-8\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: keep-alive\r\n"
        "X-Content-Type-Options: nosniff\r\n\r\n"
        "retry: 1000\n\n");
    socket->write(response);
    m_eventClients.insert(socket);
    connect(socket, &QTcpSocket::disconnected, this,
            [this, socket] { m_eventClients.remove(socket); });
}

void DocumentHttpServer::broadcastChange() {
    const QByteArray event = QByteArrayLiteral("event: change\ndata: updated\n\n");
    for (QTcpSocket *socket : std::as_const(m_eventClients)) {
        if (socket && socket->state() == QAbstractSocket::ConnectedState)
            socket->write(event);
    }
}

bool DocumentHttpServer::sendDocumentAsset(QTcpSocket *socket, const QString &requestPath) {
    const QString documentPath = m_controller ? m_controller->filePath() : m_path;
    if (documentPath.isEmpty() || !requestPath.startsWith(QLatin1Char('/')))
        return false;
    const QString relative = QDir::cleanPath(requestPath.mid(1));
    if (relative.isEmpty() || relative == QStringLiteral(".") || relative == QStringLiteral("..") ||
        relative.startsWith(QStringLiteral("../")) || !QDir::isRelativePath(relative))
        return false;

    const QFileInfo document(documentPath);
    const QString root = document.dir().canonicalPath();
    const QFileInfo asset(document.dir().absoluteFilePath(relative));
    const QString canonical = asset.canonicalFilePath();
    if (root.isEmpty() || canonical.isEmpty() || !asset.isFile() || asset.size() <= 0 ||
        asset.size() > 8 * 1024 * 1024 ||
        !canonical.startsWith(root + QDir::separator()))
        return false;

    const QString suffix = asset.suffix().toLower();
    const QHash<QString, QByteArray> types{
        {QStringLiteral("png"), QByteArrayLiteral("image/png")},
        {QStringLiteral("jpg"), QByteArrayLiteral("image/jpeg")},
        {QStringLiteral("jpeg"), QByteArrayLiteral("image/jpeg")},
        {QStringLiteral("webp"), QByteArrayLiteral("image/webp")},
        {QStringLiteral("gif"), QByteArrayLiteral("image/gif")},
    };
    if (!types.contains(suffix))
        return false;
    QFile file(canonical);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    sendResponse(socket, 200, types.value(suffix), file.readAll());
    return true;
}

void DocumentHttpServer::sendResponse(QTcpSocket *socket, int status, const QByteArray &contentType,
                                      const QByteArray &body) {
    QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + ' ' + statusText(status) + "\r\n";
    response += "Content-Type: " + contentType + "\r\n";
    response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    response += "Cache-Control: no-store\r\n";
    response += "Connection: close\r\n";
    response += "X-Content-Type-Options: nosniff\r\n";
    if (contentType.startsWith(QByteArrayLiteral("text/html")))
        response += "Content-Security-Policy: default-src 'none'; img-src 'self' data:; style-src 'self' 'unsafe-inline'; script-src 'unsafe-inline'; connect-src 'self'\r\n";
    response += "\r\n";
    response += body;
    socket->write(response);
    socket->disconnectFromHost();
}

QJsonObject DocumentHttpServer::state() {
    if (m_controller) {
        QJsonObject result = m_controller->apiState();
        result.insert(QStringLiteral("instructions"), m_instructions);
        return result;
    }
    const FileResult file = FileDocument::read(m_path);
    if (!file.ok)
        return errorObject(QStringLiteral("read_failed"), file.error);
    m_session->setSource(file.source);
    if (QFileInfo::exists(QFileInfo(m_path).canonicalFilePath() + ".mustermark.json")) {
        const auto linked = FileDocument::readLinked(m_path, *m_session);
        if (!linked.ok) return errorObject(linked.error, linked.error);
    }
    const Document &document = m_session->document();
    QJsonObject result = document.toJson();
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("path"), m_path);
    result.insert(QStringLiteral("name"), QFileInfo(m_path).fileName());
    result.insert(QStringLiteral("status"), QStringLiteral("Watching for changes"));
    result.insert(QStringLiteral("conflict"), false);
    result.insert(QStringLiteral("source"), QString::fromUtf8(document.source));
    result.insert(QStringLiteral("html"), document.renderedHtml);
    result.insert(QStringLiteral("instructions"), m_instructions);
    return result;
}

QJsonObject DocumentHttpServer::applyInstruction(const QJsonObject &instruction) {
    if (m_controller)
        return m_controller->applyApiInstruction(instruction);

    const FileResult file = FileDocument::read(m_path);
    if (!file.ok)
        return errorObject(QStringLiteral("read_failed"), file.error);
    m_session->setSource(file.source);
    if (QFileInfo::exists(QFileInfo(m_path).canonicalFilePath() + ".mustermark.json")) {
        const auto linked = FileDocument::readLinked(m_path, *m_session);
        if (!linked.ok) return errorObject(linked.error, linked.error);
    }
    const QString revision = m_session->document().revision;
    const QString expected = instruction.value(QStringLiteral("baseRevision")).toString();
    if (expected.isEmpty())
        return errorObject(QStringLiteral("base_revision_required"),
                           QStringLiteral("baseRevision is required."));
    if (expected != revision)
        return errorObject(QStringLiteral("stale_revision"),
                           QStringLiteral("The document changed after this instruction was prepared."));

    const QString action = instruction.value(QStringLiteral("action")).toString();
    if (action == QStringLiteral("snapshot"))
        return m_session->snapshot(
            m_path, instruction.value(QStringLiteral("node")).toString(),
            instruction.value(QStringLiteral("scope")).toString(QStringLiteral("item")));
    if (action == QStringLiteral("undo")) {
        if (m_undo.isEmpty())
            return errorObject(QStringLiteral("nothing_to_undo"),
                               QStringLiteral("There is no Visual change to undo."));
        const UndoEntry entry = m_undo.constLast();
        if (entry.resultingRevision != revision) {
            m_undo.clear();
            return errorObject(
                QStringLiteral("nothing_to_undo"),
                QStringLiteral("The document changed outside Visual Mode; its undo history was cleared."));
        }
        const FileResult written = FileDocument::writeAtomic(m_path, entry.source, revision);
        if (!written.ok)
            return errorObject(written.error == QStringLiteral("stale_revision")
                                   ? QStringLiteral("stale_revision")
                                   : QStringLiteral("write_failed"), written.error);
        m_session->setSource(entry.source);
        m_undo.removeLast();
        const Document &document = m_session->document();
        QJsonObject result = document.toJson();
        result.insert(QStringLiteral("ok"), true);
        result.insert(QStringLiteral("path"), m_path);
        result.insert(QStringLiteral("source"), QString::fromUtf8(document.source));
        result.insert(QStringLiteral("html"), document.renderedHtml);
        result.insert(QStringLiteral("edits"), QJsonArray{
            QJsonObject{{QStringLiteral("kind"), QStringLiteral("undo")}}
        });
        return result;
    }

    const QByteArray previousSource = m_session->document().source;
    EditResult edit;
    if (action == QStringLiteral("track") || action == QStringLiteral("tracking_start"))
        edit = m_session->startTracking();
    else if (action == QStringLiteral("untrack") || action == QStringLiteral("tracking_stop"))
        edit = m_session->stopTracking();
    else
        edit = m_session->apply(revision, action,
                                instruction.value(QStringLiteral("node")).toString(), instruction);
    if (!edit.ok)
        return errorObject(edit.errorCode, edit.errorMessage);
    if (previousSource != m_session->document().source) {
        const FileResult written = FileDocument::writeAtomic(
            m_path, m_session->document().source, revision, m_session);
        if (!written.ok) {
            m_session->setSource(previousSource);
            return errorObject(written.error == QStringLiteral("stale_revision")
                                   ? QStringLiteral("stale_revision")
                                   : QStringLiteral("write_failed"), written.error);
        }
        if (!m_undo.isEmpty() && m_undo.constLast().resultingRevision != revision)
            m_undo.clear();
        m_undo.append({previousSource, m_session->document().revision});
        constexpr qsizetype maxUndoEntries = 100;
        while (m_undo.size() > maxUndoEntries)
            m_undo.removeFirst();
    }

    const Document &document = m_session->document();
    QJsonObject result = document.toJson();
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("path"), m_path);
    result.insert(QStringLiteral("source"), QString::fromUtf8(document.source));
    result.insert(QStringLiteral("html"), document.renderedHtml);
    result.insert(QStringLiteral("edits"), edit.edits);
    return result;
}

void DocumentHttpServer::recordInstruction(const QJsonObject &instruction,
                                           const QJsonObject &result) {
    QJsonObject entry{
        {QStringLiteral("sequence"), ++m_sequence},
        {QStringLiteral("at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("instruction"), instruction},
        {QStringLiteral("ok"), result.value(QStringLiteral("ok"))},
    };
    if (result.value(QStringLiteral("ok")).toBool())
        entry.insert(QStringLiteral("revision"), result.value(QStringLiteral("revision")));
    else
        entry.insert(QStringLiteral("error"), result.value(QStringLiteral("error")));
    m_instructions.prepend(entry);
    while (m_instructions.size() > 20)
        m_instructions.removeLast();
}

QByteArray DocumentHttpServer::styleSheet() const {
    QVariantMap theme{
        {QStringLiteral("background"), QStringLiteral("#160f12")},
        {QStringLiteral("foreground"), QStringLiteral("#d9cfc2")},
        {QStringLiteral("accent"), QStringLiteral("#c7a6b7")},
        {QStringLiteral("muted"), QStringLiteral("#81777a")},
        {QStringLiteral("selection"), QStringLiteral("#34282d")},
        {QStringLiteral("selectionForeground"), QStringLiteral("#d9cfc2")},
    };
    if (m_controller) {
        const QVariantMap controllerTheme = m_controller->theme();
        for (auto it = controllerTheme.cbegin(); it != controllerTheme.cend(); ++it)
            theme.insert(it.key(), it.value());
    } else {
        QFile file(QDir::homePath() + QStringLiteral("/.local/state/omarchy/current/theme/colors.toml"));
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QRegularExpression assignment(
                QStringLiteral(R"rx(^\s*([a-z0-9_]+)\s*=\s*"(#[0-9a-fA-F]{6,8})")rx"));
            while (!file.atEnd()) {
                const auto match = assignment.match(QString::fromUtf8(file.readLine()));
                if (!match.hasMatch())
                    continue;
                const QString name = match.captured(1);
                const QString color = match.captured(2);
                if (name == QStringLiteral("background") || name == QStringLiteral("foreground") ||
                    name == QStringLiteral("accent"))
                    theme.insert(name, color);
                else if (name == QStringLiteral("color8"))
                    theme.insert(QStringLiteral("muted"), color);
                else if (name == QStringLiteral("selection_background"))
                    theme.insert(QStringLiteral("selection"), color);
                else if (name == QStringLiteral("selection_foreground"))
                    theme.insert(QStringLiteral("selectionForeground"), color);
            }
        }
    }

    const QColor background(theme.value(QStringLiteral("background")).toString());
    const QString panel = background.isValid() ? background.lighter(108).name()
                                               : QStringLiteral("#1d1518");
    const QString line = background.isValid() ? background.lighter(190).name()
                                              : QStringLiteral("#34282d");
    QByteArray css = QStringLiteral(
        ":root{--bg:%1;--panel:%2;--fg:%3;--muted:%4;--accent:%5;--line:%6;"
        "--selection-bg:%7;--selection-fg:%8}\n")
        .arg(theme.value(QStringLiteral("background")).toString(), panel,
             theme.value(QStringLiteral("foreground")).toString(),
             theme.value(QStringLiteral("muted")).toString(),
             theme.value(QStringLiteral("accent")).toString(), line,
             theme.value(QStringLiteral("selection")).toString(),
             theme.value(QStringLiteral("selectionForeground")).toString()).toUtf8();
    QByteArray currentStyle = m_style;
    if (!m_stylePath.isEmpty()) {
        QFile styleFile(m_stylePath);
        if (styleFile.open(QIODevice::ReadOnly) && styleFile.size() <= 256 * 1024)
            currentStyle = styleFile.readAll();
    }
    css += currentStyle;
    return css;
}

QByteArray DocumentHttpServer::page() const {
    QByteArray html = QByteArrayLiteral(R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Mustermark HTML</title>
<style>
:root{color-scheme:dark;--bg:#160f12;--panel:#1d1518;--fg:#d9cfc2;--muted:#81777a;--accent:#c7a6b7;--line:#34282d;--ok:#a9bd9c;--bad:#d28f8f}
*{box-sizing:border-box}::selection{background:var(--selection-bg);color:var(--selection-fg)}body{margin:0;background:var(--bg);color:var(--fg);font-family:ui-monospace,SFMono-Regular,Consolas,monospace}
header{height:38px;border-bottom:1px solid var(--line);display:flex;align-items:center;padding:0 22px;gap:18px;font-size:12px;color:var(--muted)}
header strong{color:var(--accent);font-size:13px;white-space:nowrap}header button{border:0;background:transparent;color:var(--muted);font:inherit;cursor:pointer;padding:5px 0;white-space:nowrap}header button:hover{color:var(--fg)}#instruction-toggle{margin-left:auto}#message{color:var(--accent)}#message.bad{color:var(--bad)}#revision{max-width:28vw;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
main{display:grid;grid-template-columns:minmax(0,1fr) minmax(260px,.45fr);min-height:calc(100vh - 38px)}
body.instructions-hidden main{grid-template-columns:1fr}body.instructions-hidden #stream,body.instructions-hidden #revision{display:none}
#document{padding:22px clamp(20px,4vw,52px) 36px;font-family:ui-monospace,SFMono-Regular,Consolas,monospace;font-size:16px;line-height:1.35;transition:background .2s}
#document.changed{background:#21171b}h1,h2,h3,h4,h5,h6{margin:1em 0 .35em;color:var(--fg)}h1:first-child{margin-top:0}h1{font-size:1.45em}h2{font-size:1.25em}h3{font-size:1.1em}
p{margin:.5em 0}ul,ol{margin:.45em 0;padding-left:1.45em}li{position:relative;margin:.08em 0}li>p{margin:.2em 0}li>input+p{display:inline;margin:0}input[type=checkbox]{accent-color:var(--accent);margin-right:.45em}
[data-mm-task=true][data-mm-checked=true]>.mm-task-text,[data-mm-task=true][data-mm-checked=true]>p{text-decoration:line-through;color:var(--muted)}
[data-mm-id]{scroll-margin:3em}.mm-label{display:inline-block;margin-left:.8em;padding:.08em .48em;border:1px solid var(--line);color:var(--accent);font-size:.68em;vertical-align:.12em}
.mm-structure{position:relative;transition:outline-color .12s,background-color .12s}.mm-structure.mm-hovered{outline:1px solid color-mix(in srgb,var(--accent) 45%,transparent);outline-offset:3px}.mm-structure.mm-active-item{outline:1px solid var(--accent);outline-offset:3px}.mm-structure.dragging{opacity:.38}.mm-structure.drop-before{box-shadow:inset 0 2px var(--accent)}.mm-structure.drop-after{box-shadow:inset 0 -2px var(--accent)}
.mm-controls{position:fixed;z-index:20;top:0;left:0;display:flex;align-items:center;gap:2px;padding:4px;border:1px solid var(--line);border-radius:8px;background:var(--panel);box-shadow:0 6px 18px #0007;opacity:0;pointer-events:none;transform:translateY(-2px);transition:opacity .12s,transform .12s}.mm-hovered>.mm-controls,.mm-controls:focus-within{opacity:1;pointer-events:auto;transform:none}.mm-controls button{min-width:30px;height:28px;padding:0 7px;border:0;border-radius:5px;background:transparent;color:var(--muted);font:13px/1 ui-monospace,SFMono-Regular,Consolas,monospace;cursor:pointer}.mm-controls button:hover,.mm-controls button:focus-visible{outline:0;background:color-mix(in srgb,var(--accent) 18%,transparent);color:var(--fg)}.mm-controls button:disabled{opacity:.28;cursor:default}.mm-controls .mm-danger:hover{background:color-mix(in srgb,var(--bad) 18%,transparent);color:var(--bad)}.mm-controls .mm-drag-handle{cursor:grab;color:var(--accent)}.mm-controls .mm-drag-handle:active{cursor:grabbing}
.mm-inline-create{outline:1px dashed var(--accent);outline-offset:3px;background:color-mix(in srgb,var(--accent) 7%,transparent)}.mm-inline-create-list{margin:.45em 0}.mm-inline-create textarea.mm-inline-input{width:min(38rem,calc(100% - 12px));min-height:2.1em;resize:vertical;padding:3px 5px;border:0;border-bottom:1px solid var(--accent);outline:0;background:transparent;color:var(--fg);font:inherit}.mm-inline-create textarea.mm-inline-input::placeholder{color:var(--muted)}.mm-inline-create>input[type=checkbox]{vertical-align:middle}.mm-inline-edit{margin:.45em 0}.mm-inline-editor{display:block;width:100%;min-height:3.25em;max-height:60vh;resize:none;overflow:auto;padding:9px 11px;border:1px solid var(--accent);border-radius:6px;outline:0;background:var(--bg);color:var(--fg);font:14px/1.4 ui-monospace,SFMono-Regular,Consolas,monospace;tab-size:4;scrollbar-width:thin;scrollbar-color:var(--muted) transparent;box-shadow:0 0 0 2px color-mix(in srgb,var(--accent) 12%,transparent)}.mm-inline-editor::-webkit-scrollbar{width:6px;height:6px}.mm-inline-editor::-webkit-scrollbar-track{background:transparent}.mm-inline-editor::-webkit-scrollbar-thumb{border-radius:999px;background:var(--muted)}
li[data-mm-id]:hover{background:rgba(199,166,183,.07)}li[data-mm-id]:hover::before{content:attr(data-mm-id);position:absolute;right:100%;padding-right:12px;color:var(--muted);font-size:9px;white-space:nowrap}
#stream{border-left:1px solid var(--line);background:var(--panel);padding:16px;overflow:auto;max-height:calc(100vh - 38px)}
#stream h2{margin:0 0 12px;color:var(--accent);font-size:13px;font-weight:700}.entry{border-top:1px solid var(--line);padding:10px 0}.entry:first-of-type{border-top:0}
.entry-head{display:flex;gap:9px;font-size:11px;color:var(--muted)}.entry .ok{color:var(--ok)}.entry .bad{color:var(--bad)}pre{margin:9px 0 0;white-space:pre-wrap;word-break:break-word;color:var(--fg);font:11px/1.45 ui-monospace,SFMono-Regular,Consolas,monospace}
.entry-error{display:grid;gap:4px;margin-top:9px;padding:8px;border:1px solid color-mix(in srgb,var(--bad) 55%,transparent);border-radius:5px;background:color-mix(in srgb,var(--bad) 9%,transparent);color:var(--bad);font-size:11px;line-height:1.4}.entry-error code{font:700 10px/1.3 ui-monospace,SFMono-Regular,Consolas,monospace;text-transform:uppercase;letter-spacing:.04em}.entry-error span{color:var(--fg)}
.mm-attachment{position:relative;display:inline-block;margin-left:-2px;color:var(--accent);cursor:pointer;vertical-align:middle}
.mm-attachment-icon{display:inline-grid;place-items:center;width:1.15rem;height:1.15rem;border:1px solid color-mix(in srgb,var(--accent) 65%,transparent);border-radius:3px;font:700 12px/1 monospace}
.mm-attachment-preview{display:none;position:fixed;z-index:1000;right:20px;bottom:20px;padding:8px;background:var(--bg);border:1px solid var(--accent);box-shadow:0 12px 36px #0009;pointer-events:none}
.mm-attachment.mm-open .mm-attachment-preview{display:block;pointer-events:auto}.mm-attachment-preview img{display:block;max-width:min(70vw,720px);max-height:75vh;width:auto;height:auto;user-select:none}
#mm-dialog{width:min(430px,calc(100vw - 32px));padding:0;border:1px solid var(--line);border-radius:10px;background:var(--panel);color:var(--fg);box-shadow:0 24px 70px #000b;font:14px/1.45 ui-monospace,SFMono-Regular,Consolas,monospace}#mm-dialog::backdrop{background:#0009;backdrop-filter:blur(2px)}#mm-dialog [hidden]{display:none!important}#mm-dialog form{display:grid;gap:14px;padding:20px}#mm-dialog h2{margin:0;color:var(--accent);font-size:16px}#mm-dialog-description{margin:0;color:var(--muted)}#mm-dialog-field{display:grid;gap:7px;color:var(--muted);font-size:12px}#mm-dialog input[type=text]{width:100%;padding:10px 11px;border:1px solid var(--line);border-radius:6px;outline:0;background:var(--bg);color:var(--fg);font:14px/1.3 inherit}#mm-dialog input[type=text]:focus{border-color:var(--accent);box-shadow:0 0 0 2px color-mix(in srgb,var(--accent) 20%,transparent)}#mm-dialog-option{display:flex;align-items:center;gap:8px;color:var(--fg);font-size:12px;cursor:pointer}#mm-dialog-option input{margin:0;accent-color:var(--accent)}.mm-dialog-actions{display:flex;justify-content:flex-end;gap:8px;margin-top:2px}.mm-dialog-actions button{min-width:84px;padding:8px 12px;border:1px solid var(--line);border-radius:6px;background:transparent;color:var(--fg);font:12px/1 inherit;cursor:pointer}.mm-dialog-actions button:hover,.mm-dialog-actions button:focus-visible{border-color:var(--accent);outline:0}.mm-dialog-actions .primary{border-color:var(--accent);background:var(--accent);color:var(--bg)}.mm-dialog-actions .danger{border-color:var(--bad);background:var(--bad);color:var(--bg)}
#empty{color:var(--muted);font-size:12px}@media(max-width:1100px){header{padding:0 16px;gap:12px}#path,#revision{display:none}}@media(max-width:820px){main{grid-template-columns:1fr}#stream{border-left:0;border-top:1px solid var(--line);max-height:none}#document{min-height:55vh}}
</style>
<link id="html-style" rel="stylesheet" href="/style.css">
</head>
<body>
<header><strong>mustermark html</strong><span id="path"></span><span id="message"></span><button id="instruction-toggle" type="button">hide diagnostics</button><span id="revision"></span></header>
<main><article id="document"></article><aside id="stream"><h2>API instructions</h2><div id="entries"><span id="empty">Waiting for instructions.</span></div></aside></main>
<dialog id="mm-dialog"><form method="dialog"><h2 id="mm-dialog-title"></h2><p id="mm-dialog-description"></p><label id="mm-dialog-field"><span id="mm-dialog-label"></span><input id="mm-dialog-input" type="text" autocomplete="off"></label><label id="mm-dialog-option"><input id="mm-dialog-option-input" type="checkbox"><span id="mm-dialog-option-label"></span></label><div class="mm-dialog-actions"><button value="cancel">Cancel</button><button id="mm-dialog-confirm" class="primary" value="confirm">Apply</button></div></form></dialog>
<script>
const token="__MUSTERMARK_TOKEN__";
let currentState=null;
let dragged=null;
let activeItemRef=null;
let activeDraft=null;
let refreshSequence=0;
let appliedRefreshSequence=0;
const instructionToggle=document.getElementById("instruction-toggle");
function toggleInstructions(){const hidden=document.body.classList.toggle("instructions-hidden");instructionToggle.textContent=hidden?"show diagnostics":"hide diagnostics";instructionToggle.setAttribute("aria-pressed",hidden?"false":"true")}
instructionToggle.addEventListener("click",toggleInstructions);
function toggleAttachment(attachment){const shouldOpen=!attachment.classList.contains("mm-open");document.querySelectorAll(".mm-attachment.mm-open").forEach(item=>item.classList.remove("mm-open"));if(shouldOpen)attachment.classList.add("mm-open")}
document.addEventListener("click",event=>{const attachment=event.target.closest(".mm-attachment");if(attachment)toggleAttachment(attachment)});
const embeddedPreview=location.hash==="#preview";
if(embeddedPreview)toggleInstructions();
const message=document.getElementById("message");
function setMessage(value,bad=false){message.textContent=value;message.classList.toggle("bad",bad);if(value)setTimeout(()=>{if(message.textContent===value)message.textContent=""},1800)}
function instructionEntry(value){
  const row=document.createElement("section");row.className="entry";
  const head=document.createElement("div");head.className="entry-head";
  const number=document.createElement("span");number.textContent="#"+value.sequence;
  const origin=document.createElement("span");origin.textContent=value.instruction.origin==="ui"?"UI":"API";
  const action=document.createElement("span");action.textContent=value.instruction.action||"unknown";
  const status=document.createElement("span");status.className=value.ok?"ok":"bad";status.textContent=value.ok?"applied":"rejected";
  head.append(number,origin,action,status);const payload=document.createElement("pre");payload.textContent=JSON.stringify(value.instruction,null,2);
  row.append(head,payload);
  if(!value.ok){const error=document.createElement("div");error.className="entry-error";error.setAttribute("role","alert");const code=document.createElement("code");code.textContent=value.error?.code||"error";const message=document.createElement("span");message.textContent=value.error?.message||"The instruction was rejected.";error.append(code,message);row.append(error)}
  return row;
}
async function action(payload,options={}){
  if(!currentState)return null;
  payload.baseRevision=options.baseRevision||currentState.revision;
  const response=await fetch("/api/actions",{method:"POST",headers:{"Content-Type":"application/json","X-Mustermark-Token":token},body:JSON.stringify(payload)});
  const result=await response.json();
  if(!result.ok){setMessage(result.error?.message||"action rejected",true);if(!options.skipRefresh)await refresh();return null}
  if(!options.skipRefresh)await refresh();return result;
}
function clearDropMarks(){document.querySelectorAll(".drop-before,.drop-after").forEach(node=>node.classList.remove("drop-before","drop-after"))}
function selectHostLine(line){if(!currentState)return false;const node=currentState.nodes.find(node=>node.startLine===line);if(!node)return false;const element=document.querySelector('[data-mm-ref="'+CSS.escape(node.ref)+'"]');if(!element)return false;setActiveItem(element);element.scrollIntoView({block:"center"});return true}
function stateNode(ref,state=currentState){return state?.nodes.find(node=>node.ref===ref)||null}
function setActiveItem(element){document.querySelectorAll(".mm-active-item").forEach(item=>item.classList.remove("mm-active-item"));if(!element){activeItemRef=null;return}activeItemRef=element.dataset.mmRef;element.classList.add("mm-active-item")}
function compatible(source,target){
  if(!source||!target||source.ref===target.ref)return false;
  const sourceBlock=source.kind==="list"||source.kind==="block";
  const targetBlock=target.kind==="list"||target.kind==="block";
  if(sourceBlock||targetBlock){const sourceParent=stateNode(source.parent);const targetParent=stateNode(target.parent);const sourceTop=!source.parent||sourceParent?.kind==="heading";const targetTop=!target.parent||targetParent?.kind==="heading";return sourceBlock&&targetBlock&&((source.parent||"")===(target.parent||"")||(sourceTop&&targetTop))}
  if((source.parent||"")!==(target.parent||""))return false;
  if(source.kind!==target.kind)return false;
  return source.kind!=="heading"||source.level===target.level;
}
function relativeTarget(node,direction,state){
  const start=state.nodes.findIndex(candidate=>candidate.ref===node.ref);
  for(let index=start+direction;index>=0&&index<state.nodes.length;index+=direction)
    if(compatible(node,state.nodes[index]))return state.nodes[index];
  return null;
}
function controlButton(label,title,handler,className=""){
  const button=document.createElement("button");button.type="button";button.textContent=label;button.title=title;button.setAttribute("aria-label",title);button.className=className;button.addEventListener("click",event=>{event.preventDefault();event.stopPropagation();handler()});return button;
}
const modalDialog=document.getElementById("mm-dialog");
const modalTitle=document.getElementById("mm-dialog-title");
const modalDescription=document.getElementById("mm-dialog-description");
const modalField=document.getElementById("mm-dialog-field");
const modalLabel=document.getElementById("mm-dialog-label");
const modalInput=document.getElementById("mm-dialog-input");
const modalOption=document.getElementById("mm-dialog-option");
const modalOptionInput=document.getElementById("mm-dialog-option-input");
const modalOptionLabel=document.getElementById("mm-dialog-option-label");
const modalConfirm=document.getElementById("mm-dialog-confirm");
modalDialog.addEventListener("click",event=>{if(event.target===modalDialog)modalDialog.close("cancel")});
function openDialog({title,description="",inputLabel="",value="",optionLabel="",confirmText="Apply",danger=false}){
  modalTitle.textContent=title;modalDescription.textContent=description;modalDescription.hidden=!description;
  modalField.hidden=!inputLabel;modalLabel.textContent=inputLabel;modalInput.value=value;modalInput.required=Boolean(inputLabel);
  modalOption.hidden=!optionLabel;modalOptionLabel.textContent=optionLabel;modalOptionInput.checked=false;
  modalConfirm.textContent=confirmText;modalConfirm.className=danger?"danger":"primary";modalDialog.returnValue="";
  modalDialog.showModal();if(inputLabel)requestAnimationFrame(()=>{modalInput.focus();modalInput.select()});
  return new Promise(resolve=>modalDialog.addEventListener("close",()=>resolve(modalDialog.returnValue==="confirm"?(inputLabel?modalInput.value.trim():true):null),{once:true}));
}
let skipRemovalConfirmations=sessionStorage.getItem("mustermark.skipRemovalConfirmations")==="true";
async function confirmRemoval(noun){
  const confirmed=await openDialog({title:"Remove structure?",description:"This will remove the "+noun+" from the Markdown file.",optionLabel:"Don't ask again for removals in this preview",confirmText:"Remove",danger:true});
  if(confirmed&&modalOptionInput.checked){skipRemovalConfirmations=true;sessionStorage.setItem("mustermark.skipRemovalConfirmations","true")}
  return confirmed;
}
function isEmptyStructure(node){if(node.kind==="heading"||node.kind==="list")return !node.children?.length;if(node.kind==="item")return !node.text?.trim()&&!node.children?.length&&!node.attachments?.length;return false}
function inlineInput(placeholder,removeRoot,payload,label,task=false){
  if(task){const box=document.createElement("input");box.type="checkbox";box.disabled=true;placeholder.append(box)}
  const input=document.createElement("textarea");input.className="mm-inline-input";input.rows=1;input.placeholder=label;input.setAttribute("aria-label",label);placeholder.append(input);
  const draft={kind:"create",targetId:payload.node,openingRevision:currentState.revision,originalSource:currentState.source,input,removeRoot,payload,invalid:false};activeDraft=draft;
  let submitting=false;const cancel=async()=>{if(activeDraft===draft)activeDraft=null;if(removeRoot.isConnected)removeRoot.remove();await refresh(true)};
  input.addEventListener("keydown",async event=>{
    event.stopPropagation();
    if(event.key==="Escape"){event.preventDefault();cancel();return}
    if(event.key!=="Enter"||event.shiftKey||event.isComposing)return;
    event.preventDefault();const text=input.value.trim();if(!text){cancel();return}
    if(draft.invalid){setMessage("The insertion target changed. Copy the draft or press Escape.",true);return}
    submitting=true;input.disabled=true;const result=await action({...payload,text},{baseRevision:draft.openingRevision,skipRefresh:true});if(result){if(activeDraft===draft)activeDraft=null;await refresh(true)}else if(input.isConnected){submitting=false;input.disabled=false;input.focus()}
  });
  requestAnimationFrame(()=>input.focus());
}
function clearInlineCreate(rerender=true){const temporaryList=document.querySelector(".mm-inline-create-list");if(temporaryList){if(activeDraft?.kind==="create")activeDraft=null;temporaryList.remove();if(rerender)refresh(true);return true}const placeholder=document.querySelector(".mm-inline-create");if(placeholder){if(activeDraft?.kind==="create")activeDraft=null;placeholder.remove();if(rerender)refresh(true);return true}return false}
function cancelTransientUi(){if(modalDialog.open){modalDialog.close("cancel");return true}const attachment=document.querySelector(".mm-attachment.mm-open");if(attachment){attachment.classList.remove("mm-open");return true}return clearInlineEdit()||clearInlineCreate()}
function beginInlineItem(element,node,state,requestedKind=""){
  clearInlineCreate(false);clearInlineEdit(false);
  const item=document.createElement("li");item.className="mm-inline-create";
  let removeRoot=item;let payload;let task=requestedKind?requestedKind==="task":true;
  if(node.kind==="heading"){
    const list=state.nodes.find(candidate=>candidate.kind==="list"&&candidate.parent===node.ref);
    const listElement=list&&Array.from(document.querySelectorAll("[data-mm-ref]")).find(candidate=>candidate.dataset.mmRef===list.ref);
    if(listElement){listElement.append(item);payload={action:"item_add",node:list.ref,task,origin:"ui"}}
    else{const temporaryList=document.createElement("ul");temporaryList.className="mm-inline-create-list";temporaryList.append(item);element.insertAdjacentElement("afterend",temporaryList);removeRoot=temporaryList;payload={action:"list_add",node:node.ref,task,origin:"ui"}}
  }else if(node.kind==="list"){
    element.append(item);if(!requestedKind)task=state.nodes.some(candidate=>candidate.parent===node.ref&&candidate.task);payload={action:"item_add",node:node.ref,task,origin:"ui"};
  }else{
    element.insertAdjacentElement("afterend",item);if(!requestedKind)task=node.task;payload={action:"item_add",node:node.ref,task,origin:"ui"};
  }
  inlineInput(item,removeRoot,payload,"New item",task);item.scrollIntoView({block:"nearest"});
}
function beginExternalAppend(section,kind){
  if(!currentState)return null;
  const storageKey="mustermark.append.section:"+currentState.path;
  const requestedSection=section||localStorage.getItem(storageKey)||"";
  let matches=requestedSection?currentState.nodes.filter(node=>node.kind==="heading"&&node.text===requestedSection):[];
  let heading=null;
  if(requestedSection&&matches.length!==1){setMessage(matches.length?"More than one section is named "+requestedSection:"Section not found: "+requestedSection,true);return false}
  if(matches.length===1)heading=matches[0];
  if(!heading&&activeItemRef){let node=stateNode(activeItemRef);while(node&&node.kind!=="heading")node=stateNode(node.parent);heading=node}
  if(!heading)heading=currentState.nodes.find(node=>node.kind==="heading")||null;
  if(!heading){setMessage("Add a heading before appending an item",true);return false}
  const element=document.querySelector('[data-mm-ref="'+CSS.escape(heading.ref)+'"]');
  if(!element){setMessage("The append section is not visible",true);return false}
  localStorage.setItem(storageKey,heading.text);
  beginInlineItem(element,heading,currentState,kind||"task");
  setMessage("Ready for text in "+heading.text);
  return true;
}
function beginInlineSection(element,node){
  clearInlineCreate(false);clearInlineEdit(false);
  const heading=document.createElement("h"+node.level);heading.className="mm-inline-create";
  let boundary=element.nextElementSibling;
  while(boundary&&(!/^H[1-6]$/.test(boundary.tagName)||Number(boundary.tagName.slice(1))>node.level))boundary=boundary.nextElementSibling;
  if(boundary)boundary.before(heading);else element.parentElement.append(heading);
  inlineInput(heading,heading,{action:"section_add",node:node.ref,origin:"ui"},"New section");heading.scrollIntoView({block:"nearest"});
}
function sourceContents(node,state=currentState){const bytes=new TextEncoder().encode(state.source);return new TextDecoder().decode(bytes.slice(node.startByte,node.endByte))}
function clearInlineEdit(rerender=true){const wrapper=document.querySelector(".mm-inline-edit");if(!wrapper)return false;if(activeDraft?.kind==="edit")activeDraft=null;if(wrapper.originalElement?.isConnected)wrapper.originalElement.hidden=false;wrapper.remove();if(rerender)refresh(true);return true}
function beginInlineEdit(element,node){
  clearInlineCreate(false);clearInlineEdit(false);
  const wrapper=document.createElement("div");wrapper.className="mm-inline-edit";wrapper.originalElement=element;
  const editor=document.createElement("textarea");editor.className="mm-inline-editor";editor.setAttribute("aria-label","Edit "+node.kind+" Markdown");editor.value=sourceContents(node);wrapper.append(editor);element.before(wrapper);element.hidden=true;
  const draft={kind:"edit",targetId:node.ref,openingRevision:currentState.revision,originalSource:currentState.source,originalMarkdown:editor.value,editor,wrapper,invalid:false};activeDraft=draft;
  const height=Math.min(window.innerHeight*.6,Math.max(element.getBoundingClientRect().height,editor.value.split("\n").length*20+22));editor.style.height=height+"px";
  editor.addEventListener("keydown",async event=>{if(event.key==="Escape"){event.preventDefault();event.stopPropagation();clearInlineEdit();return}if(event.key==="Enter"&&!event.shiftKey){event.preventDefault();event.stopPropagation();const markdown=editor.value;if(markdown===draft.originalMarkdown){clearInlineEdit();return}if(draft.invalid){setMessage("The edited structure changed. Copy the draft or press Escape.",true);return}editor.disabled=true;const result=await action({action:"replace",node:draft.targetId,markdown,origin:"ui"},{baseRevision:draft.openingRevision,skipRefresh:true});if(result){if(activeDraft===draft)activeDraft=null;await refresh(true);setMessage("structure edited")}else{editor.disabled=false;editor.focus()}}});
  requestAnimationFrame(()=>{editor.focus();const firstLine=editor.value.split("\n",1)[0];const marker=/^(?:\s*#{1,6}\s+|\s*(?:[-+*]|\d+[.)])\s+(?:\[[ xX]\]\s*)?)/.exec(firstLine);const cursor=marker?marker[0].length:0;editor.setSelectionRange(cursor,cursor)});
}
async function uploadImage(node,file){
  if(!file||!file.type.startsWith("image/"))return null;
  if(file.size<1||file.size>8*1024*1024){setMessage("images must be between 1 byte and 8 MiB",true);return null}
  const query=new URLSearchParams({node:node.ref,baseRevision:currentState.revision,filename:file.name||"pasted-image"});
  try{const response=await fetch("/api/attachments?"+query,{method:"POST",headers:{"Content-Type":file.type,"X-Mustermark-Token":token},body:await file.arrayBuffer()});const result=await response.json();if(!result.ok){setMessage(result.error?.message||"image could not be attached",true);await refresh();return null}await refresh();setMessage("image attached");return result}catch(error){setMessage("image could not be attached",true);return null}
}
function chooseImage(node){const picker=document.createElement("input");picker.type="file";picker.accept="image/png,image/jpeg,image/webp,image/gif";picker.hidden=true;document.body.append(picker);picker.addEventListener("change",async()=>{const file=picker.files?.[0];picker.remove();if(file)await uploadImage(node,file)},{once:true});picker.addEventListener("cancel",()=>picker.remove(),{once:true});picker.click()}
function positionControls(element,event){
  const controls=element.querySelector(":scope > .mm-controls");if(!controls)return;
  const rect=controls.getBoundingClientRect();const structure=element.getBoundingClientRect();const margin=8;const headerBottom=46;
  const left=Math.min(Math.max(event.clientX+6,margin),window.innerWidth-rect.width-margin);
  const nearestBorder=event.clientY-structure.top<=structure.bottom-event.clientY?structure.top:structure.bottom;
  const top=Math.min(Math.max(nearestBorder-rect.height/2,headerBottom),window.innerHeight-rect.height-margin);
  controls.style.left=left+"px";controls.style.top=top+"px";
}
function addControls(element,node,state){
  const controls=document.createElement("span");controls.className="mm-controls";
  const handle=controlButton("⠿","Drag to move",()=>{},"mm-drag-handle");handle.draggable=true;
  handle.addEventListener("dragstart",event=>{dragged=element;element.classList.add("dragging");event.dataTransfer.effectAllowed="move";event.dataTransfer.setData("text/plain",node.ref)});
  handle.addEventListener("dragend",()=>{element.classList.remove("dragging");dragged=null;clearDropMarks()});
  controls.append(handle);

  if(node.kind==="heading"){
    controls.append(controlButton("+","Add item",()=>beginInlineItem(element,node,state)));
    controls.append(controlButton("§+","Add section",()=>beginInlineSection(element,node)));
  }else if(node.kind==="list"){
    controls.append(controlButton("+","Add item",()=>beginInlineItem(element,node,state)));
  }else if(node.kind==="item"){
    controls.append(controlButton("+","Add item after",()=>beginInlineItem(element,node,state)));
    controls.append(controlButton("▧+","Attach image",()=>chooseImage(node)));
  }

  controls.append(controlButton("✎","Edit Markdown",()=>beginInlineEdit(element,node)));
  if(node.kind==="heading"||node.kind==="item"){
    controls.append(controlButton("←",node.kind==="heading"?"Promote section":"Outdent item",()=>action({action:"promote",node:node.ref,scope:"subtree",origin:"ui"})));
    controls.append(controlButton("→",node.kind==="heading"?"Demote section":"Indent item",()=>action({action:"demote",node:node.ref,scope:"subtree",origin:"ui"})));
  }

  const previous=relativeTarget(node,-1,state);const next=relativeTarget(node,1,state);
  const up=controlButton("↑","Move up",()=>previous&&action({action:"move_before",node:node.ref,target:previous.ref,origin:"ui"}));up.disabled=!previous;controls.append(up);
  const down=controlButton("↓","Move down",()=>next&&action({action:"move_after",node:node.ref,target:next.ref,origin:"ui"}));down.disabled=!next;controls.append(down);
  controls.append(controlButton("×","Remove",async()=>{const noun=node.kind==="heading"?"section and all of its contents":node.kind==="list"?"list and all of its items":node.kind==="block"?"block":"item and its children";if(isEmptyStructure(node)||skipRemovalConfirmations||await confirmRemoval(noun))await action({action:"delete",node:node.ref,origin:"ui"})},"mm-danger"));
  element.append(controls);
}
async function copyStructure(node){const markdown=sourceContents(node);try{await navigator.clipboard.writeText(markdown);setMessage("structure copied")}catch(error){setMessage("structure could not be copied",true)}}
async function copyImage(image){try{const response=await fetch(image.currentSrc||image.src);let blob=await response.blob();if(blob.type!=="image/png"){const bitmap=await createImageBitmap(blob);const canvas=document.createElement("canvas");canvas.width=bitmap.width;canvas.height=bitmap.height;canvas.getContext("2d").drawImage(bitmap,0,0);bitmap.close();blob=await new Promise((resolve,reject)=>canvas.toBlob(value=>value?resolve(value):reject(new Error("image conversion failed")),"image/png"))}await navigator.clipboard.write([new ClipboardItem({"image/png":blob})]);setMessage("image copied")}catch(error){setMessage("image could not be copied",true)}}
document.addEventListener("contextmenu",event=>{const image=event.target.closest(".mm-attachment-preview img");if(image){event.preventDefault();event.stopPropagation();copyImage(image);return}const element=event.target.closest(".mm-structure");if(!element)return;const node=stateNode(element.dataset.mmRef);if(!node)return;event.preventDefault();event.stopPropagation();copyStructure(node)});
document.addEventListener("paste",event=>{if(event.target.closest("textarea,input,[contenteditable=true]"))return;const node=stateNode(activeItemRef);if(!node||node.kind!=="item")return;const image=Array.from(event.clipboardData?.items||[]).find(item=>item.type.startsWith("image/"));if(!image)return;event.preventDefault();const file=image.getAsFile();if(file)uploadImage(node,file)});
document.addEventListener("keydown",async event=>{if(event.repeat||event.shiftKey||event.altKey||!(event.ctrlKey||event.metaKey)||event.key.toLowerCase()!=="z")return;if(event.target.closest("textarea,input,[contenteditable=true]"))return;event.preventDefault();event.stopPropagation();const result=await action({action:"undo",origin:"ui"});if(result)setMessage("change undone")});
function wireDocument(article,state){
  article.querySelectorAll("[data-mm-task=true]").forEach(item=>{if(item.dataset.mmTaskDecorated)return;item.dataset.mmTaskDecorated="true";Array.from(item.childNodes).forEach(child=>{if(child.nodeType!==Node.TEXT_NODE||!child.textContent.trim())return;const span=document.createElement("span");span.className="mm-task-text";span.textContent=child.textContent;child.replaceWith(span)})});
  article.querySelectorAll("[data-mm-labels]").forEach(item=>{if(item.dataset.mmLabelsDecorated)return;item.dataset.mmLabelsDecorated="true";const target=item.querySelector(":scope > p")||item;item.dataset.mmLabels.split(",").forEach(label=>{const badge=document.createElement("span");badge.className="mm-label";badge.textContent=label;target.append(badge)})});
  const structures=article.querySelectorAll("[data-mm-ref][data-mm-kind]");
  structures.forEach(element=>{
    if(element.dataset.mmStructureWired)return;const node=stateNode(element.dataset.mmRef,state);if(!node)return;element.dataset.mmStructureWired="true";element.classList.add("mm-structure");
    if(state.tracked)addControls(element,node,state);
    element.addEventListener("dragover",event=>{if(!dragged)return;const source=stateNode(dragged.dataset.mmRef,state);if(!compatible(source,node))return;event.preventDefault();event.stopPropagation();clearDropMarks();const after=event.clientY>element.getBoundingClientRect().top+element.getBoundingClientRect().height/2;element.classList.add(after?"drop-after":"drop-before")});
    element.addEventListener("dragleave",()=>element.classList.remove("drop-before","drop-after"));
    element.addEventListener("drop",async event=>{if(!dragged)return;const source=stateNode(dragged.dataset.mmRef,state);if(!compatible(source,node))return;event.preventDefault();event.stopPropagation();const after=event.clientY>element.getBoundingClientRect().top+element.getBoundingClientRect().height/2;clearDropMarks();const result=await action({action:after?"move_after":"move_before",node:source.ref,target:node.ref,origin:"ui"});if(result)setMessage("structure moved")});
  });
  const active=document.querySelector('[data-mm-ref="'+CSS.escape(activeItemRef||"")+'"]');if(active)setActiveItem(active);else activeItemRef=null;
  if(!article.dataset.mmWired){article.dataset.mmWired="true";let hovered=null;article.addEventListener("click",event=>{if(event.target.closest("button,input,textarea,.mm-attachment"))return;const item=event.target.closest('[data-mm-ref]');setActiveItem(item)});article.addEventListener("pointermove",event=>{const next=event.target.closest(".mm-structure");if(next===hovered)return;if(hovered)hovered.classList.remove("mm-hovered");hovered=next;if(hovered){hovered.classList.add("mm-hovered");positionControls(hovered,event)}});article.addEventListener("pointerleave",()=>{if(hovered)hovered.classList.remove("mm-hovered");hovered=null})}
  article.querySelectorAll("[data-mm-task=true][data-mm-id] input[type=checkbox]").forEach(box=>{box.disabled=!state.tracked;if(box.dataset.mmTaskWired)return;box.dataset.mmTaskWired="true";box.addEventListener("change",async()=>{const item=box.closest("[data-mm-task=true]");const result=await action({action:"task_set",node:item.dataset.mmRef,checked:box.checked,origin:"ui"});if(result)setMessage(box.checked?"task checked":"task unchecked")})});
}
function refreshAroundDraft(article,state){
  const template=document.createElement("template");template.innerHTML=state.html;
  const draftField=activeDraft?.editor||activeDraft?.input;
  const fresh=Array.from(template.content.querySelectorAll("[data-mm-ref][data-mm-kind]")).reverse();
  fresh.forEach(next=>{const ref=next.dataset.mmRef;const current=article.querySelector('[data-mm-ref="'+CSS.escape(ref)+'"]');if(!current||ref===activeDraft?.targetId||current.contains(draftField)||current===activeDraft?.wrapper?.originalElement)return;current.replaceWith(next.cloneNode(true))});
  wireDocument(article,state);
}
async function refresh(force=false){
  const sequence=++refreshSequence;
  const openingDraft=activeDraft;
  try{
    const response=await fetch("/api/state",{cache:"no-store"});const state=await response.json();if(!state.ok)return;
    if(sequence<appliedRefreshSequence)return;appliedRefreshSequence=sequence;
    if(activeDraft&&activeDraft!==openingDraft)force=false;
    const article=document.getElementById("document");if(currentState&&currentState.revision!==state.revision){article.classList.add("changed");setTimeout(()=>article.classList.remove("changed"),220)}
    const documentChanged=!currentState||currentState.revision!==state.revision||currentState.tracked!==state.tracked||currentState.html!==state.html;
    currentState=state;if(activeDraft&&!force){if(!stateNode(activeDraft.targetId,state)){activeDraft.invalid=true;setMessage("The draft target changed. Your text is still available.",true)}if(documentChanged)refreshAroundDraft(article,state)}else if(documentChanged||force){article.innerHTML=state.html;wireDocument(article,state)}const path=document.getElementById("path");path.textContent=state.name||state.path;path.title=state.path||"";document.getElementById("revision").textContent=state.revision;
    const entries=document.getElementById("entries");entries.replaceChildren();if(!state.instructions.length){const empty=document.createElement("span");empty.id="empty";empty.textContent="Waiting for instructions.";entries.append(empty)}else state.instructions.forEach(value=>entries.append(instructionEntry(value)));
  }catch(error){}
}
function reloadStyle(){document.getElementById("html-style").href="/style.css?v="+Date.now()}
const events=new EventSource("/api/events");events.addEventListener("change",()=>{reloadStyle();refresh()});events.onerror=()=>{};
refresh();setInterval(refresh,1500);
</script>
</body>
</html>)HTML");
    html.replace(QByteArrayLiteral("__MUSTERMARK_TOKEN__"), m_token.toUtf8());
    return html;
}

int serveDocument(const QString &path, quint16 port, const QString &stylePath) {
    if (!QFileInfo(path).isFile()) {
        QTextStream(stderr) << "mustermark: document does not exist: " << path << Qt::endl;
        return 66;
    }
    DocumentHttpServer server(path, stylePath);
    if (!server.listen(port)) {
        QTextStream(stderr) << "mustermark: " << server.errorString() << Qt::endl;
        return 1;
    }
    const QJsonObject ready{
        {QStringLiteral("ok"), true},
        {QStringLiteral("url"), QStringLiteral("http://127.0.0.1:%1/").arg(server.port())},
        {QStringLiteral("token"), server.token()},
        {QStringLiteral("path"), QFileInfo(path).absoluteFilePath()},
    };
    QTextStream(stdout) << QJsonDocument(ready).toJson(QJsonDocument::Compact) << Qt::endl;
    return QCoreApplication::exec();
}

} // namespace Mustermark
