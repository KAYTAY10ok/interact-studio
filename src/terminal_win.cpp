#include "terminal.h"

#ifdef Q_OS_WIN
#include "fonts.h"

#include <QDir>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QStackedLayout>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTextCodec>
#include <QTextCursor>
#include <QToolButton>
#include <QVBoxLayout>
#include <qt_windows.h>

#include <memory>

namespace {

QString findNativeExecutable(const QString &program)
{
    // Launch executables directly. Batch scripts require an explicit shell.
    const QString candidate = QStandardPaths::findExecutable(program);
    const QString suffix = QFileInfo(candidate).suffix().toLower();
    if (suffix == "exe" || suffix == "com") return candidate;
    if (QFileInfo(program).suffix().isEmpty())
        return QStandardPaths::findExecutable(program + ".exe");
    return {};
}

}

Terminal::Terminal(QWidget *parent)
    : QWidget(parent), workspace(QDir::currentPath()), stack(new QStackedLayout(this)),
      tabs(new QTabWidget(this))
{
    stack->setContentsMargins(0, 0, 0, 0);
    auto *idle = new QWidget(this);
    auto *layout = new QVBoxLayout(idle);
    auto *start = new QPushButton(tr("Open Windows console"), idle);
    layout->addWidget(start, 0, Qt::AlignCenter);
    connect(start, &QPushButton::clicked, this, &Terminal::launchShell);
    stack->addWidget(idle);

    tabs->setObjectName("terminalTabs");
    tabs->setDocumentMode(true);
    tabs->setTabsClosable(true);
    tabs->setMovable(true);
    tabs->setElideMode(Qt::ElideRight);
    auto *newTab = new QToolButton(tabs);
    newTab->setText("+");
    newTab->setToolTip(tr("New terminal session"));
    newTab->setAccessibleName(tr("New terminal session"));
    newTab->setPopupMode(QToolButton::InstantPopup);
    auto *menu = new QMenu(newTab);
    menu->addAction(tr("Local shell"), this, &Terminal::launchShell);
    menu->addAction(tr("SSH / virtual machine…"), this, &Terminal::launchSsh);
    menu->addAction(tr("Docker…"), this, &Terminal::launchDocker);
    menu->addAction(tr("GitHub repository"), this, &Terminal::launchGitHub);
    menu->addAction(tr("WSL"), this, &Terminal::launchWsl);
    newTab->setMenu(menu);
    tabs->setCornerWidget(newTab, Qt::TopRightCorner);
    stack->addWidget(tabs);
    connect(tabs, &QTabWidget::tabCloseRequested, this, [this](int index) {
        auto *page = tabs->widget(index);
        tabs->removeTab(index);
        page->deleteLater();
        if (!tabs->count()) stack->setCurrentIndex(0);
    });
}

void Terminal::setWorkspace(QString directory)
{
    workspace = QFileInfo(directory).absoluteFilePath();
}

void Terminal::addPage(QWidget *page, const QString &title)
{
    stack->setCurrentWidget(tabs);
    int index = tabs->addTab(page, title);
    tabs->setTabToolTip(index, workspace);
    tabs->setCurrentIndex(index);
    page->setFocus();
}

void Terminal::showMessage(const QString &title, const QString &message)
{
    auto *label = new QLabel(message);
    label->setAlignment(Qt::AlignCenter);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setMargin(12);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    addPage(label, title);
}

void Terminal::openSession(const QString &title, const QString &program, const QStringList &args)
{
    const QString executable = findNativeExecutable(program);
    if (executable.isEmpty()) {
        showMessage(title, tr("Executable unavailable: %1\nInstall it or add it to PATH.").arg(program));
        return;
    }
    if (!QFileInfo(workspace).isDir()) {
        showMessage(title, tr("Workspace directory unavailable: %1").arg(workspace));
        return;
    }

    QProcess process;
    process.setProgram(executable);
    process.setArguments(args);
    process.setWorkingDirectory(workspace);
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *options) {
        // Inherit a fresh interactive console instead of Qt's standard IO pipes.
        options->flags &= ~(DETACHED_PROCESS | CREATE_NO_WINDOW);
        options->flags |= CREATE_NEW_CONSOLE;
        options->inheritHandles = false;
        options->startupInfo->dwFlags &= ~STARTF_USESTDHANDLES;
        options->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
        options->startupInfo->wShowWindow = SW_SHOWNORMAL;
    });
    if (!process.startDetached()) {
        showMessage(title, tr("Could not open a Windows console: %1").arg(process.errorString()));
        return;
    }
    showMessage(title, tr("Session opened in a separate Windows console.\nClose that window to end the session."));
}

