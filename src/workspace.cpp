#include "workspace.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileIconProvider>
#include <QFileSystemModel>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QTreeView>
#include <QVBoxLayout>
#include <memory>

namespace {
constexpr int MaxMatches = 500;
constexpr int PathRole = Qt::UserRole;
constexpr int LineRole = Qt::UserRole + 1;
constexpr int StatusRole = Qt::UserRole + 2;
constexpr int LiteralPathRole = Qt::UserRole + 3;

QVBoxLayout *pageLayout(QStackedWidget *pages, const QString &title) {
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(9, 10, 9, 6);
    layout->setSpacing(7);
    auto *caption = new QLabel(title);
    QFont font = caption->font();
    font.setBold(true);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 1.2);
    caption->setFont(font);
    layout->addWidget(caption);
    pages->addWidget(page);
    return layout;
}

class FileIcons final : public QFileIconProvider {
public:
    FileIcons()
        : folder_(draw("#d6ae76", true)), plain_(draw("#9aa8bb")),
          cpp_(draw("#8faedc")), rust_(draw("#dd9878")),
          script_(draw("#d5bd75")), text_(draw("#7ebaaa")) {}

    QIcon icon(const QFileInfo &info) const override {
        if (info.isDir()) return folder_;
        const QString ext = info.suffix().toLower();
        if (ext == "cpp" || ext == "cc" || ext == "h" || ext == "hpp") return cpp_;
        if (ext == "rs" || ext == "toml") return rust_;
        if (ext == "json" || ext == "js" || ext == "py") return script_;
        if (ext == "md" || ext == "txt") return text_;
        return plain_;
    }
private:
    const QIcon folder_, plain_, cpp_, rust_, script_, text_;
    static QIcon draw(QColor color, bool folder = false) {
        QPixmap pix(16, 16);
        pix.fill(Qt::transparent);
        QPainter p(&pix);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(color, 1.15));
        auto fill = color;
        fill.setAlpha(33);
        p.setBrush(fill);
        if (folder) {
            p.drawPolygon(QPolygonF({{1.5,4}, {6,4}, {7.5,5.5}, {14.5,5.5}, {14.5,12.5}, {1.5,12.5}}));
        } else {
            p.drawPolygon(QPolygonF({{3.5,1.5}, {9,1.5}, {12.5,5}, {12.5,14.5}, {3.5,14.5}}));
            p.drawLine(QPointF(6,9), QPointF(10,9));
        }
        p.end();
        return QIcon(pix);
    }
};

QString jsonText(const QJsonObject &field) {
    if (field.contains("text")) return field.value("text").toString();
    return QString::fromUtf8(QByteArray::fromBase64(field.value("bytes").toString().toLatin1()));
}
}

