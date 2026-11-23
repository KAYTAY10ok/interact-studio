#include "workspace.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileIconProvider>
#include <QFileSystemModel>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
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

bool existsEntry(const QString &path) {
    const QFileInfo info(path);
    return info.exists() || info.isSymLink();
}

bool validEntryName(const QString &name) {
    if (name.trimmed().isEmpty() || name == "." || name == ".." ||
        name.compare(".git", Qt::CaseInsensitive) == 0 ||
        name.contains('/') || name.contains('\\') || name.contains(QChar::Null)) return false;
#ifdef Q_OS_WIN
    static const QRegularExpression reserved(
        QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9¹²³]|LPT[1-9¹²³])(?:\\.|$)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression invalid(QStringLiteral("[<>:\"|?*\\x00-\\x1f]"));
    if (name.endsWith(' ') || name.endsWith('.') || reserved.match(name).hasMatch() ||
        invalid.match(name).hasMatch()) return false;
#endif
    return true;
}

QMap<QString, QString> readIdentity(const QByteArray &output) {
    QMap<QString, QString> values;
    for (const auto &record : output.split('\0')) {
        const int newline = record.indexOf('\n');
        if (newline > 0)
            values.insert(QString::fromLatin1(record.left(newline)), QString::fromUtf8(record.mid(newline + 1)));
    }
    return values;
}

QStringList readGitLines(const QByteArray &output) {
    QStringList lines;
    for (const auto &line : output.split('\n')) {
        const auto value = QString::fromUtf8(line).trimmed();
        if (!value.isEmpty()) lines.append(value);
    }
    return lines;
}

QString githubWebUrl(QString remote) {
    const QString shorthand = "git@github.com:";
    if (remote.startsWith(shorthand, Qt::CaseInsensitive))
        remote = "ssh://git@github.com/" + remote.mid(shorthand.size());
    const QUrl url(remote, QUrl::StrictMode);
    if (!url.isValid() || url.host() != "github.com" ||
        (url.scheme() != "https" && url.scheme() != "http" && url.scheme() != "ssh")) return {};
    auto path = url.path(QUrl::FullyDecoded);
    if (path.endsWith(".git")) path.chop(4);
    static const QRegularExpression slug(QStringLiteral("^/([A-Za-z0-9-]+)/([A-Za-z0-9_.-]+)$"));
    const auto match = slug.match(path);
    if (!match.hasMatch() || match.captured(2) == "." || match.captured(2) == "..") return {};
    return "https://github.com" + path;
}

