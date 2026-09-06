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
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTextStream>
#include <QTcpSocket>
#include <QUuid>
#include <QUrl>

#include <utility>

namespace Mustermark {
namespace {

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
    if (buffer.size() > 1024 * 1024) {
        socket->setProperty("requestHandled", true);
        sendResponse(socket, 413, QByteArrayLiteral("application/json"),
                     json(errorObject(QStringLiteral("request_too_large"),
                                      QStringLiteral("Requests are limited to 1 MiB."))));
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
    if (contentLength < 0 || contentLength > 1024 * 1024) {
        socket->setProperty("requestHandled", true);
        sendResponse(socket, 413, QByteArrayLiteral("application/json"),
                     json(errorObject(QStringLiteral("request_too_large"),
                                      QStringLiteral("Requests are limited to 1 MiB."))));
        return;
    }
    if (buffer.size() - bodyStart < contentLength)
        return;
    request.body = buffer.mid(bodyStart, contentLength);
    socket->setProperty("requestHandled", true);
    handleRequest(socket, request);
}

void DocumentHttpServer::handleRequest(QTcpSocket *socket, const Request &request) {
    const QString path = QUrl::fromEncoded(request.target).path();
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

void DocumentHttpServer::sendResponse(QTcpSocket *socket, int status, const QByteArray &contentType,
                                      const QByteArray &body) {
    QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + ' ' + statusText(status) + "\r\n";
    response += "Content-Type: " + contentType + "\r\n";
    response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    response += "Cache-Control: no-store\r\n";
    response += "Connection: close\r\n";
    response += "X-Content-Type-Options: nosniff\r\n";
    if (contentType.startsWith(QByteArrayLiteral("text/html")))
        response += "Content-Security-Policy: default-src 'none'; style-src 'self' 'unsafe-inline'; script-src 'unsafe-inline'; connect-src 'self'\r\n";
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
    const Document &document = m_session->document();
    QJsonObject result = document.toJson();
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("path"), m_path);
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
    const QString revision = m_session->document().revision;
    const QString expected = instruction.value(QStringLiteral("baseRevision")).toString();
    if (expected.isEmpty())
        return errorObject(QStringLiteral("base_revision_required"),
                           QStringLiteral("baseRevision is required."));
    if (expected != revision)
        return errorObject(QStringLiteral("stale_revision"),
                           QStringLiteral("The document changed after this instruction was prepared."));

    const QString action = instruction.value(QStringLiteral("action")).toString();
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
            m_path, m_session->document().source, revision);
        if (!written.ok) {
            m_session->setSource(previousSource);
            return errorObject(written.error == QStringLiteral("stale_revision")
                                   ? QStringLiteral("stale_revision")
                                   : QStringLiteral("write_failed"), written.error);
        }
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
    css += m_style;
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
header strong{color:var(--accent);font-size:13px;white-space:nowrap}header button{border:0;background:transparent;color:var(--muted);font:inherit;cursor:pointer;padding:5px 0;white-space:nowrap}header button:hover{color:var(--fg)}#tracking-toggle{margin-left:auto}#tracking-toggle.active{color:var(--ok)}#message{color:var(--accent)}#message.bad{color:var(--bad)}#revision{max-width:28vw;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
main{display:grid;grid-template-columns:minmax(0,1.35fr) minmax(340px,.65fr);min-height:calc(100vh - 38px)}
body.instructions-hidden main{grid-template-columns:1fr}body.instructions-hidden #stream{display:none}
#document{padding:54px clamp(34px,7vw,100px);font-family:ui-monospace,SFMono-Regular,Consolas,monospace;font-size:17px;line-height:1.55;transition:background .2s}
#document.changed{background:#21171b}h1,h2,h3,h4,h5,h6{margin:1.6em 0 .65em;color:var(--fg)}h1:first-child{margin-top:0}h1{font-size:1.45em}h2{font-size:1.25em}h3{font-size:1.1em}
ul,ol{padding-left:1.55em}li{position:relative;margin:.3em 0}li>p{margin:.25em 0}li>input+p{display:inline;margin:0}input[type=checkbox]{accent-color:var(--accent);margin-right:.6em}
[data-mm-task=true][data-mm-checked=true]>.mm-task-text,[data-mm-task=true][data-mm-checked=true]>p{text-decoration:line-through;color:var(--muted)}
[data-mm-id]{scroll-margin:3em}.mm-label{display:inline-block;margin-left:.8em;padding:.08em .48em;border:1px solid var(--line);color:var(--accent);font-size:.68em;vertical-align:.12em}
[data-mm-id][draggable=true]{cursor:grab}[data-mm-id].dragging{opacity:.38}[data-mm-id].drop-before{box-shadow:inset 0 2px var(--accent)}[data-mm-id].drop-after{box-shadow:inset 0 -2px var(--accent)}
li[data-mm-id]:hover{background:rgba(199,166,183,.07)}li[data-mm-id]:hover::before{content:attr(data-mm-id);position:absolute;right:100%;padding-right:12px;color:var(--muted);font-size:9px;white-space:nowrap}
#stream{border-left:1px solid var(--line);background:var(--panel);padding:24px;overflow:auto;max-height:calc(100vh - 38px)}
#stream h2{margin:0 0 18px;color:var(--accent);font-size:13px;font-weight:700}.entry{border-top:1px solid var(--line);padding:14px 0}.entry:first-of-type{border-top:0}
.entry-head{display:flex;gap:9px;font-size:11px;color:var(--muted)}.entry .ok{color:var(--ok)}.entry .bad{color:var(--bad)}pre{margin:9px 0 0;white-space:pre-wrap;word-break:break-word;color:var(--fg);font:11px/1.45 ui-monospace,SFMono-Regular,Consolas,monospace}
#empty{color:var(--muted);font-size:12px}@media(max-width:1100px){header{padding:0 16px;gap:12px}#path,#revision{display:none}}@media(max-width:820px){main{grid-template-columns:1fr}#stream{border-left:0;border-top:1px solid var(--line);max-height:none}#document{min-height:55vh}}
</style>
<link id="html-style" rel="stylesheet" href="/style.css">
</head>
<body>
<header><strong>mustermark html</strong><span id="path"></span><span id="message"></span><button id="tracking-toggle" type="button">start tracking</button><button id="instruction-toggle" type="button">hide instructions</button><span id="revision"></span></header>
<main><article id="document"></article><aside id="stream"><h2>API instructions</h2><div id="entries"><span id="empty">Waiting for instructions.</span></div></aside></main>
<script>
const token="__MUSTERMARK_TOKEN__";
let currentState=null;
let dragged=null;
const instructionToggle=document.getElementById("instruction-toggle");
function toggleInstructions(){const hidden=document.body.classList.toggle("instructions-hidden");instructionToggle.textContent=hidden?"show instructions":"hide instructions";instructionToggle.setAttribute("aria-pressed",hidden?"false":"true")}
instructionToggle.addEventListener("click",toggleInstructions);
const trackingToggle=document.getElementById("tracking-toggle");
const message=document.getElementById("message");
function setMessage(value,bad=false){message.textContent=value;message.classList.toggle("bad",bad);if(value)setTimeout(()=>{if(message.textContent===value)message.textContent=""},1800)}
function instructionEntry(value){
  const row=document.createElement("section");row.className="entry";
  const head=document.createElement("div");head.className="entry-head";
  const number=document.createElement("span");number.textContent="#"+value.sequence;
  const action=document.createElement("span");action.textContent=value.instruction.action||"unknown";
  const status=document.createElement("span");status.className=value.ok?"ok":"bad";status.textContent=value.ok?"applied":"rejected";
  head.append(number,action,status);const payload=document.createElement("pre");payload.textContent=JSON.stringify(value.instruction,null,2);
  row.append(head,payload);return row;
}
async function action(payload){
  if(!currentState)return null;
  payload.baseRevision=currentState.revision;
  const response=await fetch("/api/actions",{method:"POST",headers:{"Content-Type":"application/json","X-Mustermark-Token":token},body:JSON.stringify(payload)});
  const result=await response.json();
  if(!result.ok){setMessage(result.error?.message||"action rejected",true);await refresh();return null}
  await refresh();return result;
}
trackingToggle.addEventListener("click",async()=>{
  if(!currentState)return;
  await action({action:currentState.tracked?"tracking_stop":"tracking_start"});
});
function clearDropMarks(){document.querySelectorAll(".drop-before,.drop-after").forEach(node=>node.classList.remove("drop-before","drop-after"))}
function wireDocument(article,state){
  article.querySelectorAll("[data-mm-task=true]").forEach(item=>{Array.from(item.childNodes).forEach(child=>{if(child.nodeType!==Node.TEXT_NODE||!child.textContent.trim())return;const span=document.createElement("span");span.className="mm-task-text";span.textContent=child.textContent;child.replaceWith(span)})});
  article.querySelectorAll("[data-mm-labels]").forEach(item=>{const target=item.querySelector(":scope > p")||item;item.dataset.mmLabels.split(",").forEach(label=>{const badge=document.createElement("span");badge.className="mm-label";badge.textContent=label;target.append(badge)})});
  article.querySelectorAll("[data-mm-id][data-mm-kind=heading],[data-mm-id][data-mm-kind=item]").forEach(node=>{
    node.draggable=state.tracked;node.title=state.tracked?"Drag to move":"Start tracking to move structures";
    node.addEventListener("dragstart",event=>{dragged=node;node.classList.add("dragging");event.dataTransfer.effectAllowed="move";event.dataTransfer.setData("text/plain",node.dataset.mmRef)});
    node.addEventListener("dragend",()=>{node.classList.remove("dragging");dragged=null;clearDropMarks()});
    node.addEventListener("dragover",event=>{if(!dragged||dragged===node||dragged.dataset.mmKind!==node.dataset.mmKind||(node.dataset.mmKind==="heading"&&dragged.dataset.mmLevel!==node.dataset.mmLevel))return;event.preventDefault();clearDropMarks();const after=event.clientY>node.getBoundingClientRect().top+node.getBoundingClientRect().height/2;node.classList.add(after?"drop-after":"drop-before")});
    node.addEventListener("dragleave",()=>node.classList.remove("drop-before","drop-after"));
    node.addEventListener("drop",async event=>{event.preventDefault();if(!dragged||dragged===node||dragged.dataset.mmKind!==node.dataset.mmKind||(node.dataset.mmKind==="heading"&&dragged.dataset.mmLevel!==node.dataset.mmLevel))return;const after=event.clientY>node.getBoundingClientRect().top+node.getBoundingClientRect().height/2;const source=dragged.dataset.mmRef;const target=node.dataset.mmRef;clearDropMarks();const result=await action({action:after?"move_after":"move_before",node:source,target});if(result)setMessage("structure moved")});
  });
  article.querySelectorAll("[data-mm-task=true][data-mm-id] input[type=checkbox]").forEach(box=>{box.disabled=!state.tracked;box.addEventListener("change",async()=>{const item=box.closest("[data-mm-task=true]");const result=await action({action:"task_set",node:item.dataset.mmRef,checked:box.checked});if(result)setMessage(box.checked?"task checked":"task unchecked")})});
}
async function refresh(){
  try{
    const response=await fetch("/api/state",{cache:"no-store"});const state=await response.json();if(!state.ok)return;
    const article=document.getElementById("document");if(currentState&&currentState.revision!==state.revision){article.classList.add("changed");setTimeout(()=>article.classList.remove("changed"),220)}
    const documentChanged=!currentState||currentState.revision!==state.revision||currentState.tracked!==state.tracked||currentState.html!==state.html;
    currentState=state;if(documentChanged){article.innerHTML=state.html;wireDocument(article,state)}document.getElementById("path").textContent=state.path;document.getElementById("revision").textContent=state.revision;
    trackingToggle.textContent=state.tracked?"stop tracking":"start tracking";trackingToggle.classList.toggle("active",state.tracked);
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
