#include "editor.h"
#include "catalog.h"
#include "fonts.h"
#include "languages.h"

#include <QApplication>
#include <QAbstractItemView>
#include <QCompleter>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QScrollBar>
#include <QStringListModel>
#include <QCryptographicHash>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRunnable>
#include <QSaveFile>
#include <QRegularExpression>
#include <QSet>
#include <QShortcut>
#include <QScopedValueRollback>
#include <QStandardPaths>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTimer>
#include <QThreadPool>
#include <QTabBar>
#include <QUuid>
#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
constexpr qint64 editLimit = 16 * 1024 * 1024;

QString resolvedPath(const QString &path) {
    QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}

QByteArray digest(const QByteArray &data) {
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

bool identifier(QChar ch) {
    return ch.isLetterOrNumber() || ch.isMark() || ch == '_';
}

QString leadingIndent(const QString &text) {
    int i = 0;
    while (i < text.size() && (text.at(i) == ' ' || text.at(i) == '\t')) ++i;
    return text.left(i);
}

QString codeBeforeComment(const QString &line, const QString &language) {
    static const QSet<QString> hash = {"python", "shell", "yaml", "toml", "ruby", "makefile",
        "dockerfile", "cmake", "perl", "r"};
    static const QSet<QString> other = {"plain text", "markdown", "html", "xml", "css", "json"};
    QString marker;
    if (hash.contains(language)) marker = "#";
    else if (language == "lua" || language == "sql") marker = "--";
    else if (language.contains("lisp") || language == "scheme" || language == "clojure") marker = ";";
    else if (!other.contains(language)) marker = "//";
    if (marker.isEmpty()) return line.trimmed();

    QChar quote;
    for (int i = 0; i < line.size(); ++i) {
        const auto ch = line.at(i);
        if (!quote.isNull()) {
            if (ch == '\\' && i + 1 < line.size()) ++i;
            else if (ch == quote) quote = QChar();
        } else if (ch == '\'' || ch == '"' || ch == '`') {
            quote = ch;
        } else if (line.midRef(i, marker.size()) == marker) {
            return line.left(i).trimmed();
        }
    }
    return line.trimmed();
}

int preferredWidth(const QString &language) {
    static const QSet<QString> twoSpaces = {"html", "xml", "css", "scss", "sass", "less", "javascript",
        "typescript", "javascript react", "typescript react", "json", "yaml", "ruby", "lua", "vue", "svelte"};
    return twoSpaces.contains(language) ? 2 : 4;
}

QString indentStep(const QTextBlock &current, const QString &indent, const QString &language) {
    if (language == "makefile" || indent.contains('\t')) return "\t";
    int smallest = 0;
    auto block = current;
    for (int i = 0; i < 32 && block.isValid(); ++i, block = block.previous()) {
        if (block.length() > 4096) continue;
        const auto text = block.text();
        const auto previous = leadingIndent(text);
        if (previous.size() == text.size() || previous.isEmpty()) continue;
        if (previous.startsWith('\t')) return indent.isEmpty() ? QString("\t") : QString(preferredWidth(language), ' ');
        if (previous.size() <= 8 && (smallest == 0 || previous.size() < smallest))
            smallest = previous.size();
    }
    return QString(smallest == 2 || smallest == 4 || smallest == 8 ? smallest : preferredWidth(language), ' ');
}

QString openingTag(const QString &code, const QString &language) {
    static const QSet<QString> markup = {"html", "xml", "javascript react", "typescript react", "vue", "svelte"};
    if (!markup.contains(language)) return {};
    static const QRegularExpression tag(R"(<([A-Za-z][\w.:\-]*)(?:\s[^<>]*)?>$)");
    const auto match = tag.match(code);
    if (!match.hasMatch() || code.endsWith("/>")) return {};
    const auto name = match.captured(1);
    static const QSet<QString> empty = {"area", "base", "br", "col", "embed", "hr", "img",
        "input", "link", "meta", "param", "source", "track", "wbr"};
    return language != "xml" && empty.contains(name.toLower()) ? QString() : name;
}

struct Snippet { const char *trigger; const char *description; const char *body; };
QList<Snippet> snippetsFor(const QString &language) {
    if (language == "html") return {
        {"html", "Document skeleton", "<!doctype html>\n<html lang=\"en\">\n<head>\n  <meta charset=\"utf-8\">\n  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n  <title>Page</title>\n</head>\n<body>\n  \v\n</body>\n</html>"},
        {"div", "Container element", "<div>\v</div>"},
        {"script", "Script element", "<script>\n  \v\n</script>"},
    };
    if (language == "python") return {
        {"def", "Function definition", "def function():\n    \vpass"},
        {"for", "For loop", "for item in items:\n    \vpass"},
        {"ifmain", "Module entry point", "if __name__ == \"__main__\":\n    \vmain()"},
    };
    if (language == "javascript" || language == "typescript" || language.endsWith(" react")) return {
        {"func", "Function definition", "function name() {\n  \v\n}"},
        {"forof", "For-of loop", "for (const item of items) {\n  \v\n}"},
        {"log", "Console output", "console.log(\v);"},
    };
    if (language == "c" || language == "c++") return {
        {"main", "Program entry point", "int main() {\n    \vreturn 0;\n}"},
        {"fori", "Index loop", "for (int i = 0; i < count; ++i) {\n    \v\n}"},
    };
    if (language == "rust") return {
        {"main", "Program entry point", "fn main() {\n    \v\n}"},
        {"println", "Formatted output", "println!(\"\v\");"},
    };
    return {};
}

bool underPath(const QString &candidate, const QString &path) {
    if (candidate.isEmpty() || path.isEmpty()) return false;
#if defined(Q_OS_WIN)
    constexpr auto sensitivity = Qt::CaseInsensitive;
#else
    constexpr auto sensitivity = Qt::CaseSensitive;
#endif
    return candidate.compare(path,sensitivity) == 0 ||
        candidate.startsWith(path.endsWith('/') ? path : path + '/', sensitivity);
}

class TextView;
class Gutter final : public QWidget {
public:
    explicit Gutter(TextView *owner);
    TextView *view;
    void paintEvent(QPaintEvent *event) override;
};

class TextView final : public QPlainTextEdit {
public:
    QSyntaxHighlighter *colorizer = nullptr;
    std::function<void(const LanguageSnapshot &,std::function<void(QList<SemanticCompletion>)>,bool)> requestSemantic;
    LanguageSnapshot snapshot() const {
        const QString path = languagePath();
        if (path.isEmpty() || document()->characterCount() > semanticDocumentLimit) return {};
        const auto cursor = textCursor();
        return {path,toPlainText(),document()->revision(),cursor.blockNumber(),cursor.positionInBlock()};
    }
    void diagnostics(int revision, QList<SemanticDiagnostic> items) {
        diagnosticRevision = revision;
        diagnosticItems = std::move(items);
        markLine();
    }
    explicit TextView(QWidget *parent) : QPlainTextEdit(parent), gutter(new Gutter(this)) {
        setFrameStyle(QFrame::NoFrame);
        setLineWrapMode(NoWrap);
        setCenterOnScroll(false);
        connect(this, &QPlainTextEdit::blockCountChanged, this, [this]{ updateMetrics(); });
        connect(this, &QPlainTextEdit::updateRequest, this, [this](QRect rect, int dy) {
            if (dy) gutter->scroll(0, dy);
            else gutter->update(0, rect.y(), gutter->width(), rect.height());
        });
        connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this] {
            markLine();
            if (textCursor().position() != suggestionPosition) dismissSuggestions();
        });
        suggestTimer.setSingleShot(true);
        suggestTimer.setInterval(130);
        connect(&suggestTimer, &QTimer::timeout, this, [this] {
            if (hasFocus()) completeWord(false);
        });
        updateMetrics();
        markLine();
    }
    void setLanguagePath(const QString &path) {
        const QString previous = document()->property("interactLanguagePath").toString();
        if (previous == path && colorizer) return;
        delete colorizer;
        document()->setProperty("interactLanguagePath",path);
        colorizer = createLanguageHighlighter(this,path);
        dismissSuggestions();
    }

    void completeWord(bool explicitRequest = true) {
        if (isReadOnly()) return;
        const auto cursor = textCursor();
        if (cursor.hasSelection() || cursor.block().length() > 16384) return;
        const QString line = cursor.block().text();
        int start = cursor.positionInBlock();
        while (start > 0 && identifier(line.at(start - 1))) --start;
        const QString prefix = line.mid(start, cursor.positionInBlock() - start);
        const QString before = line.left(cursor.positionInBlock());
        const bool member = before.endsWith('.') || before.endsWith("->") || before.endsWith("::");
        if (prefix.size() > 150 || (!explicitRequest && prefix.size() < 2 && !member)) return;
        if (!explicitRequest && member && !requestSemantic) return;

        QStringList quick;
        const QString path = languagePath();
        snippetBodies.clear();
        semanticItems.clear();
        for (const auto &snippet : explicitRequest || !prefix.isEmpty() ? snippetsFor(languageName()) : QList<Snippet>{}) {
            const QString trigger = QString::fromLatin1(snippet.trigger);
            if (!trigger.startsWith(prefix)) continue;
            const QString label = trigger + "  ·  " + tr(snippet.description);
            quick << label;
            snippetBodies.insert(label,QString::fromLatin1(snippet.body));
        }
        if (explicitRequest || !prefix.isEmpty()) {
            for (const auto &snippet : installedCatalogSnippets(editorLanguage(path))) {
                if (!snippet.trigger.startsWith(prefix)) continue;
                const QString label = snippet.trigger + "  ·  " + snippet.description;
                if (snippetBodies.contains(label)) continue;
                quick << label;
                snippetBodies.insert(label,snippet.body);
            }
        }
        QSet<QString> keywords;
        for (const auto &word : explicitRequest || !prefix.isEmpty() ? languageCompletions(path) : QStringList{}) {
            if (word.size() > prefix.size() && word.startsWith(prefix)) keywords.insert(word);
        }

        // Track nearby identifiers without scanning the full document per key.
        QSet<QString> local;
        const int first = qMax(0,cursor.blockNumber() - 60);
        const int last = qMin(blockCount()-1,cursor.blockNumber() + 60);
        int budget = 64 * 1024;
        for (auto block = document()->findBlockByNumber(first);
             !prefix.isEmpty() && block.isValid() && block.blockNumber() <= last && budget > 0;
             block = block.next()) {
            if (block.length() > 8192 || block.length() > budget) continue;
            const QString contents = block.text();
            budget -= contents.size();
            for (int pos = 0; pos < contents.size();) {
                if (!identifier(contents.at(pos))) { ++pos; continue; }
                int token = pos++;
                while (pos < contents.size() && identifier(contents.at(pos))) ++pos;
                if (block == cursor.block() && token == start) continue;
                const int size = pos - token;
                if (size > prefix.size() && size <= 150 && contents.midRef(token,prefix.size()) == prefix)
                    local.insert(contents.mid(token,size));
            }
        }
        local.subtract(keywords);
        auto sorted = [](const QSet<QString> &items) {
            QStringList result = items.values();
            result.sort();
            return result;
        };
        quick << sorted(local) << sorted(keywords);
        suggestionPosition = cursor.position();
        suggestionRevision = document()->revision();
        suggestionDocument = document();
        suggestionPrefix = prefix;
        if (quick.isEmpty()) {
            if (completer) completer->popup()->hide();
        } else {
            showSuggestions(quick);
        }

        if (!requestSemantic || path.isEmpty() || document()->characterCount() > semanticDocumentLimit) return;
        static const QSet<QString> supported{"C","C++","Rust"};
        if (!supported.contains(editorLanguage(path))) return;
        const auto stamp = ++suggestionTicket;
        const QPointer<TextView> view(this);
        requestSemantic(snapshot(),[view,stamp,localItems = std::move(quick)](QList<SemanticCompletion> items) {
            if (!view || stamp != view->suggestionTicket || !view->hasFocus() || items.isEmpty() ||
                view->suggestionDocument != view->document() ||
                view->suggestionRevision != view->document()->revision() ||
                view->suggestionPosition != view->textCursor().position()) return;
            if (!localItems.isEmpty() && (!view->completer || !view->completer->popup()->isVisible())) return;
            QStringList labels;
            for (auto &item : items) {
                if (labels.size() == 55) break;
                if (item.label.isEmpty() || item.insertText.isEmpty()) continue;
                QString label = item.label;
                if (!item.detail.isEmpty()) label += "  ·  " + item.detail.simplified().left(90);
                if (view->semanticItems.contains(label) || localItems.contains(label)) continue;
                labels << label;
                view->semanticItems.insert(label,std::move(item));
            }
            if (!labels.isEmpty()) view->showSuggestions(labels + localItems);
        },explicitRequest);
    }

    void showSuggestions(const QStringList &items) {
        if (!completer) {
            completer = new QCompleter(this);
            completions = new QStringListModel(completer);
            completer->setModel(completions);
            completer->setWidget(this);
            completer->setCaseSensitivity(Qt::CaseSensitive);
            completer->setCompletionMode(QCompleter::UnfilteredPopupCompletion);
            completer->setMaxVisibleItems(9);
            connect(completer,QOverload<const QString &>::of(&QCompleter::activated),this,
                    [this](const QString &choice) { acceptCompletion(choice); });
        }
        completions->setStringList(items.mid(0,80));
        completer->setCompletionPrefix({});
        completer->setCurrentRow(0);
        auto bounds = cursorRect();
        bounds.setWidth(qBound(180,completer->popup()->sizeHintForColumn(0) + 28,450));
        completer->complete(bounds);
        completer->popup()->setCurrentIndex(completer->currentIndex());
    }
    void updateMetrics() {
        const int digits = qMax(3, QString::number(blockCount()).size());
        leftMargin = fontMetrics().horizontalAdvance('9') * digits + 20;
        setViewportMargins(leftMargin, 0, 0, 0);
        setTabStopDistance(4 * fontMetrics().horizontalAdvance(' '));
        gutter->setGeometry(contentsRect().x(), contentsRect().y(), leftMargin, contentsRect().height());
    }
    void paintGutter(QPaintEvent *e) {
        QPainter painter(gutter);
        painter.fillRect(e->rect(), palette().base());
        auto block = firstVisibleBlock();
        qreal top = blockBoundingGeometry(block).translated(contentOffset()).top();
        const int active = textCursor().blockNumber();
        while (block.isValid() && top < e->rect().bottom() + 1) {
            const qreal height = blockBoundingRect(block).height();
            if (block.isVisible() && top + height >= e->rect().top()) {
                QColor color = palette().text().color();
                if (block.blockNumber() != active) color.setAlpha(115);
                painter.setPen(color);
                painter.drawText(0, qRound(top), leftMargin - 12, fontMetrics().height(),
                                 Qt::AlignRight | Qt::AlignVCenter, QString::number(block.blockNumber() + 1));
            }
            top += height;
            block = block.next();
        }
    }
