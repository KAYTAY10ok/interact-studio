#include "editor.h"

#include <QApplication>
#include <QCompleter>
#include <QDebug>
#include <QFile>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>
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
    qInfo() << "PASS: UTF-8 BOM/CRLF, external change conflict, cancelled tab close, completion, quiet session restore, monospace";
    return 0;
}