Workspace::Workspace(QWidget *parent) : QWidget(parent) {
    pages_ = new QStackedWidget(this);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(pages_);

    auto *files = pageLayout(pages_, tr("EXPLORER"));
    folder_ = new QLabel;
    folder_->setTextFormat(Qt::PlainText);
    files->addWidget(folder_);
    model_ = new QFileSystemModel(this);
    // The provider must outlive the model.
    icons_ = std::make_unique<FileIcons>();
    model_->setIconProvider(icons_.get());
    model_->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    tree_ = new QTreeView;
    tree_->setModel(model_);
    tree_->setHeaderHidden(true);
    tree_->setUniformRowHeights(true);
    tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree_->setIndentation(14);
    tree_->setIconSize(QSize(16,16));
    tree_->setSortingEnabled(true);
    tree_->sortByColumn(0, Qt::AscendingOrder);
    for (int i = 1; i < model_->columnCount(); ++i) tree_->hideColumn(i);
    files->addWidget(tree_);
    connect(tree_, &QTreeView::activated, this, [this](QModelIndex index) {
        if (onOpen && model_->fileInfo(index).isFile()) onOpen(model_->filePath(index));
    });

    auto *searchLayout = pageLayout(pages_, tr("PROJECT SEARCH"));
    query_ = new QLineEdit;
    query_->setPlaceholderText(tr("Find text in files…"));
    query_->setClearButtonEnabled(true);
    query_->setAccessibleName(tr("Find text in project"));
    searchLayout->addWidget(query_);
    auto *submit = new QPushButton(tr("Search"));
    searchLayout->addWidget(submit);
    searchState_ = new QLabel;
    searchState_->setWordWrap(true);
    searchState_->setTextFormat(Qt::PlainText);
    searchLayout->addWidget(searchState_);
    matches_ = new QListWidget;
    matches_->setUniformItemSizes(true);
    searchLayout->addWidget(matches_);
    connect(submit, &QPushButton::clicked, this, [this] { search(); });
    connect(query_, &QLineEdit::returnPressed, this, [this] { search(); });
    connect(matches_, &QListWidget::itemActivated, this, [this](auto *item) { openResult(item); });

    auto *git = pageLayout(pages_, tr("SOURCE CONTROL"));
    auto *bar = new QHBoxLayout;
    gitState_ = new QLabel;
    gitState_->setTextFormat(Qt::PlainText);
    gitState_->setWordWrap(true);
    bar->addWidget(gitState_, 1);
    refresh_ = new QPushButton(tr("Refresh"));
    bar->addWidget(refresh_);
    git->addLayout(bar);
    auto *indexActions = new QHBoxLayout;
    stage_ = new QPushButton(tr("Stage"));
    stage_->setObjectName("gitStage");
    stage_->setToolTip(tr("Stage the selected file"));
    unstage_ = new QPushButton(tr("Unstage"));
    unstage_->setObjectName("gitUnstage");
    unstage_->setToolTip(tr("Remove the selected file from the index"));
    commit_ = new QPushButton(tr("Commit…"));
    commit_->setObjectName("gitCommit");
    commit_->setToolTip(tr("Review and commit all staged files in the repository"));
    for (auto *button : {stage_, unstage_, commit_}) indexActions->addWidget(button);
    git->addLayout(indexActions);
    gitFeedback_ = new QLabel;
    gitFeedback_->setWordWrap(true);
    gitFeedback_->setTextFormat(Qt::PlainText);
    gitFeedback_->hide();
    git->addWidget(gitFeedback_);
    auto *split = new QSplitter(Qt::Vertical);
    changes_ = new QListWidget;
    changes_->setUniformItemSizes(true);
    split->addWidget(changes_);
    auto *preview = new QWidget;
    auto *previewLayout = new QVBoxLayout(preview);
    previewLayout->setContentsMargins(0, 3, 0, 0);
    diffSide_ = new QComboBox;
    diffSide_->addItems({tr("Working tree"), tr("Staged")});
    previewLayout->addWidget(diffSide_);
    diff_ = new QPlainTextEdit;
    diff_->setReadOnly(true);
    diff_->setLineWrapMode(QPlainTextEdit::NoWrap);
    diff_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    previewLayout->addWidget(diff_);
    split->addWidget(preview);
    split->setSizes({220, 300});
    git->addWidget(split);
    connect(refresh_, &QPushButton::clicked, this, [this] {
        gitFeedback_->hide();
        refreshGit();
    });
    connect(stage_, &QPushButton::clicked, this, [this] { changeIndex(true); });
    connect(unstage_, &QPushButton::clicked, this, [this] { changeIndex(false); });
    connect(commit_, &QPushButton::clicked, this, [this] { prepareCommit(); });
    connect(changes_, &QListWidget::currentItemChanged, this, [this](auto *item) {
        const QString state = item ? item->data(StatusRole).toString() : QString();
        { QSignalBlocker block(diffSide_); diffSide_->setCurrentIndex(state.size() == 2 && state[1] == ' ' ? 1 : 0); }
        updateGitActions();
        previewChange();
    });
    connect(diffSide_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { previewChange(); });
    connect(changes_, &QListWidget::itemActivated, this, [this](auto *item) {
        const QString path = item->data(PathRole).toString();
        if (onOpen && QFileInfo(path).isFile()) onOpen(path);
    });
    updateGitActions();
}

Workspace::~Workspace() {
    delete model_;
    cancel(searchProcess_);
    cancel(statusProcess_);
    cancel(diffProcess_);
    // A mutating process may have already updated the index. Allow it to
    // finish normally while the window is open.
    if (mutationProcess_) mutationProcess_->disconnect(this);
}

void Workspace::cancel(QProcess *&process) {
    if (!process) return;
    process->disconnect(this);
    if (process->state() != QProcess::NotRunning) process->kill();
    process->deleteLater();
    process = nullptr;
}

