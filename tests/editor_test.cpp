#include "editor.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCompleter>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QKeyEvent>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTimer>
#include <cstdlib>

namespace {
int fail(const char *message) {
    qCritical() << message;
    return 1;
}

// The test must dismiss the intended modal, using its actual Cancel button.
void cancelNextDialog(const QString &title, bool &seen) {
    QTimer::singleShot(0, [title, &seen] {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (box && box->windowTitle() == title && box->button(QMessageBox::Cancel)) {
            seen = true;
            box->button(QMessageBox::Cancel)->click();
        }
    });
}

bool writeFile(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.flush();
}

QByteArray readFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

void key(QWidget *widget, int code, const QString &text = {}) {
    QKeyEvent event(QEvent::KeyPress,code,Qt::NoModifier,text);
    QApplication::sendEvent(widget,&event);
}
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QTimer::singleShot(10000, [] {
        qCritical() << "Editor test timed out in a modal dialog";
        std::_Exit(2);
    });

    QTemporaryDir dir;
    if (!dir.isValid()) return fail("Unable to create fixture directory");
    Editor editor;
    const QString path = dir.filePath("fixture.cpp");
    const QByteArray original = QByteArray::fromHex("efbbbf") + QStringLiteral("// é\u00a0x\r\n").toUtf8();
    const QString binaryPath = dir.filePath("replaced.cpp");
    if (!writeFile(binaryPath, QByteArray::fromHex("7f454c4600")) ||
        editor.openFile(binaryPath, false) || editor.count() != 0)
        return fail("Quiet startup restore accepted a binary file or opened a tab for it");

    if (!writeFile(path, original) || !editor.openFile(path)) return fail("Unable to open UTF-8 fixture");
    auto *view = editor.currentText();
    if (view->fontMetrics().horizontalAdvance('i') != view->fontMetrics().horizontalAdvance('m'))
        return fail("Default editor font is proportional");
    view->moveCursor(QTextCursor::End);
    view->insertPlainText("int main() {}");
    if (!editor.save() || readFile(path) != original + "int main() {}")
        return fail("Saving modified the BOM, UTF-8 text, or line endings");

    view->insertPlainText("// edits");
    if (!writeFile(path, "external change")) return fail("Unable to simulate external write");
    bool conflictSeen = false;
    cancelNextDialog("File changed on disk", conflictSeen);
    if (editor.save() || !conflictSeen || readFile(path) != "external change" || !view->document()->isModified())
        return fail("Cancelling a conflict lost the external change or unsaved edits");

    bool closeSeen = false;
    cancelNextDialog("Unsaved changes", closeSeen);
    editor.closeCurrent();
    if (!closeSeen || editor.count() != 1 || editor.currentText() != view)
        return fail("Cancelling close lost the dirty tab");

    editor.newFile();
    view = editor.currentText();
    view->insertPlainText("openWorkspace\nopenWorks");
    editor.completeWord();
    auto *completion = view->findChild<QCompleter *>();
    if (!completion || completion->completionCount() != 1 || completion->currentCompletion() != "openWorkspace")
        return fail("Completion did not find the nearby identifier");
    QMetaObject::invokeMethod(completion, "activated", Qt::DirectConnection, Q_ARG(QString, QStringLiteral("openWorkspace")));
    if (view->toPlainText() != "openWorkspace\nopenWorkspace") return fail("Completion replaced the wrong text");
    if (editor.openPaths() != QStringList{path}) return fail("Open paths should include only named tabs");

    editor.resize(700,420);
    editor.show();
    editor.activateWindow();
    QApplication::processEvents();
    const QString script = dir.filePath("indent.py");
    if (!writeFile(script,"if ready: # comment") || !editor.openFile(script))
        return fail("Unable to open Python fixture");
    view = editor.currentText();
    view->moveCursor(QTextCursor::End);
    key(view,Qt::Key_Return,"\r");
    if (view->toPlainText() != "if ready: # comment\n    ")
        return fail("Python auto-indent did not recognize the colon before a comment");
    key(view,Qt::Key_R,"r");
    key(view,Qt::Key_E,"e");
    key(view,Qt::Key_T,"t");
    QEventLoop debounce;
    QTimer::singleShot(260,&debounce,&QEventLoop::quit);
    debounce.exec();
    completion = view->findChild<QCompleter *>();
    if (!completion || !completion->popup()->isVisible() || completion->currentCompletion() != "return")
        return fail("Typing did not automatically offer a language keyword");
    key(completion->popup(),Qt::Key_Tab,"\t");
    if (view->toPlainText() != "if ready: # comment\n    return")
        return fail("Tab did not accept the selected keyword");
    view->undo();
    if (view->toPlainText() != "if ready: # comment\n    ret")
        return fail("Undo did not revert only the completed word");

    auto *split = editor.createSplitView(&editor);
    split->setDocument(view->document());
    split->moveCursor(QTextCursor::End);
    key(split,Qt::Key_Return,"\r");
    if (view->toPlainText() != "if ready: # comment\n    ret\n    ")
        return fail("The split editor changed or lost its language indentation");
    delete split;

    const auto braces = dir.filePath("braces.cpp");
    if (!writeFile(braces,"int main() { }") || !editor.openFile(braces))
        return fail("Unable to open bracket fixture");
    view = editor.currentText();
    auto cursor = view->textCursor();
    cursor.setPosition(12);
    view->setTextCursor(cursor);
    key(view,Qt::Key_Return,"\r");
    if (view->toPlainText() != "int main() {\n    \n}" || view->textCursor().positionInBlock() != 4)
        return fail("Auto-indent did not keep the paired closing brace aligned");

    const QString folder = dir.filePath("web");
    const QString moved = dir.filePath("site");
    const QString page = folder + "/index.html";
    if (!QDir().mkpath(folder) || !writeFile(page,"ht") || !editor.openFile(page))
        return fail("Unable to open HTML fixture");
    view = editor.currentText();
    view->moveCursor(QTextCursor::End);
    editor.completeWord();
    completion = view->findChild<QCompleter *>();
    if (!completion || !completion->currentCompletion().startsWith("html  "))
        return fail("Missing HTML skeleton snippet");
    key(completion->popup(),Qt::Key_Tab,"\t");
    if (!view->toPlainText().startsWith("<!doctype html>\n<html lang=") ||
            view->textCursor().block().text() != "  " || view->textCursor().positionInBlock() != 2)
        return fail("HTML skeleton was not inserted or the caret is outside its body");
    if (editor.canChangePath(folder))
        return fail("Editor allowed a folder change while its open document was dirty");
    if (!editor.save() || !editor.canChangePath(folder) || !QDir().rename(folder,moved))
        return fail("Unable to rename the saved fixture folder");
    editor.pathRenamed(folder,moved);
    if (editor.currentText() != view || editor.currentPath() != moved + "/index.html")
        return fail("Renaming a folder lost or failed to retarget its open editor");
    if (!QFile::remove(editor.currentPath())) return fail("Unable to delete the fixture file");
    editor.pathRemoved(moved + "/index.html");
    if (editor.openPaths().contains(moved + "/index.html"))
        return fail("Deleted document was not closed");

    qInfo() << "PASS: safe file edits, completion with Tab and undo, snippets, split auto-indent, explorer renames";
    return 0;
}