private:
    Gutter *gutter;
    QCompleter *completer = nullptr;
    QStringListModel *completions = nullptr;
    QTimer suggestTimer;
    QHash<QString, QString> snippetBodies;
    QHash<QString, SemanticCompletion> semanticItems;
    QString suggestionPrefix;
    quint64 suggestionTicket = 0;
    int diagnosticRevision = -1;
    QList<SemanticDiagnostic> diagnosticItems;
    int suggestionPosition = -1;
    int suggestionRevision = -1;
    QPointer<QTextDocument> suggestionDocument;
    int leftMargin = 0;
    void markLine() {
        QTextEdit::ExtraSelection sel;
        sel.cursor = textCursor();
        sel.cursor.clearSelection();
        QColor tint = palette().text().color();
        tint.setAlpha(12);
        sel.format.setBackground(tint);
        sel.format.setProperty(QTextFormat::FullWidthSelection, true);
        if (diagnosticItems.isEmpty() || diagnosticRevision != document()->revision()) {
            setExtraSelections({sel});
        } else {
            QList<QTextEdit::ExtraSelection> selections{sel};
            for (const auto &item : diagnosticItems) {
                const auto start = document()->findBlockByNumber(item.startLine);
                const auto end = document()->findBlockByNumber(item.endLine);
                if (!start.isValid() || !end.isValid() || item.startColumn < 0 || item.endColumn < 0 ||
                    item.startColumn >= start.length() || item.endColumn >= end.length() ||
                    item.endLine < item.startLine || item.endLine > item.startLine + 8) continue;
                QTextEdit::ExtraSelection diagnostic;
                diagnostic.cursor = QTextCursor(start);
                const int from = start.position() + item.startColumn;
                const int to = end.position() + item.endColumn;
                diagnostic.cursor.setPosition(from);
                diagnostic.cursor.setPosition(qBound(from,qMax(from + 1,to),document()->characterCount()-1),
                                              QTextCursor::KeepAnchor);
                diagnostic.format.setUnderlineStyle(QTextCharFormat::WaveUnderline);
                diagnostic.format.setUnderlineColor(item.severity == 1 ? QColor("#de716e") : QColor("#d3a34f"));
                diagnostic.format.setToolTip(item.message);
                selections << diagnostic;
            }
            setExtraSelections(selections);
        }
        gutter->update();
    }
    void resizeEvent(QResizeEvent *e) override {
        QPlainTextEdit::resizeEvent(e);
        gutter->setGeometry(contentsRect().x(), contentsRect().y(), leftMargin, contentsRect().height());
    }
    void changeEvent(QEvent *e) override {
        QPlainTextEdit::changeEvent(e);
        if (e->type() == QEvent::FontChange && gutter) updateMetrics();
        if (e->type() == QEvent::PaletteChange && gutter) {
            markLine();
            if (colorizer) colorizer->rehighlight();
        }
    }
    QString languagePath() const {
        return document()->property("interactLanguagePath").toString();
    }

    QString languageName() const {
        const auto path = languagePath();
        const auto extension = QFileInfo(path).suffix().toLower();
        if (extension == "jsx") return "javascript react";
        if (extension == "tsx") return "typescript react";
        return editorLanguage(path).toLower();
    }

    void dismissSuggestions() {
        ++suggestionTicket;
        suggestTimer.stop();
        if (completer) completer->popup()->hide();
    }

    void acceptCompletion(const QString &choice) {
        auto cursor = textCursor();
        if (choice.isEmpty() || cursor.hasSelection() || cursor.position() != suggestionPosition ||
                suggestionDocument != document() || document()->revision() != suggestionRevision) return;
        const QString prefix = suggestionPrefix;
        const QString line = cursor.block().text();
        if (cursor.positionInBlock() < prefix.size() ||
                line.midRef(cursor.positionInBlock() - prefix.size(),prefix.size()) != prefix) return;
        int right = cursor.positionInBlock();
        while (right < line.size() && identifier(line.at(right))) ++right;
        int from = cursor.position() - prefix.size();
        int to = cursor.block().position() + right;
        QString text = snippetBodies.value(choice,choice);
        if (semanticItems.contains(choice)) {
            const auto &item = semanticItems[choice];
            text = item.insertText;
            if (item.startLine >= 0) {
                const auto start = document()->findBlockByNumber(item.startLine);
                const auto end = document()->findBlockByNumber(item.endLine);
                if (!start.isValid() || !end.isValid() || item.startColumn < 0 || item.endColumn < 0 ||
                    item.startColumn >= start.length() || item.endColumn >= end.length()) return;
                from = start.position() + item.startColumn;
                to = end.position() + item.endColumn;
                if (from > to) return;
            }
        } else {
            text.replace("\n","\n" + leadingIndent(line));
        }
        const int caret = text.indexOf(QChar('\v'));
        if (caret >= 0) text.remove(caret,1);
        cursor.beginEditBlock();
        cursor.setPosition(from);
        cursor.setPosition(to,QTextCursor::KeepAnchor);
        cursor.insertText(text);
        if (caret >= 0) cursor.setPosition(from + caret);
        cursor.endEditBlock();
        dismissSuggestions();
        setTextCursor(cursor);
    }

    void insertNewline() {
        auto cursor = textCursor();
        const QString line = cursor.block().text();
        const QString left = line.left(cursor.positionInBlock());
        const QString right = line.mid(cursor.positionInBlock());
        const QString indent = leadingIndent(left);
        const QString language = languageName();
        const auto code = codeBeforeComment(left,language);
        const auto trailing = right.trimmed();
        const QString tag = openingTag(code,language);
        const QChar token = code.isEmpty() ? QChar() : code.back();
        const bool structure = language != "markdown" && (language != "plain text" || token == '{');
        const QString openers = "{[(";
        const int pair = structure ? openers.indexOf(token) : -1;
        const bool bracket = pair >= 0;
        const bool nestedColon = (language == "python" || language == "yaml") && token == ':';
        const bool makeTarget = language == "makefile" && !indent.startsWith('\t') &&
            code.contains(':') && !code.contains('=') && !code.startsWith('#');
        static const QRegularExpression blockWord(R"(\b(?:then|do|function|else|begin|repeat)\s*$)");
        const bool wordBlock = QStringList{"shell","lua","ruby"}.contains(language) && code.contains(blockWord);
        const bool grow = !tag.isEmpty() || bracket || nestedColon || makeTarget || wordBlock;
        const bool hasClosing = !cursor.hasSelection() &&
            ((bracket && trailing.startsWith(QString("}])").at(pair))) ||
             (!tag.isEmpty() && trailing.startsWith("</" + tag + ">",
                language == "xml" ? Qt::CaseSensitive : Qt::CaseInsensitive)));

        cursor.beginEditBlock();
        cursor.insertText("\n" + indent + (grow ? indentStep(cursor.block(),indent,language) : QString()));
        if (hasClosing) {
            const int inner = cursor.position();
            cursor.setPosition(inner + leadingIndent(right).size(),QTextCursor::KeepAnchor);
            cursor.insertText("\n" + indent);
            cursor.setPosition(inner);
        }
        cursor.endEditBlock();
        setTextCursor(cursor);
    }

    void insertIndent() {
        auto cursor = textCursor();
        const auto language = languageName();
        const auto indentation = leadingIndent(cursor.block().text());
        const auto step = indentStep(cursor.block(),indentation,language);
        if (step == "\t") cursor.insertText(step);
        else cursor.insertText(QString(step.size() - cursor.positionInBlock() % step.size(),' '));
        setTextCursor(cursor);
    }

    void keyPressEvent(QKeyEvent *e) override {
        const int key = e->key();
        const bool simple = !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
        if (completer && completer->popup()->isVisible()) {
            switch (key) {
            case Qt::Key_Tab: case Qt::Key_Enter: case Qt::Key_Return: case Qt::Key_Escape:
                // QCompleter's popup receives these events before the editor.
                // Ignore forwarded events so the selected suggestion consumes them.
                e->ignore();
                return;
            default:
                dismissSuggestions();
            }
        }
        if ((key == Qt::Key_Return || key == Qt::Key_Enter) && simple &&
                !(e->modifiers() & Qt::ShiftModifier) && !isReadOnly() &&
                textCursor().block().length() <= 16384) {
            insertNewline();
            e->accept();
            return;
        }
        if (key == Qt::Key_Tab && simple && !textCursor().hasSelection() && !isReadOnly()) {
            insertIndent();
            e->accept();
            return;
        }
        if (key == Qt::Key_Escape) dismissSuggestions();
        QPlainTextEdit::keyPressEvent(e);
        const auto text = e->text();
        // Modifier-based edit commands and navigation never open the popup.
        if (simple && !isReadOnly() &&
                ((!text.isEmpty() && (identifier(text.back()) || text == "." || text == ">" || text == ":")) || key == Qt::Key_Backspace))
            suggestTimer.start();
        else suggestTimer.stop();
    }

};

