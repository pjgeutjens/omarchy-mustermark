#pragma once

#include "documentengine.h"

#include <QFileSystemWatcher>
#include <QObject>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

class DocumentController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString source READ source NOTIFY sourceChanged)
    Q_PROPERTY(QString renderedHtml READ renderedHtml NOTIFY documentChanged)
    Q_PROPERTY(QString filePath READ filePath NOTIFY filePathChanged)
    Q_PROPERTY(QString title READ title NOTIFY filePathChanged)
    Q_PROPERTY(QString revision READ revision NOTIFY documentChanged)
    Q_PROPERTY(QVariantList nodes READ nodes NOTIFY documentChanged)
    Q_PROPERTY(QVariantMap theme READ theme NOTIFY themeChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool modified READ modified NOTIFY modifiedChanged)
    Q_PROPERTY(bool tracked READ tracked NOTIFY documentChanged)
    Q_PROPERTY(bool conflict READ conflict NOTIFY conflictChanged)

public:
    explicit DocumentController(QObject *parent = nullptr);

    QString source() const;
    QString renderedHtml() const;
    QString filePath() const { return m_filePath; }
    QString title() const;
    QString revision() const { return m_document.revision; }
    QVariantList nodes() const;
    QVariantMap theme() const { return m_theme; }
    QString status() const { return m_status; }
    bool modified() const { return m_modified; }
    bool tracked() const { return m_document.tracked; }
    bool conflict() const { return m_conflict; }

    Q_INVOKABLE void newDocument();
    Q_INVOKABLE bool loadFile(const QUrl &url);
    Q_INVOKABLE bool save();
    Q_INVOKABLE bool saveAs(const QUrl &url);
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
    Q_INVOKABLE void checkExternalChange();

signals:
    void sourceChanged();
    void documentChanged();
    void filePathChanged();
    void themeChanged();
    void statusChanged();
    void modifiedChanged();
    void conflictChanged();

private:
    void setSource(const QByteArray &source, bool modified);
    void setStatus(const QString &status);
    void setConflict(bool conflict);
    void watchCurrentFile();
    void loadTheme();
    void writeRecovery() const;
    void clearRecovery() const;
    QString recoveryPath() const;

    Mustermark::DocumentEngine m_engine;
    Mustermark::Document m_document;
    QString m_filePath;
    QString m_diskRevision;
    QString m_status;
    bool m_modified = false;
    bool m_conflict = false;
    QVariantMap m_theme;
    QFileSystemWatcher m_watcher;
    QString m_themePath;
};
