#pragma once

#include <QWidget>
#include <QSet>
#include <functional>
#include <memory>

class QFileSystemModel;
class QFileIconProvider;
class QLabel;
class QLineEdit;
class QAction;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QProcess;
class QPushButton;
class QComboBox;
class QStackedWidget;
class QTreeView;

class Workspace : public QWidget {
public:
    explicit Workspace(QWidget *parent = nullptr);
    ~Workspace() override;
    void setRoot(QString directory);
    QString root() const { return directory_; }
    bool gitOperationInProgress() const { return gitBusy_; }
    void showFiles();
    void showSearch();
    void showGit();
    std::function<void(QString)> onOpen;
    std::function<void(QString, int)> onOpenLine;
    std::function<void(QString)> onLive;
    std::function<bool(QString)> beforeChange;
    std::function<void(QString, QString)> onRenamed;
    std::function<void(QString)> onRemoved;
    std::function<bool()> beforeCheckout;
    std::function<void()> onCheckout;
    std::function<void(QString)> onGitHub;
    std::function<void(QString, QStringList)> onGitCommand;

private:
    void showFileMenu(const QString &path, const QPoint &screenPosition);
    bool safeEntry(const QString &path, bool allowRoot = false) const;
    void createEntry(const QString &parent, bool directory);
    void renameEntry(const QString &path);
    void trashEntry(const QString &path);
    void fileError(const QString &message);
    void search();
    void refreshGit();
    void readStatus();
    void readBranchSummary(const QByteArray &line);
    void setGitFeedback(const QString &message);
    void previewChange();
    void updateGitActions();
    void changeIndex(bool stage);
    void prepareCommit();
    void modifyRepository(QStringList arguments, const QString &successMessage, bool changesFiles = false);
    void editIdentity();
    void writeIdentity(const QList<QStringList> &commands, int index = 0);
    void switchBranch(bool create);
    enum class SyncOperation { Fetch, Pull, Push };
    void synchronize(SyncOperation operation);
    void fastForward(const QString &expectedBranch, const QString &ref);
    void selectRemote(const QString &preferred, std::function<void(QString)> ready);
    void browseGitHub();
    void finishGitAction(const QString &feedback = {}, bool refresh = false);
    void openResult(QListWidgetItem *item);
    void cancel(QProcess *&process);
    void runGit(QProcess *&slot, const QString &cwd, QStringList arguments, int byteLimit,
                std::function<void(QByteArray, QString, bool)> done, bool stopOnLimit = true,
                QSet<int> acceptedCodes = {0});

    QString directory_, gitRoot_, branch_, upstream_;
    QStackedWidget *pages_;
    std::unique_ptr<QFileIconProvider> icons_;
    QFileSystemModel *model_;
    QTreeView *tree_;
    QLabel *folder_, *searchState_, *gitState_, *gitFeedback_, *gitBranch_;
    QLineEdit *query_;
    QListWidget *matches_, *changes_;
    QPlainTextEdit *diff_;
    QComboBox *diffSide_;
    QPushButton *refresh_, *stage_, *unstage_, *commit_;
    QAction *gitIdentity_, *gitSwitch_, *gitCreate_, *gitFetch_, *gitPull_, *gitPush_,
            *gitHubRepo_, *gitHubStatus_, *gitHubLogin_;
    QProcess *searchProcess_ = nullptr, *statusProcess_ = nullptr, *diffProcess_ = nullptr,
             *mutationProcess_ = nullptr;
    bool gitReady_ = false, gitBusy_ = false, branchHasCommit_ = false;
};
