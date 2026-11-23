#include "languages.h"

#include <QApplication>
#include <QDebug>
#include <QPlainTextEdit>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTextLayout>

namespace {
QColor tokenColor(const QPlainTextEdit &editor, int line, const QString &word)
{
    const auto block = editor.document()->findBlockByNumber(line);
    const int index = block.text().indexOf(word);
    for (const auto &span : block.layout()->formats()) {
        if (index >= span.start && index < span.start + span.length)
            return span.format.foreground().color();
    }
    return {};
}

bool expect(bool condition, const char *message)
{
    if (!condition) qCritical().noquote() << message;
    return condition;
}

template<typename Check>
bool withLanguage(const QString &path, const QString &source, Check check)
{
    QPlainTextEdit editor;
    editor.setPlainText(source);
    auto *highlighter = createLanguageHighlighter(&editor, path);
    if (!highlighter) return expect(false, "Expected a highlighter for this language");
    highlighter->rehighlight();
    return check(editor);
}
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    if (!expect(editorLanguage("docker/Containerfile.dev") == "Dockerfile" &&
                editorLanguage("CMakeLists.txt") == "CMake" &&
                editorLanguage("view.tsx") == "TSX" &&
                editorLanguage("client.vue") == "Vue" &&
                editorLanguage("unknown.file") == "Plain text" &&
                languageCompletions("main.py").contains("with") &&
                !languageCompletions("main.py").contains("namespace") &&
                languageCompletions("Dockerfile").contains("COPY"),
                "Language and completion catalog lookup failed")) return 1;

    if (!withLanguage("example.rs", "/* first\n/* nested */\nlet stopped\n*/ let alive", [](const auto &view) {
        const auto comment = tokenColor(view, 0, "first");
        return expect(comment.isValid() && tokenColor(view, 2, "let") == comment &&
                      tokenColor(view, 3, "let").isValid() && tokenColor(view, 3, "let") != comment,
                      "Nested Rust comments did not retain multi-line state");
    })) return 1;

    if (!withLanguage("example.py", "def run():\n    \"\"\"doc\n    if text\n    \"\"\"; return 42", [](const auto &view) {
        const auto keyword = tokenColor(view, 0, "def");
        const auto string = tokenColor(view, 1, "doc");
        return expect(keyword.isValid() && string.isValid() && string != keyword &&
                      tokenColor(view, 2, "if") == string && tokenColor(view, 3, "return") == keyword,
                      "Python keywords or triple-quoted strings were not highlighted correctly");
    })) return 1;

    if (!withLanguage("Example.tsx", "return <section data-name=\"main\">text</section>", [](const auto &view) {
        const auto tag = tokenColor(view, 0, "section");
        const auto attr = tokenColor(view, 0, "data-name");
        return expect(tag.isValid() && attr.isValid() && attr != tag &&
                      tokenColor(view, 0, "return") == tag &&
                      !tokenColor(view, 0, "text").isValid(),
                      "TSX tag, attribute and code highlighting failed");
    })) return 1;

    if (!withLanguage("page.vue", "<!-- comment\n<div>not a tag</div>\n-->\n<template\n  name=\"main\">", [](const auto &view) {
        const auto comment = tokenColor(view, 0, "comment");
        const auto tag = tokenColor(view, 3, "template");
        const auto attr = tokenColor(view, 4, "name");
        return expect(comment.isValid() && tokenColor(view, 1, "div") == comment &&
                      tag.isValid() && attr.isValid() && attr != tag,
                      "Vue markup comments or multiline attributes were not highlighted correctly");
    })) return 1;

    if (!withLanguage("query.sql", "sElEcT 42; -- query", [](const auto &view) {
        const auto keyword = tokenColor(view, 0, "sElEcT");
        return expect(keyword.isValid() && keyword != tokenColor(view, 0, "42") &&
                      keyword != tokenColor(view, 0, "query"),
                      "SQL highlighting did not classify mixed-case keyword, number and comment");
    })) return 1;

    qInfo() << "PASS: language detection, keywords, multilanguage syntax and multiline lexer state";
    return 0;
}