Gutter::Gutter(TextView *owner) : QWidget(owner), view(owner) {}
void Gutter::paintEvent(QPaintEvent *e) { view->paintGutter(e); }
} // namespace

class EditorPage final : public QWidget {
public:
    explicit EditorPage(QWidget *parent) : QWidget(parent) {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        search = new QWidget(this);
        auto *row = new QHBoxLayout(search);
        row->setContentsMargins(8, 4, 8, 4);
        query = new QLineEdit(search);
        query->setPlaceholderText(tr("Find in document"));
        row->addWidget(query);
        auto button = [&](QString label, const std::function<void()> &action) {
            auto *b = new QToolButton(search);
            b->setText(label);
            row->addWidget(b);
            connect(b, &QToolButton::clicked, this, action);
        };
        button(tr("Previous"), [this]{ find(true); });
        button(tr("Next"), [this]{ find(false); });
        button(tr("Close"), [this]{ hideFind(); });
        search->hide();
        layout->addWidget(search);
        view = new TextView(this);
        layout->addWidget(view);
        setFocusProxy(view);
        view->setLanguagePath({});
        connect(query, &QLineEdit::returnPressed, this, [this]{ find(false); });
        auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), search);
        escape->setContext(Qt::WidgetWithChildrenShortcut);
        connect(escape, &QShortcut::activated, this, [this]{ hideFind(); });
    }

    void hideFind() { search->hide(); view->setFocus(); }
    void find(bool backwards) {
        if (query->text().isEmpty()) return;
        auto flags = backwards ? QTextDocument::FindBackward : QTextDocument::FindFlags{};
        const auto old = view->textCursor();
        auto cursor = view->document()->find(query->text(), old, flags);
        if (cursor.isNull()) {
            QTextCursor edge(view->document());
            edge.movePosition(backwards ? QTextCursor::End : QTextCursor::Start);
            cursor = view->document()->find(query->text(), edge, flags);
        }
        if (!cursor.isNull()) view->setTextCursor(cursor);
        query->setPlaceholderText(tr("Find in document"));
        QPalette colors = view->palette();
        if (cursor.isNull()) colors.setColor(QPalette::Text, QColor("#d36f66"));
        query->setPalette(colors);
    }
    QString text() const {
        QString result;
        result.reserve(view->document()->characterCount());
        for (auto block = view->document()->begin(); block.isValid(); block = block.next()) {
            if (block.blockNumber()) result += '\n';
            result += block.text();
        }
        return result;
    }

    TextView *view;
    QWidget *search;
    QLineEdit *query;
    QString path, untitled, journal;
    qint64 lastEdit = 0, dirtySince = 0;
    int journalRevision = -1, autosaveRevision = -1;
    bool blockAutomatic = false;
    QByteArray checksum;
    QString newline = "\n";
    bool bom = false;
    bool mixed = false;
};

