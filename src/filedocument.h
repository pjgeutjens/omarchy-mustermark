#pragma once

#include <QByteArray>
#include <QString>

namespace Mustermark {
class DocumentSession;

struct FileResult {
    bool ok = false;
    QByteArray source;
    QString error;
};

class FileDocument {
public:
    static FileResult read(const QString &path, qint64 maxBytes = -1);
    static FileResult writeAtomic(const QString &path, const QByteArray &source,
                                  const QString &expectedRevision = {},
                                  DocumentSession *session = nullptr);
    static FileResult readLinked(const QString &path, DocumentSession &session,
                                 bool initialize = false);
    static FileResult writeRaw(const QString &path, const QByteArray &source,
                              const QString &expectedRevision = {});
};

} // namespace Mustermark
