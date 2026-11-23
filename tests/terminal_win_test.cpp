#include "terminal.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <qt_windows.h>

#include <functional>

namespace {

const QStringList probeArguments{"--probe", "argument with spaces", "", "say \"hello\"", "path with spaces\\"};

bool waitFor(const std::function<bool()> &condition)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 5000) {
        QApplication::processEvents();
        QThread::msleep(15);
    }
    return condition();
}

int fail(const QString &reason)
{
    QTextStream(stderr) << reason << '\n';
    return 1;
}

}

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsSet("INTERACT_TEST_CONSOLE_MARKER")) {
        DWORD mode;
        if (!GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode)) return 2;
        QFile marker(qEnvironmentVariable("INTERACT_TEST_CONSOLE_MARKER"));
        return marker.open(QIODevice::WriteOnly) && marker.write("console") == 7 ? 0 : 3;
    }

    auto probe = argc > 1 && QByteArray(argv[1]) == "--probe";
    if (probe) {
        QCoreApplication app(argc, argv);
        if (app.arguments().mid(1) != probeArguments || QDir::current().dirName() != "Windows project")
            return fail("unexpected arguments or working directory");
        QTextStream input(stdin);
        QTextStream output(stdout);
        output << "literal arguments verified\n" << Qt::flush;
        return input.readLine() == "studio input" ? 0 : 4;
    }

    QApplication app(argc, argv);
    QTemporaryDir temp;
    const auto workspace = temp.filePath("Windows project");
    if (!temp.isValid() || !QDir().mkpath(workspace)) return fail("unable to create project");
    const auto executable = temp.filePath("local tool.exe");
    if (!QFile::copy(app.applicationFilePath(), executable)) return fail("unable to copy probe");
    Terminal terminal;
    terminal.setWorkspace(workspace);
    terminal.launchCommand(executable, probeArguments);
    auto *output = terminal.findChild<QPlainTextEdit *>("taskOutput");
    auto *input = terminal.findChild<QLineEdit *>();
    if (!output || !input || !waitFor([&] { return output->toPlainText().contains("literal arguments verified"); }))
        return fail("the command did not receive literal argv and project cwd");
    input->setText("studio input");
    QMetaObject::invokeMethod(input, "returnPressed");
    if (!waitFor([&] { return output->toPlainText().contains("Process exited with code 0"); }))
        return fail("the command did not receive input or report success");

    const auto shell = temp.filePath("pwsh.exe");
    if (!QFile::copy(app.applicationFilePath(), shell)) return fail("unable to copy console probe");
    const auto marker = temp.filePath("console.txt");
    qputenv("PATH", QDir::toNativeSeparators(temp.path()).toLocal8Bit() + ';' + qgetenv("PATH"));
    qputenv("INTERACT_TEST_CONSOLE_MARKER", marker.toLocal8Bit());
    terminal.launchShell();
    qunsetenv("INTERACT_TEST_CONSOLE_MARKER");
    if (!waitFor([&] { return QFileInfo(marker).size() == 7; }))
        return fail("the shell did not receive a native Windows console");
    QTextStream(stdout) << "PASS Windows terminal: argv, cwd, task input/output, detached console\n";
    return 0;
}