namespace {
constexpr int draftFileLimit = 32;
constexpr qint64 draftTotalLimit = 64 * 1024 * 1024;
constexpr int draftHeaderLimit = 16 * 1024;
constexpr int draftDelayMs = 800;

struct Draft {
    QString path, title, newline, journal;
    QByteArray checksum, text;
    bool bom = false, mixed = false;
};

struct BackgroundSave {
    Draft draft;
    QByteArray bytes, writtenChecksum;
    QString error;
    bool journalWanted = false, saveWanted = false, saved = false, journalFailed = false;
    int revision = 0;
    std::mutex commitGuard;
    bool canceled = false;
    std::atomic_bool finished{false};
};

QString recoveryDirectory(const QString &workspace) {
    const QString home = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (home.isEmpty() || workspace.isEmpty()) return {};
    return home + "/recovery/" + QString::fromLatin1(digest(workspace.toUtf8()).toHex());
}

QByteArray encode(const Draft &draft) {
    QByteArray data = draft.text;
    if (draft.newline != "\n") data.replace("\n", draft.newline.toLatin1());
    if (draft.bom) data.prepend("\xef\xbb\xbf");
    return data;
}

bool diskChecksum(const QString &path, QByteArray &checksum) {
    QFileInfo metadata(path);
    if (!metadata.isFile() || metadata.isSymLink() || metadata.size() > editLimit) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray data = file.read(editLimit + 1);
    if (file.error() != QFileDevice::NoError || data.size() > editLimit) return false;
    checksum = digest(data);
    return true;
}

QByteArray draftHeader(const Draft &draft) {
    const QJsonObject metadata{{"version", 1}, {"path", draft.path}, {"title", draft.title},
        {"checksum", QString::fromLatin1(draft.checksum.toHex())}, {"newline", draft.newline},
        {"bom", draft.bom}, {"mixed", draft.mixed}};
    return QJsonDocument(metadata).toJson(QJsonDocument::Compact) + '\n';
}

bool readDraft(const QString &journal, Draft &draft, bool contents = false) {
    const QFileInfo info(journal);
    if (info.isSymLink() || !info.isFile()) return false;
    QFile file(journal);
    if (!file.open(QIODevice::ReadOnly) || file.size() > editLimit + draftHeaderLimit) return false;
    const auto header = file.readLine(draftHeaderLimit + 1);
    if (header.size() > draftHeaderLimit || !header.endsWith('\n')) return false;
    const auto object = QJsonDocument::fromJson(header).object();
    if (object["version"].toInt() != 1) return false;
    draft.path = object["path"].toString();
    draft.title = object["title"].toString();
    draft.newline = object["newline"].toString();
    draft.checksum = QByteArray::fromHex(object["checksum"].toString().toLatin1());
    draft.mixed = object["mixed"].toBool();
    draft.bom = object["bom"].toBool();
    draft.journal = journal;
    if ((!draft.path.isEmpty() && (!QDir::isAbsolutePath(draft.path) || draft.checksum.size() != 32)) ||
        (draft.newline != "\n" && draft.newline != "\r" && draft.newline != "\r\n")) return false;
    if (contents) {
        draft.text = file.read(editLimit + 1);
        if (file.error() != QFileDevice::NoError || draft.text.size() > editLimit ||
                QString::fromUtf8(draft.text).toUtf8() != draft.text) return false;
    }
    return true;
}

bool stageDraft(const Draft &draft, QSaveFile &out) {
    const QByteArray header = draftHeader(draft);
    if (header.size() > draftHeaderLimit) return false;
    QDir dir = QFileInfo(draft.journal).dir();
    if (!dir.mkpath(".")) return false;
    if (!QFileInfo::exists(draft.journal) &&
        dir.entryList({"*.draft"}, QDir::Files | QDir::NoSymLinks).size() >= draftFileLimit) return false;
    qint64 total = draft.text.size() + header.size();
    const auto existing = dir.entryInfoList({"*.draft"}, QDir::Files | QDir::NoSymLinks);
    for (const auto &entry : existing)
        if (entry.absoluteFilePath() != draft.journal) total += entry.size();
    if (total > draftTotalLimit || !out.open(QIODevice::WriteOnly)) return false;
#ifdef Q_OS_UNIX
    if (!out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) return false;
#endif
    return out.write(header) == header.size() && out.write(draft.text) == draft.text.size();
}

void writeBackground(const std::shared_ptr<BackgroundSave> &job) {
    if (job->journalWanted) {
        QSaveFile journal(job->draft.journal);
        if (!stageDraft(job->draft, journal)) {
            job->journalFailed = true;
        } else {
            std::lock_guard<std::mutex> lock(job->commitGuard);
            if (!job->canceled) job->journalFailed = !journal.commit();
        }
    }
    if (job->saveWanted) {
        const auto path = job->draft.path;
        job->bytes = encode(job->draft);
        // Reject deleted files before preparing a replacement. Check again after
        // staging the write, so disk changes during preparation are detected.
        QByteArray first;
        if (job->bytes.size() > editLimit) {
            job->error = Editor::tr("File exceeds the auto-save limit");
        } else if (!diskChecksum(path, first) || first != job->draft.checksum) {
            job->error = Editor::tr("File changed on disk");
        } else {
            QSaveFile disk(path);
            if (!disk.open(QIODevice::WriteOnly) || disk.write(job->bytes) != job->bytes.size()) {
                job->error = disk.errorString();
            } else {
                QByteArray second;
                if (!diskChecksum(path, second) || second != job->draft.checksum) {
                    job->error = Editor::tr("File changed on disk");
                } else {
                    std::lock_guard<std::mutex> lock(job->commitGuard);
                    if (!job->canceled) {
                        job->saved = disk.commit();
                        if (job->saved) job->writtenChecksum = digest(job->bytes);
                        else job->error = disk.errorString();
                    }
                }
            }
        }
    }
    job->finished.store(true, std::memory_order_release);
}
} // namespace

