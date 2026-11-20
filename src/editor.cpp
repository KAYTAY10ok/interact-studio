#include "editor.h"
#include "fonts.h"

#include <QApplication>
#include <QAbstractItemView>
#include <QCompleter>
#include <QScrollBar>
#include <QStringListModel>
#include <QCryptographicHash>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QSaveFile>
#include <QSet>
#include <QShortcut>
#include <QSyntaxHighlighter>
#include <QTextBlock>
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

enum class Mode { Plain, CLike, Hash, Json, Markdown, Lisp };
Mode fileMode(const QString &path) {
    const QString ext = QFileInfo(path).suffix().toLower();
    if (QStringList{"h","hpp","c","cc","cpp","rs","js","jsx","ts","tsx","go","java","cs","css","swift","kt"}.contains(ext)) return Mode::CLike;
    if (QStringList{"py","sh","bash","toml","yaml","yml","rb","cmake"}.contains(ext) || QFileInfo(path).fileName() == "Makefile") return Mode::Hash;
    if (ext == "json") return Mode::Json;
    if (ext == "md" || ext == "markdown") return Mode::Markdown;
    if (ext == "el" || ext == "lisp" || ext == "scm") return Mode::Lisp;
    return Mode::Plain;
}

class Colorizer final : public QSyntaxHighlighter {
public:
    explicit Colorizer(QPlainTextEdit *view) : QSyntaxHighlighter(view->document()), view(view) {}
    Mode mode = Mode::Plain;
private:
    QPlainTextEdit *view;
    void highlightBlock(const QString &line) override {
        if (mode == Mode::Plain) return;
        const int end = qMin(line.size(), 12000);
        const bool light = view->palette().base().color().lightness() > 145;
        const QColor comment(light ? "#506555" : "#85938d");
        const QColor literal(light ? "#397248" : "#b1cb91");
        const QColor keyword(light ? "#824876" : "#ce9fce");
        const QColor number(light ? "#965020" : "#e4b878");
        const QColor call(light ? "#226879" : "#88c9cb");
        if (mode == Mode::Markdown) {
            if (line.startsWith('#')) setFormat(0, end, call);
            else if (line.startsWith('>') || line.startsWith("```")) setFormat(0, end, comment);
            return;
        }
        static const QSet<QString> words = {
            "fn","let","mut","struct","impl","trait","match","enum","pub","use","mod","as",
            "const","constexpr","class","return","if","else","for","while","break","continue",
            "include","import","from","using","export","public","private","protected","type",
            "function","def","async","await","new","auto","int","void","bool","true","false",
            "null","None","True","False","static","self","this","switch","case","throw","try",
            "catch","namespace","template","typename","virtual","override","in","lambda","defun","setq"
        };
        bool blockComment = previousBlockState() == 1 && mode == Mode::CLike;
        int i = 0;
        while (i < end) {
            const int start = i;
            const QChar ch = line.at(i);
            const QChar next = i + 1 < end ? line.at(i + 1) : QChar();
            if (blockComment || (mode == Mode::CLike && ch == '/' && next == '*')) {
                int stop = line.indexOf("*/", i + (blockComment ? 0 : 2));
                blockComment = stop < 0;
                i = blockComment ? end : qMin(end, stop + 2);
                setFormat(start, i - start, comment);
            } else if ((mode == Mode::CLike && ch == '/' && next == '/') ||
                       (mode == Mode::Hash && ch == '#') || (mode == Mode::Lisp && ch == ';')) {
                setFormat(i, end - i, comment);
                break;
            } else if (ch == '"' || ch == '\'' || ch == '`') {
                ++i;
                while (i < end) {
                    QChar c = line.at(i++);
                    if (c == '\\' && i < end) ++i;
                    else if (c == ch) break;
                }
                setFormat(start, i - start, literal);
            } else if (ch.isDigit()) {
                while (++i < end && (line.at(i).isLetterOrNumber() || line.at(i) == '.' || line.at(i) == '_')) {}
                setFormat(start, i - start, number);
            } else if (ch.isLetter() || ch == '_') {
                while (++i < end && (line.at(i).isLetterOrNumber() || line.at(i) == '_')) {}
                if (words.contains(line.mid(start, i - start))) setFormat(start, i - start, keyword);
                else if (i < end && line.at(i) == '(') setFormat(start, i - start, call);
            } else ++i;
        }
        setCurrentBlockState(blockComment ? 1 : 0);
    }
};

class TextView;
class Gutter final : public QWidget {
public:
    explicit Gutter(TextView *owner);
    TextView *view;
    void paintEvent(QPaintEvent *event) override;
};