void Workspace::setRoot(QString directory) {
    directory_ = QDir(directory).absolutePath();
    cancel(searchProcess_);
    cancel(statusProcess_);
    cancel(diffProcess_);
    matches_->clear();
    searchState_->clear();
    changes_->clear();
    diff_->clear();
    gitRoot_.clear();
    gitReady_ = false;
    gitFeedback_->hide();
    updateGitActions();
    folder_->setText(QFileInfo(directory_).fileName().isEmpty() ? directory_ : QFileInfo(directory_).fileName());
    folder_->setToolTip(directory_);
    tree_->setRootIndex(model_->setRootPath(directory_));
    if (pages_->currentIndex() == 2) refreshGit();
}

void Workspace::showFiles() { pages_->setCurrentIndex(0); }
void Workspace::showSearch() { pages_->setCurrentIndex(1); query_->setFocus(); query_->selectAll(); }
void Workspace::showGit() { pages_->setCurrentIndex(2); refreshGit(); }

void Workspace::openResult(QListWidgetItem *item) {
    if (!item) return;
    const QString path = item->data(PathRole).toString();
    if (onOpenLine) onOpenLine(path, item->data(LineRole).toInt());
    else if (onOpen) onOpen(path);
}

void Workspace::search() {
    cancel(searchProcess_);
    matches_->clear();
    searchState_->clear();
    if (directory_.isEmpty() || query_->text().isEmpty()) return;
    auto *p = new QProcess(this);
    searchProcess_ = p;
    p->setWorkingDirectory(directory_);
    searchState_->setText(tr("Searching…"));
    auto drain = [this, p] {
        while (p->canReadLine()) {
            const auto event = QJsonDocument::fromJson(p->readLine()).object();
            if (event.value("type").toString() != "match") continue;
            if (matches_->count() == MaxMatches) {
                searchState_->setText(tr("First %1 matching lines").arg(MaxMatches));
                cancel(searchProcess_);
                return;
            }
            const auto data = event.value("data").toObject();
            const auto path = QDir::cleanPath(jsonText(data.value("path").toObject()));
            const int line = data.value("line_number").toInt();
            const auto snippet = jsonText(data.value("lines").toObject()).trimmed().left(160);
            auto *item = new QListWidgetItem(QString("%1:%2\n%3").arg(path).arg(line).arg(snippet), matches_);
            item->setToolTip(path);
            item->setData(PathRole, QDir(directory_).absoluteFilePath(path));
            item->setData(LineRole, line);
        }
    };
    connect(p, &QProcess::readyReadStandardOutput, this, drain);
    connect(p, QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, p, drain](int code, QProcess::ExitStatus status) {
        drain();
        if (searchProcess_ != p) return;
        if (status == QProcess::NormalExit && (code == 0 || code == 1))
            searchState_->setText(tr("%1 matching lines").arg(matches_->count()));
        else
            searchState_->setText(QString::fromLocal8Bit(p->readAllStandardError()).trimmed().left(300));
        cancel(searchProcess_);
    });
    connect(p, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            searchState_->setText(tr("Install ripgrep (rg) to search project files."));
            cancel(searchProcess_);
        }
    });
    p->start("rg", {"--no-config", "--json", "--line-number", "--fixed-strings",
                    "--smart-case", "--max-filesize", "4M", "--", query_->text(), "."});
}

void Workspace::runGit(QProcess *&slot, const QString &cwd, QStringList arguments, int byteLimit,
                       std::function<void(QByteArray, QString, bool)> done, bool stopOnLimit) {
    cancel(slot);
    auto *p = new QProcess(this);
    slot = p;
    auto *active = &slot;
    auto output = std::make_shared<QByteArray>();
    auto errors = std::make_shared<QByteArray>();
    auto drain = [p, output, byteLimit, stopOnLimit] {
        const auto bytes = p->readAllStandardOutput();
        output->append(bytes.constData(), qMin(bytes.size(), byteLimit + 1 - output->size()));
        if (stopOnLimit && output->size() > byteLimit && p->state() != QProcess::NotRunning) p->kill();
    };
    auto drainErrors = [p, errors] {
        constexpr int errorLimit = 4096;
        const auto bytes = p->readAllStandardError();
        errors->append(bytes.constData(), qMin(bytes.size(), errorLimit - errors->size()));
    };
    p->setWorkingDirectory(cwd);
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("GIT_OPTIONAL_LOCKS", "0");
    env.insert("GIT_TERMINAL_PROMPT", "0");
    p->setProcessEnvironment(env);
    connect(p, &QProcess::readyReadStandardOutput, this, drain);
    connect(p, &QProcess::readyReadStandardError, this, drainErrors);
    connect(p, QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished), this,
            [p, active, drain, drainErrors, output, errors, byteLimit, stopOnLimit, done](int code, QProcess::ExitStatus status) {
        drain();
        drainErrors();
        const bool truncated = output->size() > byteLimit;
        QString error;
        if ((!truncated || !stopOnLimit) && (status != QProcess::NormalExit || code != 0)) {
            error = QString::fromLocal8Bit(*errors).trimmed().left(350);
            if (error.isEmpty()) error = tr("Git exited with status %1").arg(code);
        }
        *active = nullptr;
        p->deleteLater();
        done(output->left(byteLimit), error, truncated);
    });
    connect(p, &QProcess::errorOccurred, this, [p, active, done](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        *active = nullptr;
        p->deleteLater();
        done({}, tr("Could not start Git. Check that git is installed."), false);
    });
    arguments.prepend("--literal-pathspecs");
    p->start("git", arguments);
    p->closeWriteChannel();
}