struct Editor::BackgroundState {
    BackgroundState() { clock.start(); }
    QElapsedTimer clock;
    QTimer timer, bufferTimer;
    QString recovery;
    std::shared_ptr<BackgroundSave> job;
    QPointer<EditorPage> target;
    bool enabled = false, suspended = false;
    int delay = 1500;
};

Editor::Editor(QWidget *parent) : QTabWidget(parent), background(std::make_unique<BackgroundState>()),
                                  editorFont(defaultEditorFont()) {
    background->timer.setSingleShot(true);
    background->bufferTimer.setSingleShot(true);
    background->bufferTimer.setInterval(350);
    connect(&background->timer, &QTimer::timeout, this, [this] { runBackground(); });
    connect(&background->bufferTimer, &QTimer::timeout, this, [this] {
        if (!onBufferChanged) return;
        const auto snapshot = currentSnapshot();
        if (!snapshot.path.isEmpty()) onBufferChanged(snapshot);
    });

    setDocumentMode(true);
    setTabsClosable(true);
    setMovable(true);
    setElideMode(Qt::ElideMiddle);
    connect(this, &QTabWidget::tabCloseRequested, this, [this](int i){ closePage(i); });
    connect(this, &QTabWidget::currentChanged, this, [this]{ if (onStateChanged) onStateChanged(); });
}

Editor::~Editor() {
    background->timer.stop();
    if (const auto job = background->job) {
        std::lock_guard<std::mutex> lock(job->commitGuard);
        job->canceled = true;
    }
}

EditorPage *Editor::page(int index) const { return static_cast<EditorPage *>(widget(index)); }
QPlainTextEdit *Editor::currentText() const { auto *tab = page(currentIndex()); return tab ? tab->view : nullptr; }
QString Editor::currentPath() const { auto *tab = page(currentIndex()); return tab ? tab->path : QString{}; }

QStringList Editor::openPaths() const {
    QStringList paths;
    for (int i = 0; i < count(); ++i) {
        const QString &path = page(i)->path;
        if (!path.isEmpty()) paths << path;
    }
    return paths;
}

EditorPage *Editor::addPage() {
    auto *tab = new EditorPage(this);
    tab->view->setFont(editorFont);
    tab->view->requestSemantic = [this](const LanguageSnapshot &snapshot, auto done, bool explicitRequest) {
        if (onCompletionRequested) onCompletionRequested(snapshot,std::move(done),explicitRequest);
    };
    tab->untitled = tr("Untitled %1").arg(++untitledNumber);
    setCurrentIndex(addTab(tab, tab->untitled));
    auto *document = tab->view->document();
    connect(document, &QTextDocument::modificationChanged, this, [this, tab, document](bool modified) {
        if (modified) {
            tab->dirtySince = background->clock.elapsed();
            scheduleBackground();
        } else {
            cancelBackground(tab);
            if (!document->isModified()) removeDraft(tab);
        }
        refresh(tab);
    });
    connect(document, &QTextDocument::contentsChange, this, [this, tab](int, int removed, int added) {
        if (!removed && !added) return;
        tab->lastEdit = background->clock.elapsed();
        scheduleBackground();
        if (currentWidget() == tab && onBufferChanged) background->bufferTimer.start();
    });
    connect(tab->view, &QPlainTextEdit::cursorPositionChanged, this, [this,tab]{
        if (currentWidget() == tab && onStateChanged) onStateChanged();
    });
    tab->view->setFocus();
    return tab;
}

