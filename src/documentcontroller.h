#pragma once

#include "documentsession.h"

#include <QFileSystemWatcher>
#include <QObject>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

class DocumentController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString source READ source NOTIFY sourceChanged)
    Q_PROPERTY(QString renderedHtml READ renderedHtml NOTIFY documentChanged)
    Q_PROPERTY(QString previewPage READ previewPage NOTIFY documentChanged)
    Q_PROPERTY(QString filePath READ filePath NOTIFY filePathChanged)
    Q_PROPERTY(QString displayPath READ displayPath NOTIFY filePathChanged)
    Q_PROPERTY(QUrl documentBaseUrl READ documentBaseUrl NOTIFY filePathChanged)
    Q_PROPERTY(QString title READ title NOTIFY filePathChanged)
    Q_PROPERTY(QString revision READ revision NOTIFY documentChanged)
    Q_PROPERTY(QVariantList nodes READ nodes NOTIFY documentChanged)
    Q_PROPERTY(QVariantList recentFiles READ recentFiles NOTIFY recentFilesChanged)
    Q_PROPERTY(QVariantMap theme READ theme NOTIFY themeChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool modified READ modified NOTIFY modifiedChanged)
    Q_PROPERTY(bool tracked READ tracked NOTIFY documentChanged)
    Q_PROPERTY(bool conflict READ conflict NOTIFY conflictChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY undoAvailableChanged)
    Q_PROPERTY(bool recoveryAvailable READ recoveryAvailable NOTIFY recoveryAvailableChanged)

public:
    explicit DocumentController(QObject *parent = nullptr);

    QString source() const;
    QString renderedHtml() const;
    QString previewPage() const;
    QString filePath() const { return m_filePath; }
    QString displayPath() const;
    QUrl documentBaseUrl() const;
    QString title() const;
    QString revision() const { return m_session.document().revision; }
    QVariantList nodes() const;
    QVariantList recentFiles() const;
    QVariantMap theme() const { return m_theme; }
    QString status() const { return m_status; }
    bool modified() const { return m_modified; }
    bool tracked() const { return m_session.tracking(); }
    bool conflict() const { return m_conflict; }
    bool canUndo() const { return !m_apiUndo.isEmpty(); }
    bool recoveryAvailable() const { return m_recoveryAvailable; }

    Q_INVOKABLE void newDocument();
    Q_INVOKABLE bool loadFile(const QUrl &url);
    Q_INVOKABLE bool save();
    Q_INVOKABLE bool saveAs(const QUrl &url);
    Q_INVOKABLE void beginSourceEdit();
    Q_INVOKABLE void endSourceEdit();
    Q_INVOKABLE void updateSource(const QString &source);
    Q_INVOKABLE bool enableTracking();
    Q_INVOKABLE bool repairTracking();
    Q_INVOKABLE bool disableTracking();
    Q_INVOKABLE bool applyAction(const QString &action, const QString &node,
                                 const QString &target = {}, const QString &label = {},
                                 const QString &text = {});
    Q_INVOKABLE bool shiftLevel(const QString &action, const QString &node,
                                bool includeDescendants);
    Q_INVOKABLE bool setHeadingLevel(const QString &node, int level);
    Q_INVOKABLE bool pasteImage(const QString &node);
    Q_INVOKABLE bool attachImage(const QString &node, const QUrl &url);
    Q_INVOKABLE bool removeAttachment(const QString &node, const QString &path);
    Q_INVOKABLE bool launchPreview();
    Q_INVOKABLE void checkExternalChange();
    Q_INVOKABLE bool undoDocumentChange();
    Q_INVOKABLE bool reloadFromDisk();
    Q_INVOKABLE bool saveCopy(const QUrl &url);
    Q_INVOKABLE bool restoreRecovery();
    Q_INVOKABLE void discardRecovery();

    QJsonObject apiState() const;
    QJsonObject applyApiInstruction(const QJsonObject &instruction);
    QJsonObject attachApiImage(const QString &node, const QByteArray &data,
                               const QString &contentType, const QString &fileName,
                               const QString &baseRevision);

signals:
    void sourceChanged();
    void documentChanged();
    void recentFilesChanged();
    void filePathChanged();
    void themeChanged();
    void statusChanged();
    void modifiedChanged();
    void conflictChanged();
    void undoAvailableChanged();
    void recoveryAvailableChanged();

private:
    struct ApiUndoEntry {
        struct RemovedAttachment {
            QString path;
            QByteArray data;
        };
        QByteArray source;
        QString resultingRevision;
        bool wasModified = false;
        QStringList createdAttachments;
        QVector<RemovedAttachment> removedAttachments;
    };

    void setSource(const QByteArray &source, bool modified);
    void setStatus(const QString &status);
    void setConflict(bool conflict);
    void watchCurrentFile();
    void loadTheme();
    void loadRecentFiles();
    void recordRecentFile(const QString &path);
    void saveRecentFiles() const;
    void writeRecovery();
    void clearRecovery();
    void setRecoveryAvailable(bool available);
    QString recoveryPath() const;
    void notifySessionChanged(bool didChangeSource);
    bool attachImageData(const QString &node, const QByteArray &data,
                         const QString &extension, const QString &alt);
    QString attachmentRoot() const;
    QString attachmentUrl(const QString &relativePath) const;
    void pushApiUndo(const QByteArray &source, const QStringList &createdAttachments = {},
                     const QVector<ApiUndoEntry::RemovedAttachment> &removedAttachments = {});
    QJsonObject undoApiChange();

    Mustermark::DocumentSession m_session;
    QString m_filePath;
    QString m_diskRevision;
    QString m_status;
    bool m_modified = false;
    bool m_conflict = false;
    bool m_recoveryAvailable = false;
    QVariantMap m_theme;
    QFileSystemWatcher m_watcher;
    QString m_themePath;
    QStringList m_recentFiles;
    QVector<ApiUndoEntry> m_apiUndo;
    bool m_sourceEditActive = false;
    QByteArray m_sourceEditStart;
    bool m_sourceEditStartModified = false;
};