void Workspace::refreshGit() {
    updateGitActions();
    if (gitBusy_) return;
    gitReady_ = false;
    updateGitActions();
    cancel(diffProcess_);
    changes_->clear();
    diff_->clear();
    gitRoot_.clear();
    if (directory_.isEmpty()) return;
    gitState_->setText(tr("Reading changes…"));
    runGit(statusProcess_, directory_, {"rev-parse", "--show-toplevel"}, 65536,
           [this](QByteArray output, QString error, bool truncated) {
        if (!error.isEmpty() || truncated) {
            gitState_->setText(error.isEmpty() ? tr("Could not read repository path") : error);
            return;
        }
        if (output.endsWith('\n')) output.chop(1);
        gitRoot_ = QDir::cleanPath(QFile::decodeName(output));
        readStatus();
    });
}

void Workspace::readStatus() {
    const auto pathspec = QDir(gitRoot_).relativeFilePath(directory_);
    runGit(statusProcess_, gitRoot_,
           {"status", "--porcelain=v1", "--no-renames", "--untracked-files=all", "-z", "--", pathspec},
           256 * 1024, [this](QByteArray output, QString error, bool truncated) {
        if (!error.isEmpty()) { gitState_->setText(error); return; }
        int offset = 0;
        bool limited = truncated;
        while (offset < output.size()) {
            const int end = output.indexOf('\0', offset);
            if (end < 0) break;
            const QByteArray record = output.mid(offset, end - offset);
            offset = end + 1;
            if (record.size() <= 3) continue;
            if (changes_->count() == 1000) { limited = true; break; }
            const QString state = QString::fromLatin1(record.left(2));
            const auto bytes = record.mid(3);
            const QString path = QFile::decodeName(bytes);
            auto *item = new QListWidgetItem(state + "   " + path, changes_);
            item->setToolTip(path);
            item->setData(StatusRole, state);
            item->setData(PathRole, QDir(gitRoot_).filePath(path));
            if (QFile::encodeName(path) == bytes) item->setData(LiteralPathRole, path);
        }
        gitState_->setText(limited ? tr("Showing first %1 changes").arg(changes_->count())
                                   : changes_->count() ? tr("%1 changed files").arg(changes_->count())
                                                       : tr("Working tree clean"));
        gitReady_ = true;
        if (changes_->count()) changes_->setCurrentRow(0);
        updateGitActions();
    });
}

void Workspace::updateGitActions() {
    const auto *item = changes_->currentItem();
    const QString state = item ? item->data(StatusRole).toString() : QString();
    static const QStringList conflicts{"DD", "AU", "UD", "UA", "DU", "AA", "UU"};
    const bool conflict = conflicts.contains(state);
    const bool editable = gitReady_ && !gitBusy_;
    const bool editablePath = editable && item && item->data(LiteralPathRole).isValid();
    refresh_->setEnabled(!gitBusy_);
    commit_->setEnabled(editable);
    stage_->setEnabled(editablePath && state.size() == 2 &&
                       (state == "??" || state.at(1) != ' ' || conflict));
    unstage_->setEnabled(editablePath && state.size() == 2 && !conflict &&
                         state.at(0) != ' ' && state != "??");
}

void Workspace::changeIndex(bool stage) {
    if (!(stage ? stage_ : unstage_)->isEnabled()) return;
    const QString path = changes_->currentItem()->data(LiteralPathRole).toString();
    if (stage) modifyRepository({"add", "--", path}, tr("File staged"));
    else modifyRepository({"reset", "--quiet", "--", path}, tr("File unstaged"));
}

