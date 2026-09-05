#include "markdownhighlighter.h"

#include <QQuickTextDocument>
#include <QRegularExpression>
#include <QTextCharFormat>
#include <QTextDocument>

#include <algorithm>

MarkdownHighlighter::MarkdownHighlighter(QObject *parent)
    : QSyntaxHighlighter(parent) {}

void MarkdownHighlighter::attach(QObject *quickTextDocument) {
    auto *document = qobject_cast<QQuickTextDocument *>(quickTextDocument);
    if (document)
        setDocument(document->textDocument());
}

void MarkdownHighlighter::setColors(const QColor &foreground, const QColor &muted,
                                    const QColor &accent) {
    m_foreground = foreground;
    m_muted = muted;
    m_accent = accent;
    rehighlight();
}

void MarkdownHighlighter::setStructuralRange(int startLine, int endLine) {
    if (m_structuralStart == startLine && m_structuralEnd == endLine)
        return;
    m_structuralStart = startLine;
    m_structuralEnd = endLine;
    rehighlight();
}

void MarkdownHighlighter::highlightBlock(const QString &text) {
    const int line = currentBlock().blockNumber() + 1;
    const bool structurallySelected = line >= m_structuralStart && line <= m_structuralEnd;
    QColor structuralColor = m_accent;
    structuralColor.setAlpha(32);

    auto selectedFormat = [&](QTextCharFormat format) {
        if (structurallySelected)
            format.setBackground(structuralColor);
        return format;
    };
    if (structurallySelected) {
        QTextCharFormat background;
        background.setBackground(structuralColor);
        setFormat(0, std::max(1, static_cast<int>(text.size())), background);
    }

    QTextCharFormat marker;
    marker.setForeground(m_muted);

    QTextCharFormat heading;
    heading.setForeground(m_foreground);
    heading.setFontWeight(QFont::DemiBold);

    static const QRegularExpression headingPattern(QStringLiteral(R"(^(\s*)(#{1,6})(\s+.*)$)"));
    const auto headingMatch = headingPattern.match(text);
    if (headingMatch.hasMatch()) {
        setFormat(0, headingMatch.capturedLength(), selectedFormat(heading));
        setFormat(headingMatch.capturedStart(2), headingMatch.capturedLength(2), selectedFormat(marker));
        return;
    }

    static const QRegularExpression listPattern(QStringLiteral(R"(^(\s*)((?:[-+*])|(?:\d+[.)]))(\s+))"));
    const auto listMatch = listPattern.match(text);
    if (listMatch.hasMatch())
        setFormat(listMatch.capturedStart(2), listMatch.capturedLength(2), selectedFormat(marker));

    QTextCharFormat task;
    task.setForeground(m_accent);
    task.setFontWeight(QFont::DemiBold);
    static const QRegularExpression taskPattern(QStringLiteral(R"(\[[ xX]\])"));
    auto taskIterator = taskPattern.globalMatch(text);
    while (taskIterator.hasNext()) {
        const auto match = taskIterator.next();
        setFormat(match.capturedStart(), match.capturedLength(), selectedFormat(task));
    }

    QTextCharFormat code;
    code.setForeground(m_accent);
    static const QRegularExpression codePattern(QStringLiteral(R"(`[^`]+`)"));
    auto codeIterator = codePattern.globalMatch(text);
    while (codeIterator.hasNext()) {
        const auto match = codeIterator.next();
        setFormat(match.capturedStart(), match.capturedLength(), selectedFormat(code));
    }

    QTextCharFormat metadata;
    metadata.setForeground(m_muted);
    metadata.setFontItalic(true);
    if (text.trimmed().startsWith(QStringLiteral("<!-- mustermark:")))
        setFormat(0, text.size(), selectedFormat(metadata));
}
