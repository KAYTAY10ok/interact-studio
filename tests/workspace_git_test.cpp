#include "workspace.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <functional>
#include <stdexcept>
#include <cstdio>

namespace {
void require(bool result, const char *message) {
    if (!result) throw std::runtime_error(message);
}

void waitUntil(const std::function<bool()> &condition, const char *failure) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < 12000) {
        qApp->processEvents();
        QThread::msleep(5);
    }
    require(condition(), failure);
}

QString git(const QString &cwd, const QStringList &args) {
    QProcess process;
    process.setWorkingDirectory(cwd);
    process.start("git", args);
    if (!process.waitForFinished(12000)) {
        process.kill();
        process.waitForFinished();
        throw std::runtime_error("Git fixture timed out");
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        throw std::runtime_error(process.readAllStandardError().constData());
    return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
}

void put(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "Could not write Git fixture");
}

void identity(const QString &repo) {
    git(repo, {"config", "--local", "user.name", "Fixture User"});
    git(repo, {"config", "--local", "user.email", "fixture@example.invalid"});
    git(repo, {"config", "--local", "commit.gpgsign", "false"});
}

void answerDialog(Workspace &workspace, QAction *action,
                  const std::function<bool(QWidget *)> &answer) {
    bool answered = false;
    QTimer watch;
    watch.setInterval(8);
    QObject::connect(&watch, &QTimer::timeout, &workspace, [&] {
        auto *dialog = QApplication::activeModalWidget();
        if (dialog && answer(dialog)) {
            answered = true;
            watch.stop();
        }
    });
    watch.start();
    action->trigger();
    const auto failure = action->text().toStdString() + " did not finish after accepting its dialog";
    waitUntil([&] { return answered && !workspace.gitOperationInProgress() && action->isEnabled(); },
              failure.c_str());
}