void Workspace::modifyRepository(QStringList arguments, const QString &successMessage) {
    gitBusy_ = true;
    updateGitActions();
    gitFeedback_->setText(tr("Running Git…"));
    gitFeedback_->show();
    const QString workspace = directory_;
    runGit(mutationProcess_, gitRoot_, std::move(arguments), 8192,
           [this, workspace, successMessage](QByteArray, QString error, bool) {
        gitBusy_ = false;
        if (directory_ == workspace) {
            gitFeedback_->setText(error.isEmpty() ? successMessage : error);
            gitFeedback_->show();
        }
        refreshGit();
    }, false);
}

void Workspace::prepareCommit() {
    if (!commit_->isEnabled()) return;
    gitBusy_ = true;
    updateGitActions();
    gitFeedback_->hide();
    const QString workspace = directory_;
    runGit(mutationProcess_, gitRoot_, {"diff", "--cached", "--no-ext-diff", "--no-textconv",
           "--name-only", "--no-renames", "-z"}, 256 * 1024,
           [this, workspace](QByteArray output, QString error, bool truncated) {
        if (directory_ != workspace || !gitReady_) {
            gitBusy_ = false;
            refreshGit();
            return;
        }
        if (truncated) error = tr("There are too many staged paths to review here. Commit from the terminal.");
        if (error.isEmpty() && output.isEmpty()) error = tr("Stage changes before committing.");
        if (!error.isEmpty()) {
            gitBusy_ = false;
            updateGitActions();
            gitFeedback_->setText(error);
            gitFeedback_->show();
            return;
        }

        QDialog dialog(this);
        dialog.setObjectName("gitCommitDialog");
        dialog.setWindowTitle(tr("Commit staged changes"));
        dialog.resize(440, 350);
        auto *layout = new QVBoxLayout(&dialog);
        auto *caption = new QLabel(tr("Commit all staged files in this repository."));
        caption->setWordWrap(true);
        layout->addWidget(caption);
        auto *files = new QListWidget;
        files->setObjectName("gitCommitPaths");
        for (const auto &path : output.split('\0'))
            if (!path.isEmpty()) files->addItem(QFile::decodeName(path));
        layout->addWidget(files, 1);
        auto *message = new QPlainTextEdit;
        message->setObjectName("gitCommitMessage");
        message->setPlaceholderText(tr("Commit message"));
        layout->addWidget(message, 1);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
        auto *submit = buttons->addButton(tr("Commit"), QDialogButtonBox::AcceptRole);
        submit->setEnabled(false);
        layout->addWidget(buttons);
        connect(message, &QPlainTextEdit::textChanged, &dialog, [message, submit] {
            submit->setEnabled(!message->toPlainText().trimmed().isEmpty());
        });
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        message->setFocus();
        if (dialog.exec() == QDialog::Accepted && directory_ == workspace && gitReady_) {
            const auto description = message->toPlainText().trimmed();
            if (!description.isEmpty()) {
                modifyRepository({"commit", "-m", description}, tr("Commit created"));
                return;
            }
        }
        gitBusy_ = false;
        if (directory_ == workspace && gitReady_) updateGitActions();
        else refreshGit();
    });
}

void Workspace::previewChange() {
    cancel(diffProcess_);
    diff_->clear();
    auto *item = changes_->currentItem();
    if (!item) return;
    if (!item->data(LiteralPathRole).isValid()) {
        diff_->setPlainText(tr("This filename cannot be represented in the current system encoding."));
        return;
    }
    if (item->data(StatusRole).toString() == "??") {
        diff_->setPlainText(diffSide_->currentIndex() == 1 ? tr("No staged changes")
                                                           : tr("Untracked file. Open to view its contents."));
        return;
    }
    QStringList args{"diff", "--no-ext-diff", "--no-textconv", "--no-color"};
    if (diffSide_->currentIndex() == 1) args << "--cached";
    args << "--" << item->data(LiteralPathRole).toString();
    runGit(diffProcess_, gitRoot_, args, 96 * 1024,
           [this](QByteArray output, QString error, bool truncated) {
        QString text = QString::fromUtf8(output);
        if (truncated) text += tr("\n… Diff preview truncated");
        diff_->setPlainText(!error.isEmpty() ? error : text.isEmpty() ? tr("No changes in this view") : text);
    });
}
