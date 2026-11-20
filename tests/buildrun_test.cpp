#include "buildrun.h"

#include <QApplication>
#include <QDebug>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <functional>

namespace {
int fail(const QString &message)
{
    qCritical().noquote() << message;
    return 1;
}

bool writeFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size()
        && file.flush();
}

bool waitFor(const std::function<bool()> &done)
{
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < 12000) {
        QApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QThread::msleep(5);
    }
    return done();
}

bool runBinary(const QString &binary, const QString &workspace, const QByteArray &expected)
{
    QProcess program;
    program.setWorkingDirectory(workspace);
    program.start(binary, QStringList{});
    return program.waitForStarted(3000) && program.waitForFinished(3000)
        && program.exitStatus() == QProcess::NormalExit && program.exitCode() == 0
        && program.readAllStandardOutput() == expected;
}
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QTemporaryDir project;
    if (!project.isValid()) return fail("Could not create build fixture");
    QString cppBinary, rustBinary;
    {
        QWidget window;
        const auto cpp = project.filePath("valid program.cpp");
        if (!writeFile(cpp, "#include <cstdio>\nint main() { std::puts(\"cpp result\"); }\n"))
            return fail("Could not write C++ fixture");
        QStringList runArgs;
        buildAndRun(&window, project.path(), cpp, [&](QString program, QStringList args) {
            cppBinary = program;
            runArgs = args;
        });
        if (!waitFor([&] { return !cppBinary.isEmpty(); }) || !runArgs.isEmpty()
                || !runBinary(cppBinary, project.path(), "cpp result\n"))
            return fail("C++ build failed to launch a working binary");

        const auto bad = project.filePath("invalid.cpp");
        if (!writeFile(bad, "#error INTERACT_TEST_FAILURE\n"))
            return fail("Could not write invalid C++ fixture");
        bool attemptedLaunch = false;
        buildAndRun(&window, project.path(), bad, [&](QString, QStringList) { attemptedLaunch = true; });
        auto *dialog = window.findChild<QDialog *>("buildRunDialog");
        auto *status = dialog ? dialog->findChild<QLabel *>("buildStatus") : nullptr;
        auto *output = dialog ? dialog->findChild<QPlainTextEdit *>("buildOutput") : nullptr;
        if (!status || !output || !waitFor([&] { return status->text().contains("Build failed"); })
                || !output->toPlainText().contains("INTERACT_TEST_FAILURE") || attemptedLaunch)
            return fail("Compiler error did not appear in build output, or launch ran after failure");
        dialog->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

        const auto rust = project.filePath("hello world.rs");
        if (!writeFile(rust, "fn main() { println!(\"rust result\"); }\n"))
            return fail("Could not write Rust fixture");
        buildAndRun(&window, project.path(), rust, [&](QString program, QStringList) { rustBinary = program; });
        if (QStandardPaths::findExecutable("rustc").isEmpty()) {
            dialog = window.findChild<QDialog *>("buildRunDialog");
            status = dialog ? dialog->findChild<QLabel *>("buildStatus") : nullptr;
            if (!status || !status->text().contains("Install rustc") || !rustBinary.isEmpty())
                return fail("Missing rustc was not reported");
            qInfo() << "SKIP: Rust compilation (rustc unavailable). PASS: missing-compiler feedback.";
        } else if (!waitFor([&] { return !rustBinary.isEmpty(); })
                || !runBinary(rustBinary, project.path(), "rust result\n")) {
            return fail("Rust build failed to launch a working binary");
        } else {
            qInfo() << "PASS: Rust build and launch";
        }
    }
    if (QFileInfo::exists(cppBinary) || (!rustBinary.isEmpty() && QFileInfo::exists(rustBinary)))
        return fail("Temporary build artifacts outlived the workspace window");
    qInfo() << "PASS: C++ build and launch, compiler error, temporary output cleanup";
    return 0;
}
