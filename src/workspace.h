#pragma once

#include <QWidget>
#include <functional>
#include <memory>

class QFileSystemModel;
class QFileIconProvider;
class QLabel;
class QLineEdit;
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
    void previewChange();
    void updateGitActions();
    void changeIndex(bool stage);
    void prepareCommit();
    void modifyRepository(QStringList arguments, const QString &successMessage);
    void openResult(QListWidgetItem *item);
    void cancel(QProcess *&process);
    void runGit(QProcess *&slot, const QString &cwd, QStringList arguments, int byteLimit,
                std::function<void(QByteArray, QString, bool)> done, bool stopOnLimit = true);

    QString directory_, gitRoot_;
    QStackedWidget *pages_;
    std::unique_ptr<QFileIconProvider> icons_;
    QFileSystemModel *model_;
    QTreeView *tree_;
    QLabel *folder_, *searchState_, *gitState_, *gitFeedback_;
    QLineEdit *query_;
    QListWidget *matches_, *changes_;
    QPlainTextEdit *diff_;
    QComboBox *diffSide_;
    QPushButton *refresh_, *stage_, *unstage_, *commit_;
    QProcess *searchProcess_ = nullptr, *statusProcess_ = nullptr, *diffProcess_ = nullptr,
             *mutationProcess_ = nullptr;
    bool gitReady_ = false, gitBusy_ = false;
};