class TextView final : public QPlainTextEdit {
public:
    Colorizer *colorizer = nullptr;
    explicit TextView(QWidget *parent) : QPlainTextEdit(parent), gutter(new Gutter(this)) {
        setFrameStyle(QFrame::NoFrame);
        setLineWrapMode(NoWrap);
        setCenterOnScroll(false);
        connect(this, &QPlainTextEdit::blockCountChanged, this, [this]{ updateMetrics(); });
        connect(this, &QPlainTextEdit::updateRequest, this, [this](QRect rect, int dy) {
            if (dy) gutter->scroll(0, dy);
            else gutter->update(0, rect.y(), gutter->width(), rect.height());
        });
        connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this]{ markLine(); });
        updateMetrics();
        markLine();
    }
    void completeWord() {
        auto cursor = textCursor();
        if (cursor.hasSelection()) return;
        const auto identifier = [](QChar c) { return c.isLetterOrNumber() || c == '_'; };
        const QString line = cursor.block().text();
        int start = cursor.positionInBlock();
        while (start > 0 && identifier(line[start - 1])) --start;
        const QString prefix = line.mid(start, cursor.positionInBlock() - start);
        if (prefix.isEmpty()) return;

        // Completion is explicit and bounded, even for minified sources.
        QSet<QString> candidates;
        const int first = qMax(0, cursor.blockNumber() - 250);
        const int last = qMin(blockCount() - 1, cursor.blockNumber() + 250);
        int remaining = 512 * 1024;
        for (auto block = document()->findBlockByNumber(first);
             block.isValid() && block.blockNumber() <= last && remaining > 0; block = block.next()) {
            if (block.length() > 16384) continue;
            const QString content = block.text();
            remaining -= content.size();
            for (int pos = 0; pos < content.size();) {
                if (!identifier(content[pos])) { ++pos; continue; }
                int begin = pos++;
                while (pos < content.size() && identifier(content[pos])) ++pos;
                if (pos - begin <= prefix.size() || pos - begin > 150) continue;
                if (content.midRef(begin, prefix.size()) == prefix)
                    candidates.insert(content.mid(begin, pos - begin));
            }
        }
        if (candidates.isEmpty()) return;

        if (!completer) {
            completer = new QCompleter(this);
            completions = new QStringListModel(completer);
            completer->setModel(completions);
            completer->setWidget(this);
            completer->setCaseSensitivity(Qt::CaseSensitive);
            completer->setCompletionMode(QCompleter::PopupCompletion);
            connect(completer, QOverload<const QString &>::of(&QCompleter::activated), this,
                    [this](const QString &word) {
                auto cursor = textCursor();
                // Accept only the prefix still immediately before the caret.
                const QString prefix = completer->completionPrefix();
                if (!cursor.block().text().left(cursor.positionInBlock()).endsWith(prefix)) return;
                int right = cursor.positionInBlock();
                const QString line = cursor.block().text();
                while (right < line.size() && (line[right].isLetterOrNumber() || line[right] == '_')) ++right;
                const int anchor = cursor.position();
                cursor.setPosition(anchor - prefix.size());
                cursor.setPosition(cursor.block().position() + right, QTextCursor::KeepAnchor);
                cursor.insertText(word);
                setTextCursor(cursor);
            });
        }
        QStringList sorted = candidates.values();
        sorted.sort();
        completions->setStringList(sorted);
        completer->setCompletionPrefix(prefix);
        completer->setCurrentRow(0);
        auto bounds = cursorRect();
        bounds.setWidth(qMin(420, completer->popup()->sizeHintForColumn(0) + 26));
        completer->complete(bounds);
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
    void keyPressEvent(QKeyEvent *e) override {
        if (completer && completer->popup()->isVisible()) {
            switch (e->key()) {
            case Qt::Key_Tab: case Qt::Key_Enter: case Qt::Key_Return: case Qt::Key_Escape:
                e->ignore();
                return;
            default:
                completer->popup()->hide();
            }
        }
        if (e->key() == Qt::Key_Return && e->modifiers() == Qt::NoModifier && !isReadOnly()) {
            auto cursor = textCursor();
            const QString prefix = cursor.block().text().left(cursor.positionInBlock());
            int indent = 0;
            while (indent < prefix.size() && (prefix[indent] == ' ' || prefix[indent] == '\t')) ++indent;
            cursor.beginEditBlock();
            cursor.insertText("\n" + prefix.left(indent));
            if (prefix.trimmed().endsWith('{')) cursor.insertText("    ");
            cursor.endEditBlock();
            setTextCursor(cursor);
            return;
        }
        QPlainTextEdit::keyPressEvent(e);
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
        colorizer = new Colorizer(view);
        view->colorizer = colorizer;
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
    Colorizer *colorizer;
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
    tab->colorizer->mode = fileMode(path);
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
    if (fileMode(path) != tab->colorizer->mode) {
        tab->colorizer->mode = fileMode(path);
        tab->colorizer->rehighlight();
    }
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

void Editor::findText() {
    auto *tab = page(currentIndex());
    if (!tab) return;
    QString selected = tab->view->textCursor().selectedText();
    if (!selected.isEmpty() && !selected.contains(QChar::ParagraphSeparator)) tab->query->setText(selected);
    tab->search->show();
    tab->query->setFocus();
    tab->query->selectAll();
}

void Editor::completeWord() {
    auto *tab = page(currentIndex());
    if (tab) tab->view->completeWord();
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
