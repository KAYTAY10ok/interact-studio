#include "studio.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QTextStream>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setApplicationName("Interact Studio");
    app.setOrganizationName("InteractStudio");
    app.setApplicationVersion("0.1.0");
    app.setStyle("Fusion");

    QCommandLineParser parser;
    parser.setApplicationDescription("Interact Studio | Native code workspace");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("paths", "Workspace directory and/or UTF-8 source files.", "[paths...]");
    parser.process(app);

    QString root;
    QStringList files;
    for (const QString& arg : parser.positionalArguments()) {
        QFileInfo info(arg);
        if (!info.exists() || (!info.isFile() && !info.isDir())) {
            QTextStream(stderr) << "Path unavailable: " << arg << '\n';
            return 2;
        }
        if (info.isDir()) {
            if (!root.isEmpty()) {
                QTextStream(stderr) << "Choose one workspace directory.\n";
                return 2;
            }
            root = info.absoluteFilePath();
        } else files << info.absoluteFilePath();
    }
    if (root.isEmpty()) root = files.isEmpty() ? QDir::currentPath() : QFileInfo(files.first()).absolutePath();
    StudioWindow window(root);
    for (const QString& path : files) window.openFile(path);
    window.show();
    return app.exec();
}
