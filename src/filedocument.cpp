#include "filedocument.h"

#include "documentengine.h"

#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace Mustermark {

FileResult FileDocument::read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {false, {}, file.errorString()};
    return {true, file.readAll(), {}};
}

FileResult FileDocument::writeAtomic(const QString &path, const QByteArray &source,
                                     const QString &expectedRevision) {
    QFileInfo existing(path);
    const bool existed = existing.exists();
    QFileDevice::Permissions permissions{};
    if (existed) {
        const FileResult current = read(path);
        if (!current.ok)
            return current;
        if (!expectedRevision.isEmpty() &&
            DocumentEngine::revisionFor(current.source) != expectedRevision)
            return {false, {}, QStringLiteral("stale_revision")};
        permissions = existing.permissions();
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {false, {}, file.errorString()};
    if (file.write(source) != source.size()) {
        file.cancelWriting();
        return {false, {}, file.errorString()};
    }
    if (!file.commit())
        return {false, {}, file.errorString()};
    if (existed)
        QFile::setPermissions(path, permissions);
    return {true, source, {}};
}

} // namespace Mustermark