void run() {
    QTemporaryDir temp;
    require(temp.isValid(), "Could not create Git fixture root");
    const auto directory = temp.path();
    const auto globalConfig = temp.filePath("global.gitconfig");
    put(globalConfig, "[user]\n\tname = Global Fixture\n\temail = global@example.invalid\n");
    qputenv("GIT_CONFIG_GLOBAL", QFile::encodeName(globalConfig));
    qputenv("GIT_CONFIG_NOSYSTEM", "1");
    qunsetenv("GIT_AUTHOR_NAME");
    qunsetenv("GIT_AUTHOR_EMAIL");
    qunsetenv("GIT_COMMITTER_NAME");
    qunsetenv("GIT_COMMITTER_EMAIL");

    git(directory, {"init", "--bare", "--initial-branch=main", "remote repo.git"});
    git(directory, {"init", "--initial-branch=main", "my project"});
    const auto remote = temp.filePath("remote repo.git");
    const auto mine = temp.filePath("my project");
    const auto peer = temp.filePath("peer project");
    identity(mine);
    put(QDir(mine).filePath("main.txt"), "original\n");
    git(mine, {"add", "main.txt"});
    git(mine, {"commit", "-m", "Initial"});
    git(mine, {"remote", "add", "origin", remote});
    git(mine, {"push", "--set-upstream", "origin", "main"});
    git(directory, {"clone", remote, "peer project"});
    identity(peer);

    Workspace workspace;
    workspace.setRoot(mine);
    int checkouts = 0;
    workspace.onCheckout = [&] { ++checkouts; };
    bool acceptCheckout = true;
    workspace.beforeCheckout = [&] { return acceptCheckout; };
    QString github;
    workspace.onGitHub = [&](QString url) { github = url; };
    QString terminalProgram;
    QStringList terminalArgs;
    workspace.onGitCommand = [&](QString program, QStringList args) {
        terminalProgram = program;
        terminalArgs = args;
    };
    workspace.show();
    workspace.showGit();
    auto button = [&](const char *name) {
        auto *child = workspace.findChild<QAction *>(name);
        require(child, "Missing Git action");
        return child;
    };
    auto *summary = workspace.findChild<QLabel *>("gitBranchSummary");
    auto *settings = button("gitIdentity");
    require(summary, "Missing Git branch summary");
    waitUntil([&] { return settings->isEnabled() && summary->text().contains("origin/main"); },
              "Branch summary did not show its upstream");

    bool displayedIdentity = false;
    answerDialog(workspace, settings, [&](QWidget *dialog) {
        if (dialog->objectName() != "gitIdentityDialog") return false;
        auto *name = dialog->findChild<QLineEdit *>("gitIdentityName");
        auto *email = dialog->findChild<QLineEdit *>("gitIdentityEmail");
        displayedIdentity = name && email && name->text() == "Fixture User";
        if (displayedIdentity) {
            name->setText("Studio Project");
            email->clear();
        }
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        return true;
    });
    require(displayedIdentity && git(mine, {"config", "--local", "--get", "user.name"}) == "Studio Project" &&
            git(mine, {"config", "--get", "user.email"}) == "global@example.invalid",
            "Repository identity did not preserve an inherited email when clearing the local override");

    answerDialog(workspace, button("gitCreate"), [](QWidget *dialog) {
        auto *input = qobject_cast<QInputDialog *>(dialog);
        if (!input) return false;
        input->setTextValue("feature/plan");
        input->accept();
        return true;
    });
    require(git(mine, {"branch", "--show-current"}) == "feature/plan" && checkouts == 1,
            "Creating a branch did not check it out");
    const auto toMain = [](QWidget *dialog) {
        auto *input = qobject_cast<QInputDialog *>(dialog);
        if (!input) return false;
        auto *choices = input->findChild<QComboBox *>();
        if (choices) choices->setCurrentText("main");
        input->accept();
        return true;
    };
    acceptCheckout = false;
    answerDialog(workspace, button("gitSwitch"), toMain);
    require(git(mine, {"branch", "--show-current"}) == "feature/plan" && checkouts == 1,
            "The editor checkout veto did not stop the switch");
    acceptCheckout = true;
    answerDialog(workspace, button("gitSwitch"), toMain);
    require(git(mine, {"branch", "--show-current"}) == "main" && checkouts == 2,
            "Switching to a local branch failed");

    put(QDir(mine).filePath("local.txt"), "local work\n");
    git(mine, {"add", "local.txt"});
    git(mine, {"commit", "-m", "Local work"});
    workspace.showGit();
    auto *push = button("gitPush");
    waitUntil([&] { return push->isEnabled() && summary->text().contains(QString::fromUtf8("↑1")); },
              "Ahead count was not shown");
    git(mine, {"config", "--local", "remote.origin.mirror", "true"});
    answerDialog(workspace, push, [](QWidget *dialog) {
        auto *box = qobject_cast<QMessageBox *>(dialog);
        if (!box) return false;
        box->button(QMessageBox::Yes)->click();
        return true;
    });
    require(git(remote, {"rev-parse", "refs/heads/main"}) == git(mine, {"rev-parse", "HEAD"}),
            "The confirmed push did not update main");
    require(git(remote, {"for-each-ref", "--format=%(refname)", "refs/heads/"}) == "refs/heads/main",
            "The push accidentally mirrored another local branch");

    git(peer, {"pull", "--ff-only"});
    put(QDir(peer).filePath("remote.txt"), "remote work\n");
    git(peer, {"add", "remote.txt"});
    git(peer, {"commit", "-m", "Remote work"});
    git(peer, {"push"});
    auto *fetch = button("gitFetch");
    fetch->trigger();
    waitUntil([&] { return fetch->isEnabled() && summary->text().contains(QString::fromUtf8("↓1")); },
              "Fetch did not refresh behind count");
    auto *pull = button("gitPull");
    pull->trigger();
    waitUntil([&] { return pull->isEnabled() && !summary->text().contains(QString::fromUtf8("↓1")); },
              "Fast-forward pull did not refresh status");
    require(QFile::exists(QDir(mine).filePath("remote.txt")) && checkouts == 3,
            "Pull did not refresh the worktree and notify the editor");

    put(QDir(peer).filePath("ignored.txt"), "remote version\n");
    git(peer, {"add", "ignored.txt"});
    git(peer, {"commit", "-m", "Add remote path"});
    git(peer, {"push"});
    put(QDir(mine).filePath(".git/info/exclude"), "ignored.txt\n");
    put(QDir(mine).filePath("ignored.txt"), "must preserve\n");
    const auto before = git(mine, {"rev-parse", "HEAD"});
    pull->trigger();
    waitUntil([&] { return pull->isEnabled() && summary->text().contains(QString::fromUtf8("↓1")); },
              "A rejected pull did not retain the behind count");
    QFile preserved(QDir(mine).filePath("ignored.txt"));
    require(preserved.open(QIODevice::ReadOnly) && preserved.readAll() == "must preserve\n" &&
            git(mine, {"rev-parse", "HEAD"}) == before && checkouts == 3,
            "Pull overwrote an ignored local file");

    git(mine, {"remote", "set-url", "origin", "https://someone:fixture@github.com/Some-Team/sample.git"});
    button("gitBrowse")->trigger();
    waitUntil([&] { return !github.isEmpty(); }, "GitHub browsing did not produce a repository URL");
    require(github == "https://github.com/Some-Team/sample", "GitHub browsing did not sanitize the origin URL");
    const auto actions = workspace.findChildren<QAction *>();
    bool checkedStatus = false;
    for (auto *action : actions) {
        if (action->text() == "GitHub sign-in status") {
            action->trigger();
            checkedStatus = true;
            break;
        }
    }
    require(checkedStatus && terminalProgram == "gh" &&
            terminalArgs == QStringList{"auth", "status", "--hostname", "github.com"},
            "GitHub sign-in status did not use the existing terminal and gh");
}
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        run();
        std::puts("PASS Git identity, branches, explicit sync, worktree safety and GitHub routing");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL Git integration: %s\n", error.what());
        return 1;
    }
}
