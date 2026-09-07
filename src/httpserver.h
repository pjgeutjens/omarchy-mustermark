#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QObject>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QVector>

#include <memory>

class DocumentController;

namespace Mustermark {

class DocumentSession;

class DocumentHttpServer : public QObject {
public:
    explicit DocumentHttpServer(QString path, QString stylePath = {}, QObject *parent = nullptr);
    explicit DocumentHttpServer(DocumentController *controller, QString stylePath = {},
                                QObject *parent = nullptr);

    ~DocumentHttpServer() override;

    bool listen(quint16 port = 0);
    quint16 port() const;
    QString token() const { return m_token; }
    QString errorString() const { return m_errorString.isEmpty() ? m_server.errorString()
                                                                 : m_errorString; }

private:
    struct UndoEntry {
        QByteArray source;
        QString resultingRevision;
    };

    struct Request {
        QByteArray method;
        QByteArray target;
        QByteArray contentType;
        QByteArray token;
        QByteArray body;
    };

    void acceptConnection();
    void readRequest(QTcpSocket *socket);
    void handleRequest(QTcpSocket *socket, const Request &request);
    void sendResponse(QTcpSocket *socket, int status, const QByteArray &contentType,
                      const QByteArray &body);
    QJsonObject state();
    QJsonObject applyInstruction(const QJsonObject &instruction);
    void recordInstruction(const QJsonObject &instruction, const QJsonObject &result);
    void sendEventStream(QTcpSocket *socket);
    void broadcastChange();
    bool sendDocumentAsset(QTcpSocket *socket, const QString &requestPath);
    QByteArray styleSheet() const;
    QByteArray page() const;

    QString m_path;
    QString m_stylePath;
    QString m_token;
    QByteArray m_style;
    QString m_errorString;
    DocumentController *m_controller = nullptr;
    std::unique_ptr<DocumentSession> m_ownedSession;
    DocumentSession *m_session = nullptr;
    QTcpServer m_server;
    QSet<QTcpSocket *> m_eventClients;
    QJsonArray m_instructions;
    QVector<UndoEntry> m_undo;
    int m_sequence = 0;
};

int serveDocument(const QString &path, quint16 port, const QString &stylePath = {});

} // namespace Mustermark
