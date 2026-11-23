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
#include <QThread>

int main(int argc, char **argv)
{
    // The Windows terminal sends programs through QProcess. Exercise executable
    // paths and space-containing arguments from an installed-style GUI.
    if (argc == 3 && QByteArray(argv[1]) == "--mark") {
        QFile marker(QString::fromLocal8Bit(argv[2]));
        if (!marker.open(QIODevice::WriteOnly)) return 1;
        return marker.write("launched\n") == 9 ? 0 : 1;
    }

    QApplication app(argc, argv);
    QTemporaryDir area;
    if (!area.isValid()) return 1;
    app.setApplicationName("Interact Studio desktop test");
    app.setOrganizationName("InteractStudioTests");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, area.path());

    QFile source(area.filePath("code test.cpp"));
    if (!source.open(QIODevice::WriteOnly) || source.write("// initial\r\n") != 12) return 2;
    source.close();

    StudioWindow studio(area.path());
    studio.show();
    app.processEvents();
    if (!studio.isVisible() || !studio.openFile(source.fileName())) return 3;
    auto *editor = studio.editor();
    auto *text = editor->currentText();
    if (!text || text->toPlainText() != "// initial\n") return 4;
    text->moveCursor(QTextCursor::End);
    text->insertPlainText("// saved");
    auto *save = studio.findChild<QAction *>("file.save");
    if (!save || !save->isEnabled()) return 5;
    save->trigger();
    if (!source.open(QIODevice::ReadOnly) || source.readAll() != "// initial\r\n// saved") return 6;
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
    return QFileInfo::exists(path) ? 0 : 7;
}
