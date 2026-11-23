#include "editor.h"
#include "fonts.h"
#include "languages.h"

#include <QApplication>
#include <QAbstractItemView>
#include <QCompleter>
#include <QScrollBar>
#include <QStringListModel>
#include <QCryptographicHash>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSaveFile>
#include <QRegularExpression>
#include <QSet>
#include <QShortcut>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTimer>
#include <algorithm>
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
        if (prefix.size() > 150 || (!explicitRequest && prefix.size() < 2)) return;

        QStringList quick;
        const QString path = languagePath();
        snippetBodies.clear();
        for (const auto &snippet : snippetsFor(languageName())) {
            const QString trigger = QString::fromLatin1(snippet.trigger);
            if (!trigger.startsWith(prefix)) continue;
            const QString label = trigger + "  ·  " + tr(snippet.description);
            quick << label;
            snippetBodies.insert(label,QString::fromLatin1(snippet.body));
        }
        QSet<QString> keywords;
        for (const auto &word : languageCompletions(path)) {
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
        if (quick.isEmpty()) { dismissSuggestions(); return; }

        if (!completer) {
            completer = new QCompleter(this);
            completions = new QStringListModel(completer);
            completer->setModel(completions);
            completer->setWidget(this);
            completer->setCaseSensitivity(Qt::CaseSensitive);
            completer->setCompletionMode(QCompleter::PopupCompletion);
            completer->setMaxVisibleItems(9);
            connect(completer,QOverload<const QString &>::of(&QCompleter::activated),this,
                    [this](const QString &choice) { acceptCompletion(choice); });
        }
        suggestionPosition = cursor.position();
        suggestionRevision = document()->revision();
        suggestionDocument = document();
        completions->setStringList(quick.mid(0,80));
        completer->setCompletionPrefix(prefix);
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
        setExtraSelections({sel});
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
        suggestTimer.stop();
        if (completer) completer->popup()->hide();
    }

    void acceptCompletion(const QString &choice) {
        auto cursor = textCursor();
        if (choice.isEmpty() || cursor.hasSelection() || cursor.position() != suggestionPosition ||
                suggestionDocument != document() || document()->revision() != suggestionRevision) return;
        const QString prefix = completer->completionPrefix();
        const QString line = cursor.block().text();
        if (cursor.positionInBlock() < prefix.size() ||
                line.midRef(cursor.positionInBlock() - prefix.size(),prefix.size()) != prefix) return;
        int right = cursor.positionInBlock();
        while (right < line.size() && identifier(line.at(right))) ++right;
        const int from = cursor.position() - prefix.size();
        QString text = snippetBodies.value(choice,choice);
        text.replace("\n","\n" + leadingIndent(line));
        const int caret = text.indexOf(QChar('\v'));
        if (caret >= 0) text.remove(caret,1);
        cursor.beginEditBlock();
        cursor.setPosition(from);
        cursor.setPosition(cursor.block().position() + right,QTextCursor::KeepAnchor);
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
        QPlainTextEdit::keyPressEvent(e);
        const auto text = e->text();
        // Modifier-based edit commands and navigation never open the popup.
        if (simple && !isReadOnly() &&
                ((!text.isEmpty() && identifier(text.back())) || key == Qt::Key_Backspace))
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
    QString path, untitled;
    QByteArray checksum;
    QString newline = "\n";
    bool bom = false;
    bool mixed = false;
};

Editor::Editor(QWidget *parent) : QTabWidget(parent), editorFont(defaultEditorFont()) {
    setDocumentMode(true);
    setTabsClosable(true);
    setMovable(true);
    setElideMode(Qt::ElideMiddle);
    connect(this, &QTabWidget::tabCloseRequested, this, [this](int i){ closePage(i); });
    connect(this, &QTabWidget::currentChanged, this, [this]{ if (onStateChanged) onStateChanged(); });
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
    tab->untitled = tr("Untitled %1").arg(++untitledNumber);
    setCurrentIndex(addTab(tab, tab->untitled));
    connect(tab->view->document(), &QTextDocument::modificationChanged, this, [this,tab]{ refresh(tab); });
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
    tab->path = path;
    tab->checksum = digest(bytes);
    tab->mixed = false;
    tab->view->setLanguagePath(path);
    tab->view->document()->setModified(false);
    refresh(tab);
    return true;
}
bool Editor::save() { return writePage(page(currentIndex()), false); }
bool Editor::saveAs() { return writePage(page(currentIndex()), true); }

bool Editor::mayClose(EditorPage *tab) {
    if (!tab->view->document()->isModified()) return true;
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
    return true;
}

void Editor::closePage(int index) {
    auto *tab = page(index);
    if (!tab || !mayClose(tab)) return;
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
