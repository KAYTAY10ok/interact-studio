#include "terminal.h"

#include <qtermwidget5/qtermwidget.h>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QRegularExpression>
#include <QStackedLayout>
#include <QStandardPaths>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>

namespace {
void prepareProcess(QTermWidget *view, const QString &cwd,
                    const QString &program, const QStringList &args)
{
    // QTermWidget 1.x expands environment references in paths and argv. Pass
    // the original strings as data so '$', spaces and empty args survive.
    QStringList environment{"TERM=xterm-256color", "INTERACT_PTY_CWD=" + cwd};
    QStringList names{"INTERACT_PTY_CWD"};
    QStringList references;
    const QStringList argv = QStringList{program} + args;
    for (int i = 0; i < argv.size(); ++i) {
        QString name = "INTERACT_PTY_ARG_" + QString::number(i);
        environment << name + "=" + argv.at(i);
        names << name;
        references << "\"${" + name + "}\"";
    }
    const QString script = "cd -- \"${INTERACT_PTY_CWD}\" || exit 1\nset -- " +
        references.join(' ') + "\nunset " + names.join(' ') + "\nexec \"$@\"";
    view->setEnvironment(environment);
    view->setShellProgram("/bin/sh");
    view->setWorkingDirectory("/");
    view->setArgs({"-c", script});
}
}

Terminal::Terminal(QWidget *parent)
    : QWidget(parent), workspace(QDir::currentPath()), stack(new QStackedLayout(this)),
      tabs(new QTabWidget(this))
{
    stack->setContentsMargins(0, 0, 0, 0);
    auto *idle = new QWidget(this);
    auto *layout = new QVBoxLayout(idle);
    auto *start = new QPushButton(tr("Open local shell"), idle);
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
    const QString executable = QStandardPaths::findExecutable(program);
    if (executable.isEmpty()) {
        showMessage(title, tr("Executable unavailable: %1\nInstall it or add it to PATH.").arg(program));
        return;
    }
    if (!QFileInfo(workspace).isDir()) {
        showMessage(title, tr("Workspace directory unavailable: %1").arg(workspace));
        return;
    }

    auto *view = new QTermWidget(0);
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString bundledKeys = appDir.filePath("kb-layouts/default");
    if (!QFileInfo::exists("/usr/share/qtermwidget5/kb-layouts/default.keytab") &&
            QFileInfo::exists(bundledKeys + ".keytab"))
        view->setKeyBindings(bundledKeys);
    const QString scheme = appDir.filePath("color-schemes/BreezeModified.colorscheme");
    view->setColorScheme(QFileInfo::exists(scheme) ? scheme : QString("BreezeModified"));
    view->setMargin(6);
    view->setScrollBarPosition(QTermWidgetInterface::ScrollBarRight);
    view->setHistorySize(4000);
    view->setConfirmMultilinePaste(true);
    prepareProcess(view, workspace, executable, args);

    view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(view, &QWidget::customContextMenuRequested, view, [view](const QPoint &point) {
        QMenu menu;
        menu.addAction(QObject::tr("Copy"), view, &QTermWidget::copyClipboard)
            ->setEnabled(!view->selectedText().isEmpty());
        menu.addAction(QObject::tr("Paste"), view, &QTermWidget::pasteClipboard);
        menu.addAction(QObject::tr("Find…"), view, &QTermWidget::toggleShowSearchBar);
        menu.exec(view->mapToGlobal(point));
    });
    connect(view, &QTermWidget::finished, this, [this, view, title]() {
        const int index = tabs->indexOf(view);
        if (index >= 0) tabs->setTabText(index, tr("%1 · finished").arg(title));
    });
    addPage(view, title);
    view->startShellProgram();
}

void Terminal::launchCommand(QString program, QStringList args)
{
    openSession(QFileInfo(program).fileName(), program, args);
}

void Terminal::launchShell()
{
    QString shell = qEnvironmentVariable("SHELL");
    if (QStandardPaths::findExecutable(shell).isEmpty()) shell = "/bin/sh";
    openSession(QFileInfo(shell).fileName(), shell, {});
}

void Terminal::launchGitHub()
{
    openSession("GitHub", "gh", {"repo", "view"});
}

void Terminal::launchWsl()
{
    if (QStandardPaths::findExecutable("wsl.exe").isEmpty()) {
        showMessage("WSL", tr("WSL needs wsl.exe on PATH. Run Studio inside WSL with Windows interop enabled, or connect to the machine over SSH."));
        return;
    }
    openSession("WSL", "wsl.exe", {});
}

void Terminal::launchDocker()
{
    if (QStandardPaths::findExecutable("docker").isEmpty()) {
        showMessage("Docker", tr("Install the Docker CLI and connect it to a Docker engine."));
        return;
    }
    bool accepted;
    QString container = QInputDialog::getText(this, tr("Docker"),
        tr("Container name or ID (leave empty to list running containers)"),
        QLineEdit::Normal, {}, &accepted).trimmed();
    if (!accepted) return;
    if (container.isEmpty()) {
        openSession("Docker", "docker", {"ps"});
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
