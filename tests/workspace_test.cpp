#include "quickopen.h"
#include "workspace.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QTreeView>
#include <cstdio>
#include <stdexcept>

namespace {
void require(bool result, const char *message) {
    if (!result) throw std::runtime_error(message);
}

void waitUntil(std::function<bool()> condition, const char *failure) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < 6000) {
        qApp->processEvents();
        QThread::msleep(5);
    }
    require(condition(), failure);
}

void writeFile(const QString &path, const QByteArray &data) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(data) == data.size(), "Cannot write fixture");
}

QByteArray git(const QDir &project, QStringList args) {
    QProcess process;
    process.setWorkingDirectory(project.path());
    process.start("git", args);
    require(process.waitForFinished() && process.exitStatus() == QProcess::NormalExit &&
            process.exitCode() == 0, "Git fixture command failed");
    return process.readAllStandardOutput();
}

void run() {
    QTemporaryDir fixture;
    require(fixture.isValid(), "Cannot create fixture");
    qputenv("GIT_CEILING_DIRECTORIES", QFile::encodeName(QFileInfo(fixture.path()).absolutePath()));
    const QDir project(fixture.path() + "/project");
    require(QDir().mkpath(project.filePath("src")), "Cannot create source directory");
    const QString source = project.filePath("src/thing.txt");
    writeFile(source, "main\nindex\n");
    require(QProcess::execute("git", {"-C", project.path(), "init", "-q"}) == 0, "git init failed");
    require(QProcess::execute("git", {"-C", project.path(), "add", "src/thing.txt"}) == 0, "git add failed");
    writeFile(source, "main\nspectrum\n");
    writeFile(project.filePath(".gitignore"), "ignored/\n");
    require(project.mkdir("ignored"), "Cannot create ignored directory");
    writeFile(project.filePath("ignored/thing.txt"), "spectrum\n");

    Workspace workspace;
    workspace.setRoot(project.filePath("src"));
    workspace.show();
    auto *tree = workspace.findChild<QTreeView *>();
    waitUntil([&] { return tree->model()->rowCount(tree->rootIndex()) == 1; }, "Explorer did not list source");
    workspace.showSearch();
    auto *query = workspace.findChild<QLineEdit *>();
    query->setText("spectrum");
    query->returnPressed();
    QListWidget *matches = nullptr;
    waitUntil([&] {
        for (auto *list : workspace.findChildren<QListWidget *>()) {
            if (list->count() && list->item(0)->text().contains("spectrum")) {
                matches = list;
                return true;
            }
        }
        return false;
    }, "Search did not find text");
    QString hit;
    int line = -1;
    workspace.onOpenLine = [&](QString path, int n) { hit = path; line = n; };
    matches->itemActivated(matches->item(0));
    require(hit == source && line == 2, "Search navigated to the wrong file or line");

    workspace.showGit();
    auto *diff = workspace.findChild<QPlainTextEdit *>();
    waitUntil([&] { return diff->toPlainText().contains("+spectrum"); }, "Missing working tree diff");
    workspace.findChild<QComboBox *>()->setCurrentIndex(1);
    waitUntil([&] { return diff->toPlainText().contains("+index"); }, "Missing staged diff");

    QString opened;
    quickOpenFile(&workspace, project.path(), [&](QString path) { opened = path; });
    auto *picker = workspace.findChild<QDialog *>();
    require(picker, "Could not open file picker");
    auto *results = picker->findChild<QListWidget *>();
    auto *field = picker->findChild<QLineEdit *>();
    field->setText("srgxt");
    waitUntil([&] {
        return results->count() == 1 && results->item(0)->text() == "src/thing.txt";
    }, "Quick open did not fuzzy-match relative path");
    field->setText("thng");
    waitUntil([&] {
        return results->count() == 1 && results->item(0)->text() == "src/thing.txt";
    }, "Quick open did not respect Git exclusions");
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(field, &enter);
    require(opened == source, "Quick open failed to open the keyboard selection");

    auto *stage = workspace.findChild<QPushButton *>("gitStage");
    auto *unstage = workspace.findChild<QPushButton *>("gitUnstage");
    auto *commit = workspace.findChild<QPushButton *>("gitCommit");
    require(stage && unstage && commit && unstage->isEnabled(), "Missing source control actions");
    unstage->click();
    waitUntil([&] { return stage->isEnabled() && !unstage->isEnabled(); }, "Unstage did not refresh status");
    require(git(project, {"ls-files"}).isEmpty(), "Unstage left an entry in the initial index");
    QFile content(source);
    require(content.open(QIODevice::ReadOnly) && content.readAll() == "main\nspectrum\n",
            "Unstage changed the working file");
    content.close();

    const QString literal = "src/[xy].txt";
    writeFile(project.filePath(literal), "literal\n");
    writeFile(project.filePath("src/x.txt"), "other\n");
    workspace.showGit();
    QListWidgetItem *literalItem = nullptr;
    QListWidget *gitList = nullptr;
    waitUntil([&] {
        for (auto *list : workspace.findChildren<QListWidget *>())
            for (int i = 0; i < list->count(); ++i)
                if (list->item(i)->toolTip() == literal) {
                    literalItem = list->item(i);
                    gitList = list;
                    return true;
                }
        return false;
    }, "Status did not list the path containing brackets");
    gitList->setCurrentItem(literalItem);
    require(stage->isEnabled(), "Stage unavailable for untracked file");
    stage->click();
    waitUntil([&] { return commit->isEnabled(); }, "Stage did not refresh status");
    require(git(project, {"ls-files", "-z"}) == QByteArray("src/[xy].txt\0", 13),
            "Stage treated brackets as a path pattern");

    git(project, {"add", ".gitignore"});
    git(project, {"config", "--local", "user.name", "Studio Tests"});
    git(project, {"config", "--local", "user.email", "studio@example.invalid"});
    git(project, {"config", "--local", "commit.gpgsign", "false"});
    bool reviewedFullIndex = false;
    QTimer review;
    review.setInterval(5);
    QObject::connect(&review, &QTimer::timeout, &workspace, [&] {
        auto *dialog = workspace.findChild<QDialog *>("gitCommitDialog");
        if (!dialog) return;
        review.stop();
        auto *paths = dialog->findChild<QListWidget *>("gitCommitPaths");
        auto *message = dialog->findChild<QPlainTextEdit *>("gitCommitMessage");
        auto *buttons = dialog->findChild<QDialogButtonBox *>();
        reviewedFullIndex = paths && paths->count() == 2 &&
                !paths->findItems(".gitignore", Qt::MatchExactly).isEmpty() &&
                !paths->findItems(literal, Qt::MatchExactly).isEmpty();
        if (!reviewedFullIndex || !message || !buttons) { dialog->reject(); return; }
        message->setPlainText("Verify staged files");
        for (auto *button : buttons->buttons())
            if (buttons->buttonRole(button) == QDialogButtonBox::AcceptRole) {
                button->click();
                return;
            }
        dialog->reject();
    });
    review.start();
    commit->click();
    waitUntil([&] { return commit->isEnabled(); }, "Commit did not refresh status");
    require(reviewedFullIndex, "Commit omitted staged files outside the workspace subdirectory");
    require(git(project, {"log", "-1", "--format=%s"}) == "Verify staged files\n" &&
            git(project, {"ls-tree", "--name-only", "-r", "HEAD"}) == ".gitignore\nsrc/[xy].txt\n",
            "Commit did not record the reviewed index");

    workspace.setRoot(fixture.path());
    waitUntil([&] {
        for (auto *label : workspace.findChildren<QLabel *>())
            if (label->text().contains("not a git repository")) return true;
        return false;
    }, "Missing non-repository status");
}
}

int main(int argc, char **argv) {
    qputenv("LC_ALL", "C");
    qputenv("GIT_CONFIG_GLOBAL", "/dev/null");
    QApplication app(argc, argv);
    try {
        run();
        puts("PASS workspace: file tree, search navigation, quick open, Git diffs, literal staging, unstaging and commit");
    } catch (const std::exception &e) {
        fprintf(stderr, "FAIL workspace: %s\n", e.what());
        return 1;
    }
}