QString safeGitError(const QByteArray &errors) {
    QString message = QString::fromLocal8Bit(errors).trimmed();
    // Git and credential helpers sometimes repeat a URL from the repository config.
    static const QRegularExpression userInfo(QStringLiteral("([a-zA-Z][a-zA-Z0-9+.-]*://)[^\\s/@]+@"));
    static const QRegularExpression urlQuery(QStringLiteral("([a-zA-Z][a-zA-Z0-9+.-]*://[^\\s'\"]+)\\?[^\\s'\"]+"));
    message.replace(userInfo, QStringLiteral("\\1"));
    message.replace(urlQuery, QStringLiteral("\\1"));
    return message.left(350);
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
    folder_->setContextMenuPolicy(Qt::CustomContextMenu);
    files->addWidget(folder_);
    model_ = new QFileSystemModel(this);
    // The provider must outlive the model.
    icons_ = std::make_unique<FileIcons>();
    model_->setIconProvider(icons_.get());
    model_->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    tree_ = new QTreeView;
    tree_->setObjectName("explorerTree");
    tree_->setModel(model_);
    tree_->setHeaderHidden(true);
    tree_->setUniformRowHeights(true);
    tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree_->setIndentation(14);
    tree_->setIconSize(QSize(16,16));
    tree_->setSortingEnabled(true);
    tree_->sortByColumn(0, Qt::AscendingOrder);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    for (int i = 1; i < model_->columnCount(); ++i) tree_->hideColumn(i);
    files->addWidget(tree_);
    connect(tree_, &QTreeView::activated, this, [this](QModelIndex index) {
        if (onOpen && model_->fileInfo(index).isFile()) onOpen(model_->filePath(index));
    });
    connect(tree_, &QTreeView::customContextMenuRequested, this, [this](const QPoint &position) {
        const auto index = tree_->indexAt(position);
        if (index.isValid()) tree_->setCurrentIndex(index);
        showFileMenu(index.isValid() ? model_->filePath(index) : directory_,
                     tree_->viewport()->mapToGlobal(position));
    });
    connect(folder_, &QLabel::customContextMenuRequested, this, [this](const QPoint &position) {
        showFileMenu(directory_, folder_->mapToGlobal(position));
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
    auto *branchBar = new QHBoxLayout;
    gitBranch_ = new QLabel;
    gitBranch_->setObjectName("gitBranchSummary");
    gitBranch_->setTextFormat(Qt::PlainText);
    gitBranch_->setWordWrap(true);
    branchBar->addWidget(gitBranch_, 1);
    auto *more = new QToolButton;
    more->setText(tr("Git"));
    more->setAccessibleName(tr("Git actions"));
    more->setPopupMode(QToolButton::InstantPopup);
    auto *gitMenu = new QMenu(more);
    gitIdentity_ = gitMenu->addAction(tr("Repository identity…"), this, [this] { editIdentity(); });
    gitIdentity_->setObjectName("gitIdentity");
    gitSwitch_ = gitMenu->addAction(tr("Switch branch…"), this, [this] { switchBranch(false); });
    gitSwitch_->setObjectName("gitSwitch");
    gitCreate_ = gitMenu->addAction(tr("Create branch…"), this, [this] { switchBranch(true); });
    gitCreate_->setObjectName("gitCreate");
    gitMenu->addSeparator();
    gitFetch_ = gitMenu->addAction(tr("Fetch…"), this, [this] { synchronize(SyncOperation::Fetch); });
    gitFetch_->setObjectName("gitFetch");
    gitPull_ = gitMenu->addAction(tr("Pull (fast-forward only)…"), this, [this] { synchronize(SyncOperation::Pull); });
    gitPull_->setObjectName("gitPull");
    gitPush_ = gitMenu->addAction(tr("Push branch…"), this, [this] { synchronize(SyncOperation::Push); });
    gitPush_->setObjectName("gitPush");
    gitMenu->addSeparator();
    gitHubRepo_ = gitMenu->addAction(tr("Open GitHub repository"), this, [this] { browseGitHub(); });
    gitHubRepo_->setObjectName("gitBrowse");
    gitHubStatus_ = gitMenu->addAction(tr("GitHub sign-in status"), this, [this] {
        if (onGitCommand) onGitCommand("gh", {"auth", "status", "--hostname", "github.com"});
    });
    gitHubLogin_ = gitMenu->addAction(tr("Sign in to GitHub…"), this, [this] {
        if (onGitCommand) onGitCommand("gh", {"auth", "login", "--hostname", "github.com", "--web",
                                               "--git-protocol", "https"});
    });
    more->setMenu(gitMenu);
    branchBar->addWidget(more);
    git->addLayout(branchBar);
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
    if (process->state() == QProcess::NotRunning) {
        process->deleteLater();
    } else {
        // Let Qt reap a canceled reader after the widget stops using its output.
        process->setParent(QCoreApplication::instance());
        connect(process, QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),
                process, &QObject::deleteLater);
        connect(process, &QProcess::errorOccurred, process, [dying = process](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) dying->deleteLater();
        });
        process->kill();
    }
    process = nullptr;
}