void Editor::newFile() { addPage(); }

void Editor::refresh(EditorPage *tab) {
    const auto label = tab->path.isEmpty() ? tab->untitled : QFileInfo(tab->path).fileName();
    const int index = indexOf(tab);
    if (index < 0) return;
    setTabText(index, label + (tab->view->document()->isModified() ? " •" : ""));
    setTabToolTip(index, tab->path);
    if (onStateChanged) onStateChanged();
}

bool Editor::openFile(QString path, bool reportError) {
    path = resolvedPath(path);
    for (int i = 0; i < count(); ++i) {
        if (page(i)->path == path) { setCurrentIndex(i); page(i)->view->setFocus(); return true; }
    }
    QFile file(path);
    auto error = [&](const QString &why) {
        if (reportError)
            QMessageBox::warning(this, tr("Open file"), QString("%1\n%2").arg(path, why));
        return false;
    };
    if (!QFileInfo(path).isFile()) return error(tr("Select a regular file."));
    if (!file.open(QIODevice::ReadOnly)) return error(file.errorString());
    if (file.size() > editLimit) return error(tr("Interactive editing is limited to 16 MiB per file."));
    QByteArray data = file.read(editLimit + 1);
    if (file.error() != QFileDevice::NoError) return error(file.errorString());
    if (data.size() > editLimit) return error(tr("Interactive editing is limited to 16 MiB per file."));
    QByteArray originalDigest = digest(data);
    const bool bom = data.startsWith("\xef\xbb\xbf");
    if (bom) data.remove(0, 3);
    const QString decoded = QString::fromUtf8(data.constData(), data.size());
    if (data.contains('\0') || decoded.toUtf8() != data)
        return error(tr("This editor opens UTF-8 text. Binary files and other encodings require conversion."));
    if (decoded.contains(QChar::ParagraphSeparator) || decoded.contains(QChar::LineSeparator))
        return error(tr("This file uses Unicode paragraph separators. Convert it to LF or CRLF before editing."));
    const int pair = decoded.count("\r\n");
    const int lf = decoded.count('\n') - pair, cr = decoded.count('\r') - pair;
    const bool mixed = (pair > 0) + (lf > 0) + (cr > 0) > 1;
    QString normalized = decoded;
    normalized.replace("\r\n","\n").replace('\r','\n');
    auto *tab = addPage();
    tab->path = path;
    tab->bom = bom;
    tab->mixed = mixed;
    tab->checksum = originalDigest;
    tab->newline = pair && pair >= lf && pair >= cr ? "\r\n" : cr > lf ? "\r" : "\n";
    tab->view->setLanguagePath(path);
    tab->view->setPlainText(normalized);
    tab->view->document()->setModified(false);
    refresh(tab);
    return true;
}

