#include "previewipc.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalSocket>

namespace Mustermark {

QString previewServerName(const QString &path) {
    const QString overrideName = qEnvironmentVariable("MUSTERMARK_PREVIEW_SERVER_NAME");
    if (!overrideName.isEmpty())
        return QDir::temp().absoluteFilePath(overrideName);
    const QByteArray homeHash = QCryptographicHash::hash(
        (QDir::homePath() + QFileInfo(path).absoluteFilePath()).toUtf8(), QCryptographicHash::Sha256).toHex().left(12);
    return QDir::temp().absoluteFilePath(
        QStringLiteral("mustermark-preview-%1").arg(QString::fromLatin1(homeHash)));
}

bool sendPreviewRequest(const QJsonObject &value, int timeoutMs) {
    QLocalSocket socket;
    socket.connectToServer(previewServerName(value.value("path").toString()), QIODevice::WriteOnly);
    if (!socket.waitForConnected(timeoutMs))
        return false;
    const QByteArray request = QJsonDocument(value).toJson(QJsonDocument::Compact) + '\n';
    if (socket.write(request) != request.size())
        return false;
    socket.flush();
    return socket.bytesToWrite() == 0 || socket.waitForBytesWritten(timeoutMs);
}

bool activatePreview(const QString &path, int timeoutMs) {
    return sendPreviewRequest({
        {QStringLiteral("version"), 1},
        {QStringLiteral("type"), QStringLiteral("open")},
        {QStringLiteral("path"), path},
    }, timeoutMs);
}

} // namespace Mustermark
