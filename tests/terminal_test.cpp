#include "terminal.h"

#include <qtermwidget5/qtermwidget.h>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <functional>

static bool until(const std::function<bool()> &ready)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!ready() && elapsed.elapsed() < 5000) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return ready();
}

static int failure(const QString &message)
{
    QTextStream(stderr) << message << '\n';
    return 1;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QTemporaryDir temp;
    const QString workspace = temp.filePath("project with $USER");
    if (!temp.isValid() || !QDir().mkpath(workspace)) return failure("Cannot create fixture");

    Terminal terminal;
    terminal.resize(800, 300);
    terminal.setWorkspace(workspace);
    terminal.show();
    app.processEvents();

    // Exercise literal argv, terminal input, terminal output and the child cwd.
    const QString name = "output $HOME";
    terminal.launchCommand("sh", {"-c",
        "test -t 0 && test -t 1 && test -z \"$2\" || exit 1; "
        "IFS= read -r line && printf '%s' \"$line\" > \"$1\" && printf 'PTY_ROUNDTRIP_OK\\n'",
        "terminal-test", name, ""});
    auto *view = terminal.findChild<QTermWidget *>();
    if (!view) return failure("No terminal emulator created");
    QString output;
    QObject::connect(view, &QTermWidget::receivedData, &terminal,
                     [&](const QString &data) { output += data; });
    view->sendText("input from Studio\r");
    if (!until([&] { return output.contains("PTY_ROUNDTRIP_OK"); }))
        return failure("Command output did not return through PTY");
    QFile result(QDir(workspace).filePath(name));
    if (!result.open(QIODevice::ReadOnly) || result.readAll() != "input from Studio")
        return failure("Child cwd, argv or keyboard input changed");

    auto *tabs = terminal.findChild<QTabWidget *>("terminalTabs");
    if (!until([&] { return tabs->tabText(0).contains("finished"); }))
        return failure("Missing session completion");
    terminal.launchShell();
    if (tabs->count() != 2 || tabs->currentIndex() != 1)
        return failure("Second interactive session did not open");
    auto *shell = qobject_cast<QTermWidget *>(tabs->currentWidget());
    if (!shell) return failure("Default shell has no terminal widget");
    shell->sendText("exit\r");
    if (!until([&] { return tabs->tabText(1).contains("finished"); }))
        return failure("Shell did not accept exit");
    tabs->tabCloseRequested(1);
    tabs->tabCloseRequested(0);
    if (tabs->count()) return failure("Closing sessions left tabs open");
    QTextStream(stdout) << "Terminal PTY round trip, literal argv, working directory and sessions passed.\n";
}
