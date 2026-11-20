#include "../src/editor.h"
#include "../src/studio.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QElapsedTimer>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QPlainTextEdit>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QTimer>
#include <QDebug>
#include <cstdlib>

static int fail(const QString &message)
{
    qCritical().noquote() << message;
    return 1;
}

int main(int argc, char **argv)
{
    if (argc == 3 && QByteArray(argv[1]) == "--extension-probe") {
        QFile marker(QString::fromLocal8Bit(argv[2]));
        return marker.open(QIODevice::WriteOnly) && marker.write("invoked") == 7 ? 0 : 1;
    }
    QTemporaryDir temp;
    if (!temp.isValid())
        return fail("Unable to create a temporary project");
    qputenv("XDG_CONFIG_HOME", temp.filePath("config").toUtf8());
    qputenv("XDG_DATA_HOME", temp.filePath("data").toUtf8());
    QApplication app(argc, argv);
    QTimer::singleShot(10000, [] {
        qCritical() << "Studio smoke test timed out";
        std::_Exit(2);
    });
    app.setOrganizationName("InteractSmoke");
    app.setApplicationName("Studio");
    app.setQuitOnLastWindowClosed(false);

    const auto root = temp.filePath("project");
    if (!QDir().mkpath(root))
        return fail("Unable to create a project directory");
    const auto path = root + "/sample.cpp";
    const auto obsoletePath = root + "/obsolete.cpp";
    const QByteArray initial = "// café\r\n";
    const QByteArray addition = "int main() { return 0; }";
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(initial) != initial.size())
        return fail("Unable to seed the sample");
    file.close();
    QFile obsolete(obsoletePath);
    if (!obsolete.open(QIODevice::WriteOnly) || obsolete.write("// remove\n") != 10)
        return fail("Unable to seed the second sample");
    obsolete.close();

    const auto extensionDir = temp.filePath("config/interact-studio/extensions");
    const auto markerName = QStringLiteral("extension marker");
    if (!QDir().mkpath(extensionDir))
        return fail("Unable to create the extension directory");
    const QJsonObject manifest{
        {"id", "test.probe"}, {"name", "Smoke extension"},
        {"program", app.applicationFilePath()},
        {"arguments", QJsonArray{"--extension-probe", markerName}}
    };
    const auto json = QJsonDocument(manifest).toJson();
    QFile extension(extensionDir + "/probe.json");
    if (!extension.open(QIODevice::WriteOnly) || extension.write(json) != json.size())
        return fail("Unable to write the extension manifest");
    extension.close();

    QString dockName;
    {
        StudioWindow window(root);
        window.show();
        app.processEvents();
        if (!window.openFile(path))
            return fail("Opening a project file failed");
        auto *editor = window.editor();
        if (!editor || !editor->currentText() || editor->currentPath() != path)
            return fail("The opened file is not active");
        const int tabs = editor->count();
        if (!window.openFile(path) || editor->count() != tabs)
            return fail("Opening a file twice created a duplicate tab");

        auto *text = editor->currentText();
        QAction *split = window.findChild<QAction*>("view.split");
        if (!split)
            return fail("Split view action unavailable");
        split->trigger();
        auto *second = window.findChild<QPlainTextEdit*>("splitEditor");
        if (!second || second->document() != text->document())
            return fail("Split editor does not share the active document");
        if (!window.openFile(obsoletePath) || second->document() != editor->currentText()->document())
            return fail("Split editor did not switch to the newly opened tab");
        if (!window.openFile(path) || second->document() != text->document())
            return fail("Split editor did not switch back to the original tab");
        second->moveCursor(QTextCursor::End);
        second->insertPlainText(QString::fromUtf8(addition));
        if (!text->document()->isModified())
            return fail("The edited file was not marked as modified");
        QAction *save = window.findChild<QAction*>("file.save");
        if (!save || !save->isEnabled())
            return fail("The application save command is unavailable");
        save->trigger();
        if (text->document()->isModified() || !file.open(QIODevice::ReadOnly))
            return fail("Save failed to persist the modified document");
        if (file.readAll() != initial + addition)
            return fail("Saving changed the UTF-8 text or original CRLF convention");
        file.close();

        for (auto *dock : window.findChildren<QDockWidget*>()) {
            if (!dock->objectName().isEmpty() && dock->isVisible()) {
                dockName = dock->objectName();
                dock->hide();
                break;
            }
        }
        auto *dock = window.findChild<QDockWidget*>(dockName);
        auto *saveLayout = window.findChild<QAction*>("view.saveLayout");
        auto *loadLayout = window.findChild<QAction*>("view.loadLayout");
        if (!dock || !saveLayout || !loadLayout)
            return fail("Layout commands are missing");
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
            if (dialog) {
                dialog->setTextValue("Review / lecture");
                dialog->accept();
            }
        });
        saveLayout->trigger();
        dock->show();
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
            if (dialog) dialog->accept();
        });
        loadLayout->trigger();
        if (!dock->isHidden())
            return fail("Named layout did not restore the hidden dock");
        if (!window.close())
            return fail("Closing the customized workspace failed");
    }
    // An old session entry might have been replaced with a binary file.
    if (!obsolete.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        obsolete.write(QByteArray::fromHex("7f454c4600")) != 5)
        return fail("Unable to replace the second sample");
    obsolete.close();
    {
        StudioWindow reopened(root);
        reopened.show();
        app.processEvents();
        auto *dock = reopened.findChild<QDockWidget*>(dockName);
        if (!dock || dock->isVisible())
            return fail("The customized dock layout was lost on restart");
        if (reopened.editor()->openPaths() != QStringList{path} ||
                reopened.editor()->currentPath() != path || !reopened.editor()->currentText())
            return fail("Workspace did not restore the valid tab and skip the binary tab");
        if (reopened.editor()->currentText()->toPlainText() != QString::fromUtf8(initial + addition).replace("\r\n", "\n"))
            return fail("Reopened text differs from saved text");
        auto *second = reopened.findChild<QPlainTextEdit*>("splitEditor");
        if (!second || second->document() != reopened.editor()->currentText()->document())
            return fail("Workspace did not restore the split editor");
        QAction *probe = nullptr;
        for (auto *command : reopened.findChildren<QAction*>()) {
            if (command->text() == "Smoke extension") probe = command;
        }
        if (!probe)
            return fail("The installed command extension is unavailable");
        probe->trigger();
        QFile marker(root + "/" + markerName);
        QElapsedTimer deadline;
        deadline.start();
        while (marker.size() != 7 && deadline.elapsed() < 5000) {
            app.processEvents();
            QThread::msleep(5);
        }
        if (!marker.open(QIODevice::ReadOnly) || marker.readAll() != "invoked")
            return fail("The command extension did not run with its expected directory and arguments");
        QAction *close = reopened.findChild<QAction*>("file.close");
        if (!close) return fail("Close editor action unavailable");
        close->trigger();
        if (reopened.editor()->currentText() || reopened.findChild<QPlainTextEdit*>("splitEditor"))
            return fail("Closing the final tab left a split editor attached");
        reopened.close();
    }
    qInfo() << "PASS: open, split edit, save UTF-8/CRLF, restore valid tab and layout, run extension, close final tab";
    return 0;
}
