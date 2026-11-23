#pragma once

#include <QString>
#include <QStringList>

class QPlainTextEdit;
class QSyntaxHighlighter;

// Language identification is shared by the status bar, highlighter and completion.
QString editorLanguage(const QString &path);
QStringList languageCompletions(const QString &path);
QSyntaxHighlighter *createLanguageHighlighter(QPlainTextEdit *editor, const QString &path);
