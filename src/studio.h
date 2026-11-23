#pragma once
#include <QMainWindow>
#include <QFont>
#include <QHash>
#include <functional>
#include <memory>

class Customize;
class Editor;
class LiveServer;
class Terminal;
class Workspace;
class QAction;
class QDockWidget;
class QLabel;
class QPlainTextEdit;
class QToolButton;
class QSplitter;
class QMenu;
class QWidget;

class StudioWindow final : public QMainWindow {
public:
    explicit StudioWindow(QString root);
    ~StudioWindow() override;
    bool openFile(QString path);
    Editor* editor() const { return tabs; }

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    QString project, settingsKey;
    Editor *tabs;
    Workspace *navigator;
    Terminal *terminal = nullptr;
    LiveServer *live = nullptr;
    QDockWidget *navDock, *termDock;
    QSplitter *splitter;
    QPlainTextEdit *second = nullptr;
    QWidget *header;
    QToolButton *projectButton, *paletteButton, *liveButton;
    QLabel *location, *position, *language, *logo;
    QMenu *extensionMenu;
    QHash<QString, QAction*> actions;
    QHash<QString, QToolButton*> rail;
    QString sidebarMode = "view.files";
    QColor accent;
    QFont codeFont;
    std::unique_ptr<Customize> customize;

    void buildChrome();
    void buildActions();
    void selectSidebar(const QString& mode);
    Terminal* showTerminal();
    void toggleSplit();
    void syncState();
    void applyAppearance(QFont font, QColor color);
    void setProject(QString path);
    void chooseProject();
    void rememberWorkspace();
    void showPalette();
    void runTask();
    void runCurrentFile();
    void startLive(QString path = {});
    void stopLive();
    void updateLiveControls();
    void navigateTo(QString path, int line, int column);
    void findDefinition();
    void saveLayout();
    void loadLayout();
    void restoreLayoutDefaults();
    QAction* command(QString id, QString title, QString shortcut, std::function<void()> fn);
};
