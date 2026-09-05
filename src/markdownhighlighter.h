#pragma once

#include <QColor>
#include <QSyntaxHighlighter>

class MarkdownHighlighter : public QSyntaxHighlighter {
    Q_OBJECT

public:
    explicit MarkdownHighlighter(QObject *parent = nullptr);

    Q_INVOKABLE void attach(QObject *quickTextDocument);
    Q_INVOKABLE void setColors(const QColor &foreground, const QColor &muted,
                               const QColor &accent);
    Q_INVOKABLE void setStructuralRange(int startLine, int endLine);

protected:
    void highlightBlock(const QString &text) override;

private:
    QColor m_foreground{QStringLiteral("#d4cab1")};
    QColor m_muted{QStringLiteral("#6b6465")};
    QColor m_accent{QStringLiteral("#b59caa")};
    int m_structuralStart = -1;
    int m_structuralEnd = -1;
};
