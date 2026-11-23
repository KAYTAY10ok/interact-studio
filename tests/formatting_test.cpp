#include "formatting.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <cstdio>
#include <memory>

namespace {
QString awaitFormat(QWidget &owner, QPlainTextEdit &view, const QString &path, bool editDuringRequest)
{
    const auto outcome = std::make_shared<QString>();
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline,&QTimer::timeout,&loop,&QEventLoop::quit);
    formatDocument(&owner,&view,path,[outcome, guard=QPointer<QEventLoop>(&loop)](const QString &text){
        if (text.endsWith(QChar(0x2026))) return; // Starting status.
        *outcome = text;
        if (guard) guard->quit();
    });
    if (editDuringRequest) view.insertPlainText("// pending edit\n");
    if (outcome->isEmpty()) {
        deadline.start(12000);
        loop.exec();
    }
    return *outcome;
}
}

int main(int argc,char **argv)
{
    const auto invoked = QFileInfo(QString::fromLocal8Bit(argv[0])).completeBaseName();
    if (invoked == "gofmt") {
        char buffer[512];
        const auto length = std::fread(buffer,1,sizeof buffer,stdin);
        if (!QByteArray(buffer,int(length)).contains("package main")) return 3;
        const char result[] = "package main\n\nfunc main() {}\n";
        return std::fwrite(result,1,sizeof result-1,stdout) == sizeof result-1 ? 0 : 1;
    }
    QApplication app(argc,argv);
    QTemporaryDir fixture;
    if (!fixture.isValid()) return 1;
    QString basename = "gofmt";
#if defined(Q_OS_WIN)
    basename += ".exe";
    if (!QFile::copy(app.applicationFilePath(),fixture.filePath(basename))) return 1;
#else
    if (!QFile::link(app.applicationFilePath(),fixture.filePath(basename))) return 1;
#endif
    qputenv("PATH",QDir::toNativeSeparators(fixture.path()).toLocal8Bit() + QDir::listSeparator().toLatin1() +
            qgetenv("PATH"));
    QWidget owner;
    QPlainTextEdit view(&owner);
    const QString original = "package main\nfunc main(  ){ }\n";
    view.setPlainText(original);
    const auto file = fixture.filePath("main.go");
    if (!awaitFormat(owner,view,file,false).startsWith("Formatted with") ||
            view.toPlainText() != "package main\n\nfunc main() {}\n") return 2;
    view.undo();
    if (view.toPlainText() != original) return 2;

    if (!awaitFormat(owner,view,file,true).contains("document changed") ||
            !view.toPlainText().contains("// pending edit")) return 3;
    QTextStream(stdout) << "PASS: formatting through stdin, undo, and stale edit protection\n";
    return 0;
}
