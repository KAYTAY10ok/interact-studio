#include "editor.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCompleter>
#include <QDebug>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QKeyEvent>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QThread>
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

bool await(const std::function<bool()> &condition) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < 4000) {
        QApplication::processEvents();
        QThread::msleep(10);
    }
    return condition();
}

int autosaveAndRecovery(const QTemporaryDir &dir) {
    const auto workspace = dir.filePath("recovery-project");
    if (!QDir().mkpath(workspace)) return fail("Unable to create recovery workspace");
    qputenv("XDG_DATA_HOME",QFile::encodeName(dir.filePath("private-data")));
    const auto file = workspace + "/auto.cpp";
    const QByteArray original = QByteArray::fromHex("efbbbf") + "// v1\r\n";
    if (!writeFile(file,original)) return fail("Unable to create auto-save test file");
    {
        Editor writer;
        writer.setRecoveryWorkspace(workspace);
        writer.setAutosaveEnabled(true,200);
        if (!writer.openFile(file)) return fail("Unable to open auto-save test file");
        auto *text = writer.currentText();
        text->moveCursor(QTextCursor::End);
        text->insertPlainText("// v2\n");
        if (!await([&] { return !text->document()->isModified(); }) ||
            readFile(file) != original + "// v2\r\n" || writer.hasRecoverableDrafts())
            return fail("Auto-save lost the encoding, line endings or modified state");
        QString status;
        writer.onAutosaveStatus = [&](QString message) { status = message; };
        text->insertPlainText("// unsaved");
        if (!writeFile(file,"external") || !await([&] { return status.contains("paused"); }) ||
            readFile(file) != "external" || !text->document()->isModified() ||
            QApplication::activeModalWidget() || !writer.hasRecoverableDrafts())
            return fail("Auto-save did not preserve an external change and its recovery copy");
    }
    {
        Editor reader;
        reader.setRecoveryWorkspace(workspace);
        reader.setAutosaveEnabled(true,200);
        if (!reader.hasRecoverableDrafts()) return fail("The recovery copy did not survive closing the editor");
        QTimer::singleShot(0, [] {
            if (auto *prompt = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
                prompt->accept();
        });
        reader.showRecovery();
        if (!reader.currentText() || !reader.currentPath().isEmpty() ||
            !reader.currentText()->toPlainText().endsWith("// unsaved") ||
            !reader.currentText()->document()->isModified() || readFile(file) != "external")
            return fail("Recovering a conflict changed the original file or failed to expose edits");
        bool closeSeen = false;
        QTimer::singleShot(0, [&] {
            if (auto *prompt = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                closeSeen = true;
                prompt->button(QMessageBox::Discard)->click();
            }
        });
        reader.closeCurrent();
        if (!closeSeen || reader.hasRecoverableDrafts()) return fail("Discarding a recovered tab left a draft behind");
    }
    {
        Editor untitled;
        untitled.setRecoveryWorkspace(workspace);
        untitled.newFile();
        untitled.currentText()->setPlainText("untitled after a crash");
        untitled.currentText()->document()->setModified(true);
        if (!await([&] { return untitled.hasRecoverableDrafts(); }))
            return fail("An untitled buffer did not get a recovery copy");
    }
    {
        Editor restart;
        restart.setRecoveryWorkspace(workspace);
        QTimer::singleShot(0, [] {
            if (auto *prompt = qobject_cast<QDialog *>(QApplication::activeModalWidget())) prompt->accept();
        });
        restart.showRecovery();
        if (!restart.currentText() || restart.currentText()->toPlainText() != "untitled after a crash")
            return fail("An untitled recovery copy could not be restored");
        auto *text = restart.currentText();
        text->document()->setModified(false);
    }
    return 0;
}
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QTimer::singleShot(18000, [] {
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

    const auto types = dir.filePath("semantic.cpp");
    if (!writeFile(types,"auto x = object.siz;") || !editor.openFile(types))
        return fail("Unable to open semantic completion fixture");
    view = editor.currentText();
    cursor = view->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.movePosition(QTextCursor::PreviousCharacter);
    view->setTextCursor(cursor);
    editor.activateWindow();
    view->setFocus();
    QApplication::processEvents();
    using CompletionReply = std::function<void(QList<SemanticCompletion>)>;
    CompletionReply reply;
    editor.onCompletionRequested = [&](const LanguageSnapshot &snapshot, CompletionReply callback, bool explicitRequest) {
        if (snapshot.path == types && snapshot.line == 0 && snapshot.column == 19 && explicitRequest)
            reply = std::move(callback);
    };
    editor.completeWord();
    if (!reply) return fail("The editor did not request semantic completion at the cursor");
    reply({{"size", "member", "size()", 0, 16, 0, 19}});
    completion = view->findChild<QCompleter *>();
    if (!completion || !completion->currentCompletion().startsWith("size"))
        return fail("The semantic result did not reach the editor popup");
    key(completion->popup(),Qt::Key_Tab,"\t");
    if (view->toPlainText() != "auto x = object.size();")
        return fail("The semantic insertion did not honor its precise edit range");

    if (autosaveAndRecovery(dir) != 0) return 1;
    qInfo() << "PASS: safe file edits, suggestions, indentation, explorer renames, auto-save and conflict/task recovery";
    return 0;
}
