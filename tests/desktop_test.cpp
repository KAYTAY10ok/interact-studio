#include "editor.h"
#include "studio.h"
#include "terminal.h"

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QPlainTextEdit>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>

static int fail(int code, const char *message)
{
    QTextStream(stderr) << "Desktop smoke: " << message << '\n';
    return code;
}

int main(int argc, char **argv)
{
    // The Windows terminal sends programs through QProcess. Exercise executable
    // paths and space-containing arguments from an installed-style GUI.
    if (argc == 3 && QByteArray(argv[1]) == "--mark") {
        QFile marker(QString::fromLocal8Bit(argv[2]));
        if (!marker.open(QIODevice::WriteOnly) || marker.write("launched\n") != 9)
            return fail(1, "Unable to write task marker");
        return 0;
    }

    QApplication app(argc, argv);
    QTemporaryDir area;
    if (!area.isValid()) return fail(1, "Unable to create a temporary workspace");
    app.setApplicationName("Interact Studio desktop test");
    app.setOrganizationName("InteractStudioTests");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, area.path());

    QFile source(area.filePath("code test.cpp"));
    if (!source.open(QIODevice::WriteOnly) || source.write("// initial\r\n") != 12)
        return fail(2, "Unable to write source fixture");
    source.close();

    StudioWindow studio(area.path());
    studio.show();
    app.processEvents();
    if (!studio.isVisible() || !studio.openFile(source.fileName()))
        return fail(3, "Studio or source fixture did not open");
    auto *editor = studio.editor();
    auto *text = editor->currentText();
    if (!text || text->toPlainText() != "// initial\n")
        return fail(4, "Editor did not load expected source text");
    text->moveCursor(QTextCursor::End);
    text->insertPlainText("// saved");
    auto *save = studio.findChild<QAction *>("file.save");
    if (!save || !save->isEnabled()) return fail(5, "Save action is unavailable");
    save->trigger();
    if (!source.open(QIODevice::ReadOnly) || source.readAll() != "// initial\r\n// saved")
        return fail(6, "Save did not preserve text and CRLF");
    source.close();

    Terminal terminal;
    terminal.setWorkspace(area.path());
    terminal.launchCommand(QCoreApplication::applicationFilePath(), {"--mark", "marker with spaces"});
    const QString path = area.filePath("marker with spaces");
    QElapsedTimer elapsed;
    elapsed.start();
    while (!QFileInfo::exists(path) && elapsed.elapsed() < 5000) {
        app.processEvents();
        QThread::msleep(10);
    }
    if (!QFileInfo::exists(path)) {
        if (auto *output = terminal.findChild<QPlainTextEdit *>("taskOutput")) {
            QTextStream(stderr) << output->toPlainText() << '\n';
        }
        return fail(7, "Terminal task did not create its marker in the workspace");
    }
    return 0;
}
