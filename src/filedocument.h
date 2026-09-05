#pragma once

#include <QByteArray>
#include <QString>

namespace Mustermark {

struct FileResult {
    bool ok = false;
    QByteArray source;
    QString error;
};

class FileDocument {
public:
    static FileResult read(const QString &path);
    static FileResult writeAtomic(const QString &path, const QByteArray &source,
                                  const QString &expectedRevision = {});
};

} // namespace Mustermark