void Workspace::setRoot(QString directory) {
    directory_ = QFileInfo(directory).canonicalFilePath();
    if (directory_.isEmpty()) directory_ = QDir(directory).absolutePath();
    cancel(searchProcess_);
    cancel(statusProcess_);
    cancel(diffProcess_);
    matches_->clear();
    searchState_->clear();
    changes_->clear();
    diff_->clear();
    gitRoot_.clear();
    readBranchSummary({});
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

bool Workspace::safeEntry(const QString &path, bool allowRoot) const {
    if (directory_.isEmpty() || path.isEmpty()) return false;
    const QString absolute = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    const QFileInfo info(absolute);
    // Reject links in any component, even when they resolve inside this workspace.
    if (!info.exists() || info.isSymLink() || info.canonicalFilePath() != absolute ||
        (!info.isDir() && !info.isFile())) return false;
    if (absolute == directory_) return allowRoot;
    const QString prefix = directory_.endsWith('/') ? directory_ : directory_ + '/';
    if (!absolute.startsWith(prefix)) return false;
    const QString relative = absolute.mid(prefix.size());
    for (const auto &component : relative.split('/'))
        if (component.compare(".git", Qt::CaseInsensitive) == 0) return false;
    return true;
}

void Workspace::showFileMenu(const QString &path, const QPoint &screenPosition) {
    if (directory_.isEmpty()) return;
    const QFileInfo info(path);
    const bool directory = info.isDir();
    const QString parent = directory ? path : info.absolutePath();
    const bool entryAllowed = safeEntry(path, true);
    const bool canCreate = !gitBusy_ && entryAllowed && safeEntry(parent, true);
    const bool canModify = !gitBusy_ && entryAllowed && path != directory_;

    auto *menu = new QMenu(tree_);
    menu->setObjectName("explorerContextMenu");
    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    auto add = [&](const QString &text, const QString &name, bool enabled, auto action) {
        auto *item = menu->addAction(text, this, action);
        item->setObjectName(name);
        item->setEnabled(enabled);
    };
    if (info.isFile()) {
        add(tr("Open in editor"), "explorerOpen", entryAllowed && bool(onOpen), [this, path] {
            if (onOpen && safeEntry(path)) onOpen(path);
        });
    }
    const auto suffix = info.suffix().toLower();
    if (directory || suffix == "html" || suffix == "htm") {
        add(tr("Open with Live Server"), "explorerLive", entryAllowed && bool(onLive), [this, path] {
            if (onLive && safeEntry(path, true)) onLive(path);
        });
    }
    if (!menu->isEmpty()) menu->addSeparator();
    add(tr("New file…"), "explorerNewFile", canCreate, [this, parent] { createEntry(parent, false); });
    add(tr("New folder…"), "explorerNewFolder", canCreate, [this, parent] { createEntry(parent, true); });
    add(tr("Rename…"), "explorerRename", canModify, [this, path] { renameEntry(path); });
    add(tr("Move to Trash…"), "explorerTrash", canModify, [this, path] { trashEntry(path); });
    menu->addSeparator();
    add(tr("Copy path"), "explorerCopyPath", true, [path] {
        QApplication::clipboard()->setText(QDir::toNativeSeparators(path));
    });
    menu->popup(screenPosition);
}

void Workspace::fileError(const QString &message) {
    QMessageBox box(QMessageBox::Warning, tr("Explorer"), message, QMessageBox::Ok, this);
    box.setTextFormat(Qt::PlainText);
    box.exec();
}

void Workspace::createEntry(const QString &parent, bool directory) {
    if (gitBusy_ || !safeEntry(parent, true) || !QFileInfo(parent).isDir()) return;
    bool accepted = false;
    const auto name = QInputDialog::getText(this, directory ? tr("New folder") : tr("New file"),
                                           tr("Name"), QLineEdit::Normal, {}, &accepted);
    if (!accepted) return;
    if (gitBusy_ || !safeEntry(parent, true) || !QFileInfo(parent).isDir()) {
        fileError(tr("The selected folder is unavailable."));
        return;
    }
    if (!validEntryName(name)) {
        fileError(tr("Enter a valid name within this folder."));
        return;
    }
    const QDir folder(parent);
    const QString target = folder.filePath(name);
    if (existsEntry(target)) {
        fileError(tr("That name is already in use."));
        return;
    }
    if (directory) {
        if (!folder.mkdir(name)) fileError(tr("Could not create %1").arg(target));
    } else {
        QFile file(target);
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            fileError(tr("Could not create %1\n%2").arg(target, file.errorString()));
            return;
        }
        file.close();
        if (onOpen) onOpen(target);
    }
}

