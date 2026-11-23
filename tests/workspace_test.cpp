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
#include <QFileSystemModel>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
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

void testExplorer() {
    QTemporaryDir fixture;
    require(fixture.isValid(), "Cannot create explorer fixture");
    const QString root = fixture.filePath("workspace");
    require(QDir().mkdir(root), "Cannot create explorer workspace");
    qputenv("XDG_DATA_HOME", QFile::encodeName(fixture.filePath("data")));
    require(QDir().mkdir(fixture.filePath("data")), "Cannot isolate fixture Trash");

    Workspace workspace;
    workspace.resize(320, 300);
    workspace.setRoot(root);
    workspace.show();
    auto *tree = workspace.findChild<QTreeView *>("explorerTree");
    require(tree, "Cannot find explorer tree");
    auto *model = qobject_cast<QFileSystemModel *>(tree->model());
    require(model, "Cannot find explorer model");
    waitUntil([&] { return tree->rootIndex().isValid(); }, "Explorer did not load root");

    auto menuFor = [&](const QString &path) {
        QPoint point(2, tree->viewport()->height() - 4);
        if (!path.isEmpty()) {
            QModelIndex index;
            waitUntil([&] { index = model->index(path); return index.isValid(); }, "Explorer did not list path");
            tree->scrollTo(index);
            point = tree->visualRect(index).center();
            require(tree->indexAt(point) == index, "Explorer item not visible for context click");
        }
        require(QMetaObject::invokeMethod(tree, "customContextMenuRequested", Q_ARG(QPoint, point)),
                "Cannot request explorer context menu");
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        require(menu && menu->objectName() == "explorerContextMenu", "Missing explorer context menu");
        return QPointer<QMenu>(menu);
    };
    auto action = [](QMenu *menu, const char *name) {
        auto *item = menu->findChild<QAction *>(name);
        require(item, "Missing explorer action");
        return item;
    };
    auto enterName = [&](const QString &value) {
        QTimer::singleShot(0, &workspace, [value] {
            if (auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget())) {
                dialog->setTextValue(value);
                dialog->accept();
            }
        });
    };
    auto click = [&](QPointer<QMenu> menu, const char *name) {
        auto *item = action(menu, name);
        require(item->isEnabled(), "Explorer action unexpectedly disabled");
        item->trigger();
        if (menu) menu->close();
    };

    auto menu = menuFor({});
    require(!action(menu, "explorerRename")->isEnabled() && !action(menu, "explorerTrash")->isEnabled(),
            "Workspace root enabled destructive actions");
    enterName("preview files");
    click(menu, "explorerNewFolder");
    const auto folder = root + "/preview files";
    require(QFileInfo(folder).isDir(), "Explorer did not create the folder");
    menu = menuFor(folder);
    QString opened;
    workspace.onOpen = [&](QString path) { opened = path; };
    enterName("site page.html");
    click(menu, "explorerNewFile");
    const auto page = folder + "/site page.html";
    require(QFileInfo(page).isFile() && opened == page, "New file did not open in the editor");
    tree->expand(model->index(folder));
    QString live;
    workspace.onLive = [&](QString path) { live = path; };
    click(menuFor(page), "explorerLive");
    require(live == page, "Live server received the wrong file");

    QString renamedFrom, renamedTo, guarded;
    bool allowMutation = false;
    workspace.beforeChange = [&](QString path) { guarded = path; return allowMutation; };
    workspace.onRenamed = [&](QString from, QString to) { renamedFrom = from; renamedTo = to; };
    const auto renamed = folder + "/index.html";
    menu = menuFor(page);
    enterName("index.html");
    click(menu, "explorerRename");
    require(guarded == page && renamedFrom.isEmpty() && !QFileInfo::exists(renamed) && QFileInfo::exists(page),
            "Rename ignored the editor veto");
    allowMutation = true;
    menu = menuFor(page);
    enterName("index.html");
    click(menu, "explorerRename");
    require(renamedFrom == page && renamedTo == renamed && QFileInfo::exists(renamed) && !QFileInfo::exists(page),
            "Rename did not move the file and notify the editor");

    const auto outside = fixture.filePath("outside.html");
    const auto link = root + "/outside-link.html";
    writeFile(outside, "outside");
    require(QFile::link(outside, link), "Unable to seed explorer symlink");
    menu = menuFor(link);
    require(!action(menu, "explorerLive")->isEnabled() && !action(menu, "explorerRename")->isEnabled() &&
                !action(menu, "explorerTrash")->isEnabled(),
            "Explorer actions allow a symlink escape");
    menu->close();

    QString removed;
    workspace.onRemoved = [&](QString path) { removed = path; };
    menu = menuFor(renamed);
    QTimer::singleShot(0, &workspace, [] {
        if (auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
            dialog->button(QMessageBox::Yes)->click();
    });
    click(menu, "explorerTrash");
    require(removed == renamed && !QFileInfo::exists(renamed) && QFileInfo::exists(outside),
            "Trash did not remove only the selected file and notify the editor");
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
        testExplorer();
        puts("PASS workspace: file tree context actions, search navigation, quick open, Git diffs, literal staging, unstaging and commit");
    } catch (const std::exception &e) {
        fprintf(stderr, "FAIL workspace: %s\n", e.what());
        return 1;
    }
}
