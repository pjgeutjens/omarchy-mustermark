#pragma once

#include <QString>
#include <QJsonObject>

namespace Mustermark {

QString previewServerName(const QString &path = {});
bool activatePreview(const QString &path, int timeoutMs = 150);
bool sendPreviewRequest(const QJsonObject &request, int timeoutMs = 150);

} // namespace Mustermark