void Workspace::renameEntry(const QString &path) {
    if (gitBusy_ || !safeEntry(path)) return;
    const QFileInfo info(path);
    bool accepted = false;
    const QString name = QInputDialog::getText(this, tr("Rename"), tr("New name"),
                                               QLineEdit::Normal, info.fileName(), &accepted);
    if (!accepted || name == info.fileName()) return;
    if (!validEntryName(name)) {
        fileError(tr("Enter a valid name within this folder."));
        return;
    }
    QDir folder = info.dir();
    const QString target = folder.filePath(name);
    if (existsEntry(target)) {
        fileError(tr("That name is already in use."));
        return;
    }
    if (beforeChange && !beforeChange(path)) return;
    if (gitBusy_ || !safeEntry(path) || !folder.rename(info.fileName(), name)) {
        fileError(tr("Could not rename %1").arg(path));
        return;
    }
    if (onRenamed) onRenamed(path, target);
}

void Workspace::trashEntry(const QString &path) {
    if (gitBusy_ || !safeEntry(path)) return;
    const QString name = QFileInfo(path).fileName();
    QMessageBox box(QMessageBox::Question, tr("Move to Trash"),
                    tr("Move \"%1\" to the Trash?").arg(name),
                    QMessageBox::Yes | QMessageBox::Cancel, this);
    box.setTextFormat(Qt::PlainText);
    box.setDefaultButton(QMessageBox::Cancel);
    if (box.exec() != QMessageBox::Yes || (beforeChange && !beforeChange(path))) return;
    if (gitBusy_ || !safeEntry(path)) {
        fileError(tr("The selected item is unavailable."));
        return;
    }
    QFile file(path);
    if (!file.moveToTrash()) {
        fileError(tr("Could not move %1 to the Trash.\n%2").arg(path, file.errorString()));
        return;
    }
    if (onRemoved) onRemoved(path);
}

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
                       std::function<void(QByteArray, QString, bool)> done, bool stopOnLimit,
                       QSet<int> acceptedCodes) {
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
    env.insert("LC_ALL", "C");
    p->setProcessEnvironment(env);
    connect(p, &QProcess::readyReadStandardOutput, this, drain);
    connect(p, &QProcess::readyReadStandardError, this, drainErrors);
    connect(p, QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished), this,
            [p, active, drain, drainErrors, output, errors, byteLimit, stopOnLimit, acceptedCodes, done]
            (int code, QProcess::ExitStatus status) {
        drain();
        drainErrors();
        const bool truncated = output->size() > byteLimit;
        QString error;
        if ((!truncated || !stopOnLimit) &&
            (status != QProcess::NormalExit || !acceptedCodes.contains(code) || (code != 0 && !errors->isEmpty()))) {
            error = safeGitError(*errors);
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
    readBranchSummary({});
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
           {"status", "--porcelain=v1", "--branch", "--ahead-behind", "--no-renames",
            "--untracked-files=all", "-z", "--", pathspec},
           256 * 1024, [this](QByteArray output, QString error, bool truncated) {
        if (!error.isEmpty()) { gitState_->setText(error); return; }
        int offset = 0;
        bool limited = truncated;
        while (offset < output.size()) {
            const int end = output.indexOf('\0', offset);
            if (end < 0) break;
            const QByteArray record = output.mid(offset, end - offset);
            offset = end + 1;
            if (record.startsWith("## ")) {
                readBranchSummary(record.mid(3));
                continue;
            }
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

void Workspace::readBranchSummary(const QByteArray &line) {
    branch_.clear();
    upstream_.clear();
    branchHasCommit_ = false;
    QString summary;
    if (line == "HEAD (no branch)") {
        summary = tr("Detached HEAD");
        branchHasCommit_ = true;
    } else if (!line.isEmpty()) {
        const QByteArray unborn("No commits yet on ");
        const QByteArray firstCommit("Initial commit on ");
        if (line.startsWith(unborn) || line.startsWith(firstCommit)) {
            branch_ = QString::fromUtf8(line.mid(line.startsWith(unborn) ? unborn.size() : firstCommit.size()));
            summary = tr("%1 · no commits").arg(branch_);
        } else {
            const int tracking = line.indexOf("...");
            branch_ = QString::fromUtf8(tracking < 0 ? line : line.left(tracking));
            branchHasCommit_ = true;
            summary = branch_;
            if (tracking >= 0) {
                const QByteArray trailing = line.mid(tracking + 3);
                const auto stats = trailing.indexOf(" [");
                upstream_ = QString::fromUtf8(stats < 0 ? trailing : trailing.left(stats));
                summary += tr(" → %1").arg(upstream_);
                if (stats >= 0) {
                    const QString counts = QString::fromLatin1(trailing.mid(stats + 2));
                    static const QRegularExpression ahead(QStringLiteral("ahead (\\d+)"));
                    static const QRegularExpression behind(QStringLiteral("behind (\\d+)"));
                    const auto a = ahead.match(counts);
                    const auto b = behind.match(counts);
                    if (a.hasMatch()) summary += tr(" ↑%1").arg(a.captured(1));
                    if (b.hasMatch()) summary += tr(" ↓%1").arg(b.captured(1));
                    if (counts == "gone]") summary += tr(" (gone)");
                }
            } else {
                summary += tr(" · no upstream");
            }
        }
    }
    gitBranch_->setText(summary);
}

void Workspace::updateGitActions() {
    const auto *item = changes_->currentItem();
    const QString state = item ? item->data(StatusRole).toString() : QString();
    static const QStringList conflicts{"DD", "AU", "UD", "UA", "DU", "AA", "UU"};
    const bool conflict = conflicts.contains(state);
    const bool editable = gitReady_ && !gitBusy_;
    const bool editablePath = editable && item && item->data(LiteralPathRole).isValid();
    gitIdentity_->setEnabled(editable);
    gitSwitch_->setEnabled(editable);
    gitCreate_->setEnabled(editable);
    gitFetch_->setEnabled(editable);
    gitPull_->setEnabled(editable && branchHasCommit_ && !branch_.isEmpty() && !upstream_.isEmpty());
    gitPush_->setEnabled(editable && branchHasCommit_ && !branch_.isEmpty());
    gitHubRepo_->setEnabled(editable && bool(onGitHub));
    gitHubStatus_->setEnabled(editable && bool(onGitCommand));
    gitHubLogin_->setEnabled(editable && bool(onGitCommand));
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

void Workspace::modifyRepository(QStringList arguments, const QString &successMessage, bool changesFiles) {
    gitBusy_ = true;
    updateGitActions();
    gitFeedback_->setText(tr("Running Git…"));
    gitFeedback_->show();
    const QString workspace = directory_;
    runGit(mutationProcess_, gitRoot_, std::move(arguments), 8192,
           [this, workspace, successMessage, changesFiles](QByteArray, QString error, bool) {
        gitBusy_ = false;
        if (directory_ == workspace) {
            setGitFeedback(error.isEmpty() ? successMessage : error);
            if (error.isEmpty() && changesFiles && onCheckout) onCheckout();
        }
        refreshGit();
    }, false);
}

void Workspace::setGitFeedback(const QString &message) {
    gitFeedback_->setText(message);
    gitFeedback_->setVisible(!message.isEmpty());
}

void Workspace::finishGitAction(const QString &feedback, bool refresh) {
    gitBusy_ = false;
    setGitFeedback(feedback);
    if (refresh || !gitReady_) refreshGit();
    else updateGitActions();
}

void Workspace::editIdentity() {
    if (!gitIdentity_->isEnabled()) return;
    gitBusy_ = true;
    updateGitActions();
    const auto directory = directory_;
    const QStringList query{"--null", "--get-regexp", "^user\\.(name|email)$"};
    const auto available = [this, directory](bool truncated, const QString &error) {
        if (directory != directory_ || !gitReady_) { finishGitAction(); return false; }
        if (!error.isEmpty() || truncated) {
            finishGitAction(error.isEmpty() ? tr("Git identity settings exceed the display limit.") : error);
            return false;
        }
        return true;
    };
    runGit(mutationProcess_, gitRoot_, QStringList{"config"} + query, 32 * 1024,
           [this, directory, available, query](QByteArray output, QString error, bool truncated) {
        if (!available(truncated, error)) return;
        const auto effective = readIdentity(output);
        runGit(mutationProcess_, gitRoot_,
               QStringList{"config", "--local", "--no-includes"} + query, 32 * 1024,
               [this, directory, available, effective](QByteArray bytes, QString localError, bool limited) {
            if (!available(limited, localError)) return;
            const auto local = readIdentity(bytes);
            QDialog dialog(this);
            dialog.setObjectName("gitIdentityDialog");
            dialog.setWindowTitle(tr("Repository identity"));
            auto *layout = new QFormLayout(&dialog);
            auto *description = new QLabel(tr("Only this repository is changed. Clear a field to remove its local override."));
            description->setWordWrap(true);
            layout->addRow(description);
            auto *name = new QLineEdit(local.value("user.name"));
            auto *email = new QLineEdit(local.value("user.email"));
            name->setObjectName("gitIdentityName");
            email->setObjectName("gitIdentityEmail");
            name->setMaxLength(256);
            email->setMaxLength(256);
            layout->addRow(tr("Local name"), name);
            layout->addRow(tr("Local email"), email);
            auto addEffective = [&](const QString &label, const QString &key) {
                auto *value = new QLabel(effective.value(key, tr("Not set")));
                value->setTextFormat(Qt::PlainText);
                value->setWordWrap(true);
                value->setTextInteractionFlags(Qt::TextSelectableByMouse);
                layout->addRow(label, value);
            };
            addEffective(tr("Current effective name"), "user.name");
            addEffective(tr("Current effective email"), "user.email");
            auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
            layout->addRow(buttons);
            connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
            connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            dialog.resize(460, dialog.sizeHint().height());
            if (dialog.exec() != QDialog::Accepted || directory_ != directory) {
                finishGitAction();
                return;
            }
            QList<QStringList> commands;
            auto collect = [&](const QString &key, const QString &value) {
                const auto entered = value.trimmed();
                if (entered.isEmpty()) {
                    if (local.contains(key)) commands.append({"config", "--local", "--unset-all", key});
                } else if (entered != local.value(key)) {
                    commands.append({"config", "--local", "--replace-all", key, entered});
                }
            };
            collect("user.name", name->text());
            collect("user.email", email->text());
            if (commands.isEmpty()) finishGitAction();
            else writeIdentity(commands);
        }, true, {0, 1});
    }, true, {0, 1});
}

void Workspace::writeIdentity(const QList<QStringList> &commands, int index) {
    const auto directory = directory_;
    const auto arguments = commands.at(index);
    runGit(mutationProcess_, gitRoot_, arguments, 4096,
           [this, directory, commands, index](QByteArray, QString error, bool) {
        if (directory_ != directory) { finishGitAction(); return; }
        if (!error.isEmpty()) {
            const auto message = index > 0 ? tr("Identity partly updated. %1").arg(error) : error;
            finishGitAction(message);
        } else if (index + 1 < commands.size()) {
            writeIdentity(commands, index + 1);
        } else {
            finishGitAction(tr("Repository identity updated"));
        }
    }, false, arguments.contains("--unset-all") ? QSet<int>{0, 5} : QSet<int>{0});
}

void Workspace::switchBranch(bool create) {
    if (!(create ? gitCreate_ : gitSwitch_)->isEnabled()) return;
    gitBusy_ = true;
    updateGitActions();
    const auto directory = directory_;
    auto change = [this, create, directory](const QString &branch) {
        if (branch.isEmpty() || directory != directory_ || !gitReady_ ||
            (!create && branch == branch_) || (beforeCheckout && !beforeCheckout())) {
            finishGitAction();
            return;
        }
        QStringList arguments{"switch", "--no-guess", "--no-merge", "--no-overwrite-ignore", "--no-recurse-submodules"};
        if (create) arguments << "-c";
        else arguments << "--";
        arguments << branch;
        modifyRepository(arguments, tr("Switched to %1").arg(branch), true);
    };
    if (create) {
        bool ok = false;
        const auto branch = QInputDialog::getText(this, tr("Create branch"), tr("New branch name"),
                                                   QLineEdit::Normal, {}, &ok).trimmed();
        if (ok && (branch.startsWith('-') || branch.contains("@{"))) {
            finishGitAction(tr("Enter a branch name, not a Git option or revision shorthand."));
        } else {
            change(ok ? branch : QString());
        }
        return;
    }
    runGit(mutationProcess_, gitRoot_, {"for-each-ref", "--format=%(refname:lstrip=2)", "refs/heads/"}, 64 * 1024,
           [this, directory, change](QByteArray output, QString error, bool truncated) {
        if (directory != directory_) { finishGitAction(); return; }
        if (truncated || !error.isEmpty()) {
            finishGitAction(error.isEmpty() ? tr("Too many local branches. Use the terminal to select one.") : error);
            return;
        }
        const auto branches = readGitLines(output);
        if (branches.isEmpty()) { finishGitAction(tr("This repository has no local branches yet.")); return; }
        bool ok = false;
        const auto branch = QInputDialog::getItem(this, tr("Switch branch"), tr("Local branch"), branches,
                                                 qMax(0, branches.indexOf(branch_)), false, &ok);
        change(ok ? branch : QString());
    });
}

void Workspace::selectRemote(const QString &preferred, std::function<void(QString)> ready) {
    if (!preferred.isEmpty() && preferred != ".") {
        ready(preferred);
        return;
    }
    const auto directory = directory_;
    runGit(mutationProcess_, gitRoot_, {"remote"}, 32 * 1024,
           [this, directory, ready](QByteArray output, QString error, bool truncated) {
        if (directory_ != directory) { finishGitAction(); return; }
        if (!error.isEmpty() || truncated) {
            finishGitAction(error.isEmpty() ? tr("Too many remotes. Use the terminal to select one.") : error);
            return;
        }
        const auto remotes = readGitLines(output);
        if (remotes.isEmpty()) { finishGitAction(tr("No Git remote is configured.")); return; }
        if (remotes.size() == 1) { ready(remotes.first()); return; }
        bool ok = false;
        const auto remote = QInputDialog::getItem(this, tr("Select remote"), tr("Remote"), remotes,
                                                  qMax(0, remotes.indexOf("origin")), false, &ok);
        if (!ok || directory != directory_) { finishGitAction(); return; }
        ready(remote);
    });
}

void Workspace::fastForward(const QString &expectedBranch, const QString &ref) {
    const auto directory = directory_;
    runGit(mutationProcess_, gitRoot_, {"symbolic-ref", "--short", "HEAD"}, 8192,
           [this, directory, expectedBranch, ref](QByteArray output, QString error, bool truncated) {
        if (directory_ != directory) { finishGitAction(); return; }
        if (truncated || !error.isEmpty() || QString::fromUtf8(output).trimmed() != expectedBranch) {
            finishGitAction(tr("The checked-out branch changed. Pull again after refreshing."), true);
            return;
        }
        if (beforeCheckout && !beforeCheckout()) { finishGitAction({}, true); return; }
        // This fast-forward uses the fetched upstream ref and protects ignored
        // files as well as working changes. Git pull does not expose that option.
        modifyRepository({"merge", "--ff-only", "--no-autostash", "--no-overwrite-ignore", "--", ref},
                         tr("Pull completed"), true);
    });
}

void Workspace::synchronize(SyncOperation operation) {
    QAction *action = operation == SyncOperation::Fetch ? gitFetch_ :
                      operation == SyncOperation::Pull ? gitPull_ : gitPush_;
    if (!action->isEnabled()) return;
    gitBusy_ = true;
    updateGitActions();
    const auto directory = directory_;
    const auto branch = branch_;
    // Arguments do not use shell interpolation. The fully qualified ref
    // prevents tag ambiguity and push configuration from selecting extra refs.
    const QString fullRef = "refs/heads/" + branch;
    runGit(mutationProcess_, gitRoot_, {"for-each-ref",
           "--format=%(refname)%00%(upstream:remotename)%00%(upstream:remoteref)%00%(upstream)",
           "--", fullRef}, 16 * 1024,
           [this, operation, directory, branch, fullRef](QByteArray output, QString error, bool truncated) {
        if (directory_ != directory || !gitReady_) { finishGitAction(); return; }
        if (!error.isEmpty() || truncated) {
            finishGitAction(error.isEmpty() ? tr("Could not read branch tracking settings.") : error);
            return;
        }
        const auto tracking = output.trimmed().split('\0');
        const bool localBranch = tracking.size() == 4 && QString::fromUtf8(tracking.first()) == fullRef;
        const auto preferred = localBranch ? QString::fromUtf8(tracking.at(1)) : QString();
        const auto remoteRef = localBranch ? QString::fromUtf8(tracking.at(2)) : QString();
        const auto upstreamRef = localBranch ? QString::fromUtf8(tracking.at(3)) : QString();
        QStringList fetch{"fetch", "--no-tags", "--no-prune", "--no-prune-tags", "--no-recurse-submodules", "--"};
        if (operation == SyncOperation::Fetch) {
            selectRemote(preferred, [this, fetch](const QString &remote) {
                modifyRepository(fetch + QStringList{remote}, tr("Fetch completed"));
            });
            return;
        }
        if (!localBranch) { finishGitAction(tr("Refresh to select an existing branch."), true); return; }
        if (operation == SyncOperation::Pull) {
            if (preferred.isEmpty() || !upstreamRef.startsWith("refs/")) {
                finishGitAction(tr("This branch has no configured upstream."));
                return;
            }
            if (preferred == ".") { fastForward(branch, upstreamRef); return; }
            setGitFeedback(tr("Fetching %1…").arg(preferred));
            runGit(mutationProcess_, gitRoot_, fetch + QStringList{preferred}, 8192,
                   [this, directory, branch, upstreamRef](QByteArray, QString error, bool) {
                if (directory_ != directory) { finishGitAction(); return; }
                if (!error.isEmpty()) finishGitAction(error, true);
                else fastForward(branch, upstreamRef);
            }, false);
            return;
        }
        // Support normal branch targets only. An upstream in this same repository
        // is not rewritten by an action named Push branch.
        if (preferred == "." || (!remoteRef.isEmpty() && !remoteRef.startsWith("refs/heads/"))) {
            finishGitAction(tr("This upstream is not a branch on a configured remote. Use the terminal."));
            return;
        }
        selectRemote(preferred, [this, directory, branch, fullRef, remoteRef](const QString &remote) {
            if (remote.contains('=')) {
                finishGitAction(tr("Use the terminal to push to this unusually named remote."));
                return;
            }
            const auto target = remoteRef.isEmpty() ? fullRef : remoteRef;
            QMessageBox confirm(QMessageBox::Question, tr("Push branch"),
                tr("Push %1 to %2/%3?").arg(branch, remote, target.mid(11)),
                QMessageBox::Yes | QMessageBox::Cancel, this);
            confirm.setTextFormat(Qt::PlainText);
            confirm.setDefaultButton(QMessageBox::Cancel);
            if (confirm.exec() != QMessageBox::Yes || directory != directory_) {
                finishGitAction();
                return;
            }
            QStringList args{"-c", "remote." + remote + ".mirror=false", "push",
                             "--no-mirror", "--no-force", "--no-force-with-lease", "--no-force-if-includes",
                             "--no-follow-tags", "--no-prune", "--recurse-submodules=no"};
            if (remoteRef.isEmpty()) args << "--set-upstream";
            args << "--" << remote << fullRef + ':' + target;
            modifyRepository(args, tr("Push completed"));
        });
    });
}

void Workspace::browseGitHub() {
    if (!gitHubRepo_->isEnabled()) return;
    gitBusy_ = true;
    updateGitActions();
    selectRemote({}, [this](const QString &remote) {
        const auto directory = directory_;
        runGit(mutationProcess_, gitRoot_, {"config", "--get", "remote." + remote + ".url"}, 16 * 1024,
               [this, directory](QByteArray output, QString error, bool truncated) {
            if (directory_ != directory) { finishGitAction(); return; }
            const auto url = githubWebUrl(QString::fromUtf8(output).trimmed());
            if (truncated || !error.isEmpty() || url.isEmpty()) {
                finishGitAction(tr("This remote does not identify a github.com repository."));
                return;
            }
            finishGitAction();
            if (onGitHub) onGitHub(url);
        });
    });
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