void Terminal::openCommand(const QString &title, const QString &program, const QStringList &args)
{
    const QString executable = findNativeExecutable(program);
    if (executable.isEmpty()) {
        showMessage(title, tr("Executable unavailable: %1\nInstall it or add it to PATH.").arg(program));
        return;
    }
    if (!QFileInfo(workspace).isDir()) {
        showMessage(title, tr("Workspace directory unavailable: %1").arg(workspace));
        return;
    }

    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(4, 4, 4, 4);
    auto *output = new QPlainTextEdit(page);
    output->setObjectName("taskOutput");
    output->setReadOnly(true);
    output->setFont(defaultEditorFont());
    output->setMaximumBlockCount(4000);
    layout->addWidget(output);
    auto *input = new QLineEdit(page);
    input->setAccessibleName(tr("Program input"));
    input->setPlaceholderText(tr("Send a line to the program"));
    layout->addWidget(input);

    auto *process = new QProcess(page);
    process->setProgram(executable);
    process->setArguments(args);
    process->setWorkingDirectory(workspace);
    process->setProcessChannelMode(QProcess::MergedChannels);
    auto decoder = std::shared_ptr<QTextDecoder>(QTextCodec::codecForLocale()->makeDecoder());
    auto cancel = new QPushButton(tr("Stop task"), page);
    layout->addWidget(cancel, 0, Qt::AlignRight);
    connect(cancel, &QPushButton::clicked, process, &QProcess::kill);
    const auto readOutput = [output, process, decoder] {
        const auto chunk = decoder->toUnicode(process->readAllStandardOutput());
        auto cursor = output->textCursor();
        cursor.movePosition(QTextCursor::End);
        cursor.insertText(chunk);
        // Also bound long output with no line separators.
        constexpr int maxChars = 512 * 1024;
        const int excess = output->document()->characterCount() - maxChars;
        if (excess > 0) {
            cursor.setPosition(0);
            cursor.setPosition(excess, QTextCursor::KeepAnchor);
            cursor.removeSelectedText();
        }
        cursor.movePosition(QTextCursor::End);
        output->setTextCursor(cursor);
    };
    connect(process, &QProcess::readyReadStandardOutput, page, readOutput);
    connect(input, &QLineEdit::returnPressed, page, [input, process] {
        process->write(input->text().toLocal8Bit() + "\n");
        input->clear();
    });
    connect(process, &QProcess::errorOccurred, page, [input, output, cancel, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        output->appendPlainText(QObject::tr("Could not start: %1").arg(process->errorString()));
        input->setEnabled(false);
        cancel->setEnabled(false);
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), page,
            [readOutput, input, output, cancel](int code, QProcess::ExitStatus status) {
        readOutput();
        output->appendPlainText(status == QProcess::NormalExit
            ? QObject::tr("Process exited with code %1.").arg(code)
            : QObject::tr("Process stopped unexpectedly."));
        input->setEnabled(false);
        cancel->setEnabled(false);
    });
    addPage(page, title);
    process->start();
}

void Terminal::launchCommand(QString program, QStringList args)
{
    openCommand(QFileInfo(program).fileName(), program, args);
}

void Terminal::launchShell()
{
    for (const auto &name : {QString("pwsh.exe"), QString("powershell.exe"),
                             qEnvironmentVariable("ComSpec"), QString("cmd.exe")}) {
        if (!name.isEmpty() && !findNativeExecutable(name).isEmpty()) {
            openSession(QFileInfo(name).fileName(), name, {});
            return;
        }
    }
    showMessage(tr("Local shell"), tr("PowerShell or cmd.exe is needed to open a local shell."));
}

void Terminal::launchGitHub()
{
    openCommand("GitHub", "gh", {"repo", "view"});
}

void Terminal::launchWsl()
{
    if (findNativeExecutable("wsl.exe").isEmpty()) {
        showMessage("WSL", tr("Install Windows Subsystem for Linux and a Linux distribution first."));
        return;
    }
    openSession("WSL", "wsl.exe", {});
}

void Terminal::launchDocker()
{
    if (findNativeExecutable("docker").isEmpty()) {
        showMessage("Docker", tr("Install the Docker CLI and connect it to a Docker engine."));
        return;
    }
    bool accepted;
    QString container = QInputDialog::getText(this, tr("Docker"),
        tr("Container name or ID (leave empty to list running containers)"),
        QLineEdit::Normal, {}, &accepted).trimmed();
    if (!accepted) return;
    if (container.isEmpty()) {
        openCommand("Docker", "docker", {"ps"});
        return;
    }
    if (!QRegularExpression("^[a-zA-Z0-9][a-zA-Z0-9_.-]*$").match(container).hasMatch()) {
        showMessage("Docker", tr("Enter a container name or ID from docker ps."));
        return;
    }
    openSession(container, "docker", {"exec", "-it", container, "sh"});
}

void Terminal::launchSsh()
{
    bool accepted;
    QString host = QInputDialog::getText(this, tr("Connect over SSH"),
        tr("SSH config alias or user@host"), QLineEdit::Normal, {}, &accepted).trimmed();
    if (!accepted || host.isEmpty()) return;
    if (host.startsWith('-') || host.contains(QRegularExpression("[\\s\\x00-\\x1f]"))) {
        showMessage("SSH", tr("Enter one hostname or SSH config alias."));
        return;
    }
    openSession(host, "ssh", {"--", host});
}
#endif