bool Editor::writePage(EditorPage *tab, bool chooseName) {
    if (!tab) return false;
    cancelBackground(tab);
    const QScopedValueRollback<bool> suspended(background->suspended, true);
    QString path = tab->path;
    if (!chooseName && !path.isEmpty() && !tab->view->document()->isModified()) return true;
    if (chooseName || path.isEmpty()) {
        path = QFileDialog::getSaveFileName(this, tr("Save file"), path);
        if (path.isEmpty()) return false;
        path = resolvedPath(path);
    }
    auto error = [&](const QString &why) {
        QMessageBox::warning(this, tr("Save file"), QString("%1\n%2").arg(path, why));
        return false;
    };
    for (int i = 0; i < count(); ++i) {
        if (page(i) != tab && page(i)->path == path) return error(tr("This file is open in another tab."));
    }
    if (path == tab->path) {
        QFile disk(path);
        bool same = false;
        if (disk.exists()) {
            if (!disk.open(QIODevice::ReadOnly)) return error(disk.errorString());
            QCryptographicHash hash(QCryptographicHash::Sha256);
            if (!hash.addData(&disk)) return error(disk.errorString());
            same = hash.result() == tab->checksum;
        }
        if (!same && QMessageBox::warning(this, tr("File changed on disk"),
                tr("Replace the current disk version of %1 with your edits?").arg(path),
                QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Save) return false;
    }
    if (tab->mixed && QMessageBox::question(this, tr("Mixed line endings"),
            tr("Saving %1 will use one line ending style throughout. Continue?").arg(path),
            QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Save) return false;
    QString text = tab->text();
    if (tab->newline != "\n") text.replace("\n", tab->newline);
    QByteArray bytes = text.toUtf8();
    if (tab->bom) bytes.prepend("\xef\xbb\xbf");
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit()) return error(out.errorString());
    const QString previousPath = tab->path;
    tab->path = path;
    if (!previousPath.isEmpty() && previousPath != path && onBufferClosed) onBufferClosed(previousPath);
    tab->blockAutomatic = false;
    tab->checksum = digest(bytes);
    tab->mixed = false;
    tab->view->setLanguagePath(path);
    tab->view->document()->setModified(false);
    removeDraft(tab);
    refresh(tab);
    return true;
}
bool Editor::save() { return writePage(page(currentIndex()), false); }
bool Editor::saveAs() { return writePage(page(currentIndex()), true); }

bool Editor::mayClose(EditorPage *tab) {
    if (!tab->view->document()->isModified()) return true;
    cancelBackground(tab);
    if (!tab->view->document()->isModified()) return true;
    const QScopedValueRollback<bool> suspended(background->suspended, true);
    setCurrentWidget(tab);
    const auto answer = QMessageBox::warning(this, tr("Unsaved changes"),
        tr("Save changes to %1?").arg(tab->path.isEmpty() ? tab->untitled : tab->path),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    return answer == QMessageBox::Discard || (answer == QMessageBox::Save && writePage(tab, false));
}

bool Editor::confirmCloseAll() {
    auto *previous = currentWidget();
    for (int i = 0; i < count(); ++i) if (!mayClose(page(i))) return false;
    if (previous) setCurrentWidget(previous);
    for (int i = 0; i < count(); ++i) {
        cancelBackground(page(i));
        removeDraft(page(i));
    }
    return true;
}

void Editor::closePage(int index) {
    auto *tab = page(index);
    if (!tab || !mayClose(tab)) return;
    cancelBackground(tab);
    removeDraft(tab);
    if (!tab->path.isEmpty() && onBufferClosed) onBufferClosed(tab->path);
    removeTab(index);
    tab->deleteLater();
}
void Editor::closeCurrent() { closePage(currentIndex()); }

bool Editor::canChangePath(const QString &path) const {
    const QString affected = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    for (int i = 0; i < count(); ++i) {
        auto *tab = page(i);
        if (tab->view->document()->isModified() && underPath(tab->path,affected)) return false;
    }
    return true;
}

void Editor::pathRenamed(const QString &oldPath, const QString &newPath) {
    const QString before = QDir::cleanPath(QFileInfo(oldPath).absoluteFilePath());
    const QString after = QDir::cleanPath(QFileInfo(newPath).absoluteFilePath());
    for (int i = 0; i < count(); ++i) {
        auto *tab = page(i);
        if (!underPath(tab->path,before)) continue;
        cancelBackground(tab);
        if (onBufferClosed) onBufferClosed(tab->path);
        tab->path = resolvedPath(after + tab->path.mid(before.size()));
        tab->view->setLanguagePath(tab->path);
        refresh(tab);
    }
}

void Editor::pathRemoved(const QString &path) {
    const QString affected = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    for (int i = count() - 1; i >= 0; --i) {
        auto *tab = page(i);
        if (underPath(tab->path,affected) && !tab->view->document()->isModified()) closePage(i);
    }
}

void Editor::findText() {
    auto *tab = page(currentIndex());
    if (!tab) return;
    QString selected = tab->view->textCursor().selectedText();
    if (!selected.isEmpty() && !selected.contains(QChar::ParagraphSeparator)) tab->query->setText(selected);
    tab->search->show();
    tab->query->setFocus();
    tab->query->selectAll();
}

QPlainTextEdit *Editor::createSplitView(QWidget *parent) {
    auto *view = new TextView(parent);
    view->setFont(editorFont);
    view->requestSemantic = [this](const LanguageSnapshot &snapshot, auto done, bool explicitRequest) {
        if (onCompletionRequested) onCompletionRequested(snapshot,std::move(done),explicitRequest);
    };
    return view;
}

void Editor::completeWord() {
    auto *tab = page(currentIndex());
    if (!tab) return;
    auto *focused = dynamic_cast<TextView *>(QApplication::focusWidget());
    auto *view = focused && focused->document() == tab->view->document() ? focused : tab->view;
    view->completeWord();
}

void Editor::goToLine() {
    auto *view = currentText();
    if (!view) return;
    bool ok = false;
    int line = QInputDialog::getInt(this,tr("Go to line"),tr("Line"),view->textCursor().blockNumber() + 1,1,view->blockCount(),1,&ok);
    if (ok) {
        view->setTextCursor(QTextCursor(view->document()->findBlockByNumber(line - 1)));
        view->centerCursor();
        view->setFocus();
    }
}
void Editor::setEditorFont(const QFont &font) {
    editorFont = font;
    for (int i = 0; i < count(); ++i) page(i)->view->setFont(font);
}

void Editor::setAutosaveEnabled(bool enabled, int delayMs) {
    background->enabled = enabled;
    background->delay = qBound(200,delayMs,30000);
    for (int i = 0; i < count(); ++i) page(i)->autosaveRevision = -1;
    if (!enabled && background->target) cancelBackground(background->target);
    scheduleBackground();
}

void Editor::setRecoveryWorkspace(const QString &path) {
    if (path.isEmpty() || !QFileInfo(path).isDir()) return;
    const QString location = recoveryDirectory(resolvedPath(path));
    if (background->recovery == location) return;
    background->recovery = location;
    scheduleBackground();
}

void Editor::scheduleBackground() {
    if (background->job) {
        background->timer.start(40);
        return;
    }
    if (background->suspended) return;
    if (!background->enabled && background->recovery.isEmpty()) return;
    const qint64 now = background->clock.elapsed();
    qint64 earliest = std::numeric_limits<qint64>::max();
    for (int i = 0; i < count(); ++i) {
        const auto *tab = page(i);
        const auto *doc = tab->view->document();
        if (!doc->isModified() || doc->characterCount() > editLimit) continue;
        const int revision = doc->revision();
        if (!background->recovery.isEmpty() && tab->journalRevision != revision)
            earliest = qMin(earliest,qMin(tab->lastEdit + draftDelayMs,tab->dirtySince + 5000));
        if (background->enabled && !tab->path.isEmpty() && !tab->mixed && !tab->blockAutomatic &&
            tab->autosaveRevision != revision)
            earliest = qMin(earliest,tab->lastEdit + background->delay);
    }
    if (earliest != std::numeric_limits<qint64>::max())
        background->timer.start(static_cast<int>(qBound<qint64>(0,earliest - now,30000)));
    else
        background->timer.stop();
}

void Editor::removeDraft(EditorPage *tab) {
    if (!tab->journal.isEmpty()) QFile::remove(tab->journal);
    tab->journal.clear();
    tab->journalRevision = -1;
}

void Editor::cancelBackground(EditorPage *tab) {
    if (background->target != tab || !background->job) return;
    const auto job = background->job;
    QByteArray savedChecksum;
    {
        std::lock_guard<std::mutex> lock(job->commitGuard);
        job->canceled = true;
        if (job->saved) savedChecksum = job->writtenChecksum;
        job->saved = false;
    }
    if (job->journalWanted && savedChecksum.isEmpty()) tab->journalRevision = -1;
    if (!savedChecksum.isEmpty() && tab->path == job->draft.path) {
        tab->checksum = savedChecksum;
        auto *document = tab->view->document();
        document->setModified(document->revision() != job->revision);
    }
}

void Editor::runBackground() {
    if (const auto job = background->job) {
        if (!job->finished.load(std::memory_order_acquire)) {
            background->timer.start(40);
            return;
        }
        const auto tab = background->target;
        background->job.reset();
        background->target.clear();
        if (tab && indexOf(tab) >= 0 && !job->canceled) {
            if (job->saved && tab->path == job->draft.path) {
                tab->checksum = job->writtenChecksum;
                if (tab->view->document()->revision() == job->revision) {
                    tab->view->document()->setModified(false);
                    if (onAutosaveStatus) onAutosaveStatus(tr("Auto-saved %1").arg(QFileInfo(tab->path).fileName()));
                }
            } else if (!job->error.isEmpty()) {
                if (onAutosaveStatus)
                    onAutosaveStatus(tr("Auto-save paused for %1: %2").arg(QFileInfo(tab->path).fileName(),job->error));
                tab->blockAutomatic = true;
            } else if (job->journalFailed && onAutosaveStatus) {
                onAutosaveStatus(tr("Recovery copy could not be written"));
            }
        }
    }
    if (background->suspended) {
        // Don't run a scheduled save inside a Save As or Close confirmation.
        background->timer.start(150);
        return;
    }
    const qint64 now = background->clock.elapsed();
    for (int i = 0; i < count(); ++i) {
        auto *tab = page(i);
        const auto *doc = tab->view->document();
        if (!doc->isModified() || doc->characterCount() > editLimit) continue;
        const int revision = doc->revision();
        const bool save = background->enabled && !tab->path.isEmpty() && !tab->mixed &&
            !tab->blockAutomatic && tab->autosaveRevision != revision &&
            now >= tab->lastEdit + background->delay;
        const bool journal = !background->recovery.isEmpty() && tab->journalRevision != revision &&
            (save || now >= qMin(tab->lastEdit + draftDelayMs,tab->dirtySince + 5000));
        if (!save && !journal) continue;
        if (save) tab->autosaveRevision = revision;
        if (journal) {
            tab->journalRevision = revision;
            tab->dirtySince = now;
            if (tab->journal.isEmpty())
                tab->journal = QDir(background->recovery).filePath(QUuid::createUuid().toString(QUuid::Id128) + ".draft");
        }
        auto job = std::make_shared<BackgroundSave>();
        job->draft = {tab->path, tab->untitled, tab->newline, tab->journal,
                      tab->checksum, tab->text().toUtf8(), tab->bom, tab->mixed};
        if (job->draft.text.size() > editLimit) {
            if (onAutosaveStatus) onAutosaveStatus(tr("File exceeds the recovery and auto-save limit"));
            continue;
        }
        job->revision = revision;
        job->saveWanted = save;
        job->journalWanted = journal;
        background->job = job;
        background->target = tab;
        QThreadPool::globalInstance()->start(QRunnable::create([job] { writeBackground(job); }));
        break;
    }
    scheduleBackground();
}

bool Editor::hasRecoverableDrafts() const {
    if (background->recovery.isEmpty()) return false;
    const auto drafts = QDir(background->recovery).entryInfoList({"*.draft"}, QDir::Files | QDir::NoSymLinks);
    for (const auto &file : drafts) {
        Draft metadata;
        if (readDraft(file.absoluteFilePath(),metadata)) return true;
    }
    return false;
}

void Editor::showRecovery() {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Restore Drafts"));
    dialog.resize(500, 300);
    auto *layout = new QVBoxLayout(&dialog);
    auto *note = new QLabel(tr("Restore opens a modified editor. Press Save to write it to disk."),&dialog);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto *list = new QListWidget(&dialog);
    layout->addWidget(list);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel,&dialog);
    auto *restore = buttons->addButton(tr("Restore"),QDialogButtonBox::AcceptRole);
    auto *discard = buttons->addButton(tr("Discard draft"),QDialogButtonBox::DestructiveRole);
    layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    connect(list,&QListWidget::itemActivated,&dialog,&QDialog::accept);
    connect(discard,&QPushButton::clicked,&dialog,[&] {
        auto *item = list->currentItem();
        if (!item || QMessageBox::question(&dialog,tr("Discard draft"),tr("Remove this recovery copy?"),
                                          QMessageBox::Discard | QMessageBox::Cancel) != QMessageBox::Discard) return;
        if (QFile::remove(item->data(Qt::UserRole).toString())) delete list->takeItem(list->currentRow());
        else QMessageBox::warning(&dialog,tr("Restore Drafts"),tr("The recovery copy could not be removed."));
    });
    connect(list,&QListWidget::currentItemChanged,&dialog,[&](QListWidgetItem *item) {
        restore->setEnabled(item);
        discard->setEnabled(item);
    });

    const auto drafts = background->recovery.isEmpty() ? QFileInfoList{} :
        QDir(background->recovery).entryInfoList({"*.draft"}, QDir::Files | QDir::NoSymLinks, QDir::Time);
    QSet<QString> currentJournals;
    for (int i = 0; i < count(); ++i) currentJournals.insert(page(i)->journal);
    for (const auto &file : drafts.mid(0,draftFileLimit)) {
        if (currentJournals.contains(file.absoluteFilePath())) continue;
        Draft draft;
        if (!readDraft(file.absoluteFilePath(),draft)) continue;
        const QString title = draft.path.isEmpty() ? draft.title : draft.path;
        auto *item = new QListWidgetItem(tr("%1 · %2").arg(title,
            file.lastModified().toLocalTime().toString("yyyy-MM-dd hh:mm:ss")),list);
        item->setData(Qt::UserRole,draft.journal);
    }
    restore->setEnabled(false);
    discard->setEnabled(false);
    if (list->count()) list->setCurrentRow(0);
    else note->setText(tr("There are no drafts from another session."));
    if (dialog.exec() != QDialog::Accepted || !list->currentItem()) return;

    Draft draft;
    if (!readDraft(list->currentItem()->data(Qt::UserRole).toString(),draft,true)) {
        QMessageBox::warning(this,tr("Restore Drafts"),tr("Could not read this recovery copy."));
        return;
    }
    EditorPage *tab = nullptr;
    QByteArray checksum;
    if (!draft.path.isEmpty() && canChangePath(draft.path) && diskChecksum(draft.path,checksum) &&
        checksum == draft.checksum && openFile(draft.path,false)) {
        tab = page(currentIndex());
    } else {
        tab = addPage();
        tab->untitled = tr("Recovered %1").arg(draft.path.isEmpty() ? draft.title : QFileInfo(draft.path).fileName());
        tab->view->setLanguagePath(draft.path);
    }

    // Preserve the journal until the user explicitly saves or discards the buffer.
    tab->blockAutomatic = true;
    tab->newline = draft.newline;
    tab->bom = draft.bom;
    tab->mixed = draft.mixed;
    tab->view->setPlainText(QString::fromUtf8(draft.text));
    tab->journal = draft.journal;
    tab->view->document()->setModified(true);
    tab->journalRevision = tab->view->document()->revision();
    refresh(tab);
}

bool Editor::canSwitchProjectFiles() const {
    for (int i = 0; i < count(); ++i)
        if (page(i)->view->document()->isModified()) return false;
    return true;
}

void Editor::reloadWorkspaceFiles() {
    auto *selected = currentWidget();
    const auto paths = openPaths();
    QString selectedPath = currentPath();
    for (const auto &path : paths) {
        for (int i = 0; i < count(); ++i) {
            auto *tab = page(i);
            if (tab->path != path || tab->view->document()->isModified()) continue;
            QByteArray checksum;
            if (diskChecksum(path,checksum) && checksum == tab->checksum) break;
            const int position = tab->view->textCursor().position();
            if (selected == tab) selected = nullptr;
            closePage(i);
            if (openFile(path,false)) {
                auto *reopened = currentText();
                auto cursor = reopened->textCursor();
                cursor.setPosition(qMin(position,reopened->document()->characterCount() - 1));
                reopened->setTextCursor(cursor);
                tabBar()->moveTab(currentIndex(),i);
            }
            break;
        }
    }
    if (selected) setCurrentWidget(selected);
    else if (!selectedPath.isEmpty()) {
        for (int i = 0; i < count(); ++i)
            if (page(i)->path == selectedPath) setCurrentIndex(i);
    }
}

LanguageSnapshot Editor::currentSnapshot() const {
    auto *tab = page(currentIndex());
    if (!tab) return {};
    auto *focused = dynamic_cast<TextView *>(QApplication::focusWidget());
    auto *view = focused && focused->document() == tab->view->document() ? focused : tab->view;
    return view->snapshot();
}

void Editor::setDiagnostics(QString path, int revision, QList<SemanticDiagnostic> diagnostics) {
    for (int i = 0; i < count(); ++i) {
        auto *tab = page(i);
        if (tab->path == path && tab->view->document()->revision() == revision)
            tab->view->diagnostics(revision,diagnostics.mid(0,128));
    }
}
