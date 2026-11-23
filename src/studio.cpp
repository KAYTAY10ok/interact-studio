#include "studio.h"
#include "editor.h"
#include "workspace.h"
#include "terminal.h"
#include "customize.h"
#include "icons.h"
#include "quickopen.h"
#include "buildrun.h"
#include "definition.h"
#include "languages.h"
#include "liveserver.h"
#include <algorithm>
#include <QtWidgets>
#include <QCryptographicHash>

namespace {
constexpr int layoutVersion = 1;

class StudioStyle final : public QProxyStyle {
public:
    StudioStyle() : QProxyStyle(QStringLiteral("Fusion")) {}

    void drawPrimitive(PrimitiveElement element, const QStyleOption *option,
                       QPainter *painter, const QWidget *widget) const override {
        if (element != PE_IndicatorTabClose) {
            QProxyStyle::drawPrimitive(element,option,painter,widget);
            return;
        }
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const QPointF center = option->rect.center();
        const bool hovered = option->state & State_MouseOver;
        if (hovered) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor("#444a52"));
            painter->drawRoundedRect(QRectF(center-QPointF(7,7),QSizeF(14,14)),3,3);
        }
        painter->setPen(QPen(hovered ? QColor("#f4f3ef") : QColor("#a2aab0"),
                             1.15,Qt::SolidLine,Qt::RoundCap));
        painter->drawLine(center-QPointF(3,3),center+QPointF(3,3));
        painter->drawLine(center-QPointF(3,-3),center+QPointF(3,-3));
        painter->restore();
    }
};

QString projectName(const QString& path) {
    QString name = QDir(path).dirName();
    return name.isEmpty() ? path : name;
}
QString keyFor(const QString& root) {
    return "workspace/" + QCryptographicHash::hash(root.toUtf8(),QCryptographicHash::Sha256).toHex() + "/";
}
}

StudioWindow::StudioWindow(QString root) : accent("#e0ae69") {
    if (!dynamic_cast<StudioStyle*>(QApplication::style()))
        QApplication::setStyle(new StudioStyle);
    resize(1200,790);
    setMinimumSize(640,400);
    setDockNestingEnabled(true);
    setWindowIcon(studioIcon(Glyph::Mark,accent));
    customize = std::make_unique<Customize>(this);
    tabs = new Editor;
    splitter = new QSplitter(Qt::Horizontal);
    splitter->addWidget(tabs);
    splitter->setChildrenCollapsible(false);
    setCentralWidget(splitter);

    navigator = new Workspace;
    navDock = new QDockWidget(tr("Workspace"),this);
    navDock->setObjectName("navigatorDock");
    navDock->setWidget(navigator);
    navDock->setMinimumWidth(180);
    addDockWidget(Qt::LeftDockWidgetArea,navDock);
    termDock = new QDockWidget(tr("Console"),this);
    termDock->setObjectName("terminalDock");
    addDockWidget(Qt::BottomDockWidgetArea,termDock);
    termDock->hide();
    buildActions();
    buildChrome();
    restoreLayoutDefaults();
    QSettings config;
    restoreGeometry(config.value("window/geometry").toByteArray());
    bool restored = restoreState(config.value("window/docks").toByteArray(), layoutVersion);
    setProject(std::move(root));
    if (restored && !termDock->isHidden()) showTerminal();

    navigator->onOpen = [this](QString path){openFile(path);};
    navigator->onLive = [this](QString path){startLive(path);};
    navigator->beforeChange = [this](QString path){
        if (tabs->canChangePath(path)) return true;
        QMessageBox::information(this,tr("Unsaved files"),
            tr("Save or close the affected editors before renaming or removing this item."));
        return false;
    };
    navigator->onRenamed = [this](QString oldPath,QString newPath){tabs->pathRenamed(oldPath,newPath);};
    navigator->onRemoved = [this](QString path){tabs->pathRemoved(path);};
    navigator->onOpenLine = [this](QString path, int line){
        navigateTo(path, line - 1, 0);
    };
    customize->onAppearance = [this](QFont font,QColor color){applyAppearance(font,color);};
    customize->onRun = [this](QString program,QStringList args){showTerminal()->launchCommand(program,args);};
    customize->onExtensionsChanged = [this]{
        extensionMenu->clear();
        for (auto *action : customize->extensionActions()) extensionMenu->addAction(action);
        if (extensionMenu->isEmpty()) extensionMenu->addAction(tr("No command extensions installed"))->setEnabled(false);
        extensionMenu->addSeparator();
        extensionMenu->addAction(actions["extensions.reload"]);
    };
    customize->reload();
    tabs->onStateChanged = [this]{syncState();};
    const auto session = config.value(settingsKey+"openFiles").toStringList();
    QString last = config.value(settingsKey+"activeFile", QDir(project).filePath("README.md")).toString();
    qint64 remaining = 4 * 1024 * 1024;
    auto tryRestore = [&](const QString& path, qint64 budget) -> qint64 {
        QFileInfo file(path);
        if (!file.isFile() || !file.isReadable() || file.size() > budget ||
                file.size() > 2 * 1024 * 1024) return 0;
        return tabs->openFile(path,false) ? file.size() : 0;
    };
    const auto eager = session.mid(0,12);
    qint64 activeBudget = qBound<qint64>(0, QFileInfo(last).size(), 2 * 1024 * 1024);
    for (const auto& path : eager) {
        if (path == last) activeBudget = 0;
        remaining -= tryRestore(path,remaining-activeBudget);
    }
    if (eager.contains(last)) {
        // Re-select the matching buffer without moving its tab.
        tryRestore(last,2 * 1024 * 1024);
    } else tryRestore(last,remaining);
    if (!tabs->count()) tabs->newFile();
    if (config.value("window/split",false).toBool()) toggleSplit();
    splitter->restoreState(config.value("window/editorSizes").toByteArray());
    syncState();
}

StudioWindow::~StudioWindow() {
    tabs->onStateChanged = {};
    // The dock and its children outlive C++ member fields during QWidget teardown.
    for (auto *object : findChildren<QObject*>()) disconnect(object,nullptr,this,nullptr);
}

QAction* StudioWindow::command(QString id,QString title,QString shortcut,std::function<void()> fn) {
    auto *a = new QAction(title,this);
    a->setObjectName(id);
    addAction(a);
    connect(a,&QAction::triggered,this,[fn]{fn();});
    actions.insert(id,a);
    customize->bind(id,a,shortcut);
    return a;
}

void StudioWindow::buildActions() {
    command("file.new",tr("New file"),"Ctrl+N",[this]{tabs->newFile();});
    command("file.open",tr("Open file…"),"Ctrl+O",[this]{
        const auto paths = QFileDialog::getOpenFileNames(this,tr("Open files"),project);
        for (const auto& p : paths) openFile(p);
    });
    command("file.quickOpen",tr("Quick open file…"),"Ctrl+P",[this]{quickOpenFile(this,project,[this](QString p){openFile(p);});});
    command("edit.complete",tr("Show suggestions"),"Ctrl+Space",[this]{tabs->completeWord();});
    command("file.folder",tr("Open workspace…"),"Ctrl+Shift+O",[this]{chooseProject();});
    command("file.save",tr("Save file"),"Ctrl+S",[this]{tabs->save();});
    command("file.saveAs",tr("Save as…"),"Ctrl+Shift+S",[this]{tabs->saveAs();});
    command("file.close",tr("Close editor"),"Ctrl+W",[this]{tabs->closeCurrent();});
    command("app.quit",tr("Close window"),"Ctrl+Q",[this]{close();});
    command("edit.find",tr("Find in file"),"Ctrl+F",[this]{tabs->findText();});
    command("edit.goto",tr("Go to line…"),"Ctrl+G",[this]{tabs->goToLine();});
    command("edit.definition",tr("Go to definition"),"F12",[this]{findDefinition();});
    command("view.files",tr("Explorer"),"Ctrl+Shift+E",[this]{selectSidebar("view.files");});
    command("view.search",tr("Search workspace"),"Ctrl+Shift+F",[this]{selectSidebar("view.search");});
    command("view.git",tr("Source control"),"Ctrl+Shift+G",[this]{selectSidebar("view.git");});
    command("view.sidebar",tr("Toggle sidebar"),"Ctrl+B",[this]{navDock->setVisible(navDock->isHidden());});
    command("view.split",tr("Split editor"),"Ctrl+\\",[this]{toggleSplit();});
    command("view.reset",tr("Restore default layout"),"",[this]{restoreLayoutDefaults();});
    command("view.saveLayout",tr("Save named layout…"),"",[this]{saveLayout();});
    command("view.loadLayout",tr("Load named layout…"),"",[this]{loadLayout();});
    command("terminal.local",tr("New local terminal"),"Ctrl+Shift+`",[this]{showTerminal()->launchShell();});
    command("terminal.toggle",tr("Toggle terminal"),"Ctrl+`",[this]{
        if (!termDock->isHidden()) termDock->hide();
        else if (!terminal) showTerminal()->launchShell();
        else showTerminal();
    });
    command("connect.github",tr("GitHub repository details (gh)"),"",[this]{showTerminal()->launchGitHub();});
    command("connect.remoteRefs",tr("List remote branches and tags"),"",[this]{
        showTerminal()->launchCommand("git",{"ls-remote","--heads","--tags","origin"});
    });
    command("connect.docker",tr("Docker container…"),"",[this]{showTerminal()->launchDocker();});
    command("connect.wsl",tr("WSL shell"),"",[this]{showTerminal()->launchWsl();});
    command("connect.ssh",tr("SSH host or VM…"),"",[this]{showTerminal()->launchSsh();});
    command("task.run",tr("Run workspace task…"),"Ctrl+Return",[this]{runTask();});
    command("task.runFile",tr("Build and run current file"),"F5",[this]{runCurrentFile();});
    command("live.start",tr("Open with Live Server"),"",[this]{startLive();});
    command("live.copyUrl",tr("Copy Live Server URL"),"",[this]{
        if (live && live->isRunning()) {
            QApplication::clipboard()->setText(live->url().toString(QUrl::FullyEncoded));
            statusBar()->showMessage(tr("Live Server URL copied"),2200);
        }
    })->setEnabled(false);
    command("live.stop",tr("Stop Live Server"),"",[this]{stopLive();})->setEnabled(false);
    command("app.palette",tr("Command palette"),"Ctrl+Shift+P",[this]{showPalette();});
    command("app.preferences",tr("Preferences…"),"Ctrl+,",[this]{customize->configure();});
    command("extensions.reload",tr("Reload command extensions"),"",[this]{
        customize->reload(); statusBar()->showMessage(tr("Extension commands reloaded"),2200);
    });
}

void StudioWindow::buildChrome() {
    header = new QWidget;
    header->setObjectName("topbar");
    auto *top = new QHBoxLayout(header);
    top->setContentsMargins(11,0,10,0); top->setSpacing(7);
    logo = new QLabel;
    logo->setPixmap(studioIcon(Glyph::Mark,accent).pixmap(23,23)); top->addWidget(logo);
    auto *brand = new QLabel("Interact Studio"); brand->setObjectName("brand"); top->addWidget(brand);
    top->addSpacing(8);
    auto *menus = new QMenuBar;
    menus->setNativeMenuBar(false);
    auto menu = [&](QString title,QStringList ids){
        auto *m = menus->addMenu(title);
        for (const auto& id : ids) if (id.isEmpty()) m->addSeparator(); else m->addAction(actions[id]);
        return m;
    };
    menu(tr("&File"),{"file.new","file.quickOpen","file.open","file.folder","","file.save","file.saveAs","file.close","","app.quit"});
    menu(tr("&Edit"),{"edit.find","edit.goto","edit.definition","edit.complete","","app.preferences"});
    menu(tr("&View"),{"app.palette","view.files","view.search","view.git","","view.split","view.sidebar","terminal.toggle","","view.saveLayout","view.loadLayout","view.reset"});
    menu(tr("&Run"),{"task.runFile","task.run","live.start","live.copyUrl","live.stop","","terminal.local","connect.remoteRefs","connect.github","connect.docker","connect.wsl","connect.ssh"});
    extensionMenu = menus->addMenu(tr("E&xtensions")); top->addWidget(menus);
    top->addStretch();
    paletteButton = new QToolButton;
    paletteButton->setObjectName("paletteButton");
    actions["app.palette"]->setIcon(studioIcon(Glyph::Command));
    paletteButton->setDefaultAction(actions["app.palette"]);
    paletteButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    top->addWidget(paletteButton);
    header->setFixedHeight(43);
    setMenuWidget(header);

    auto *bar = new QToolBar(tr("Activity"));
    bar->setObjectName("activityRail");
    bar->setMovable(false);
    bar->setFloatable(false);
    bar->setOrientation(Qt::Vertical);
    bar->setFixedWidth(46);
    addToolBar(Qt::LeftToolBarArea,bar);
    auto railButton = [&](QString id, Glyph glyph, bool tab = false) {
        auto *b = new QToolButton;
        b->setObjectName("railItem");
        actions[id]->setIcon(studioIcon(glyph));
        b->setDefaultAction(actions[id]);
        b->setAccessibleName(actions[id]->text());
        b->setIconSize(QSize(22,22));
        b->setFixedSize(44,43);
        b->setProperty("selected",tab && id==sidebarMode);
        rail[id]=b;
        bar->addWidget(b);
        return b;
    };
    railButton("view.files",Glyph::Files,true); railButton("view.search",Glyph::Search,true);
    railButton("view.git",Glyph::Branch,true); railButton("terminal.toggle",Glyph::Console);
    auto *plug = new QToolButton;
    plug->setObjectName("railItem");
    plug->setFixedSize(44,43); plug->setIcon(studioIcon(Glyph::Plug)); plug->setIconSize(QSize(22,22));
    plug->setAccessibleName(tr("Connections")); plug->setToolTip(tr("Connections"));
    auto *connections = new QMenu(plug);
    for (const auto& id : {"connect.ssh","connect.docker","connect.wsl","connect.remoteRefs","connect.github"})
        connections->addAction(actions[id]);
    plug->setMenu(connections); plug->setPopupMode(QToolButton::InstantPopup); bar->addWidget(plug);
    auto *spring = new QWidget; spring->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Expanding); bar->addWidget(spring);
    railButton("app.preferences",Glyph::Settings);

    // A compact project strip sits above the editors even when both docks are hidden.
    auto *strip = new QToolBar(tr("Editor tools"));
    strip->setObjectName("projectStrip"); strip->setMovable(false);
    addToolBar(Qt::TopToolBarArea,strip);
    projectButton = new QToolButton;
    projectButton->setAccessibleName(tr("Open workspace"));
    connect(projectButton,&QToolButton::clicked,actions["file.folder"],&QAction::trigger);
    projectButton->setIcon(studioIcon(Glyph::Folder,accent));
    projectButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    strip->addWidget(projectButton);
    location = new QLabel; location->setObjectName("location"); strip->addWidget(location);
    auto *spacer = new QWidget; spacer->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);
    strip->addWidget(spacer);
    for (auto pair : {qMakePair(QString("task.run"),Glyph::Play),qMakePair(QString("view.split"),Glyph::Split)}) {
        auto *button = new QToolButton;
        actions[pair.first]->setIcon(studioIcon(pair.second));
        button->setDefaultAction(actions[pair.first]);
        button->setToolTip(actions[pair.first]->text());
        strip->addWidget(button);
    }
    auto *studio = new QLabel("  INTERACT"); studio->setObjectName("statusBrand");
    statusBar()->addWidget(studio);
    position = new QLabel; language = new QLabel;
    statusBar()->addPermanentWidget(language); statusBar()->addPermanentWidget(position);
    liveButton = new QToolButton;
    liveButton->setObjectName("liveStatus");
    connect(liveButton,&QToolButton::clicked,this,[this]{
        if (live && live->isRunning()) stopLive();
        else startLive();
    });
    statusBar()->addPermanentWidget(liveButton);
    updateLiveControls();

    connect(navDock,&QDockWidget::visibilityChanged,this,[this](bool visible){
        for (auto id : {"view.files","view.search","view.git"}) {
            auto *button = rail.value(id);
            button->setProperty("selected",visible && sidebarMode==id);
            button->style()->unpolish(button); button->style()->polish(button);
        }
    });
}

void StudioWindow::selectSidebar(const QString& mode) {
    sidebarMode = mode;
    if (mode=="view.search") navigator->showSearch();
    else if (mode=="view.git") navigator->showGit();
    else navigator->showFiles();
    navDock->show(); navDock->raise();
    for (auto id : {"view.files","view.search","view.git"}) {
        auto *button = rail[id];
        button->setProperty("selected",mode==id);
        button->style()->unpolish(button); button->style()->polish(button);
    }
}

Terminal* StudioWindow::showTerminal() {
    if (!terminal) {
        terminal = new Terminal;
        terminal->setWorkspace(project);
        termDock->setWidget(terminal);
    }
    termDock->show(); termDock->raise();
    return terminal;
}

void StudioWindow::toggleSplit() {
    if (second) {
        delete second;
        second = nullptr;
        if (tabs->currentText()) tabs->currentText()->setFocus();
        return;
    }
    if (!tabs->currentText()) tabs->newFile();
    second = tabs->createSplitView();
    second->setObjectName("splitEditor");
    second->setLineWrapMode(QPlainTextEdit::NoWrap);
    second->setFont(codeFont);
    second->setDocument(tabs->currentText()->document());
    second->setTextCursor(tabs->currentText()->textCursor());
    second->setTabStopDistance(tabs->currentText()->tabStopDistance());
    splitter->addWidget(second);
    splitter->setSizes({width()/2,width()/2});
    connect(second,&QPlainTextEdit::cursorPositionChanged,this,[this]{syncState();});
}

void StudioWindow::syncState() {
    auto *text = tabs->currentText();
    const QString path = tabs->currentPath();
    if (second && !text) { delete second; second = nullptr; }
    if (second && text && second->document()!=text->document()) {
        second->setDocument(text->document());
        second->setTextCursor(text->textCursor());
    }
    const QString relative = path.isEmpty() ? tr("Untitled") : QDir(project).relativeFilePath(path);
    const QString label = text ? relative : tr("Workspace");
    if (location->toolTip()!=label || location->text().isEmpty()) {
        location->setText("  /  " + QFontMetrics(location->font()).elidedText(label,Qt::ElideMiddle,350));
        location->setToolTip(label);
    }
    auto *focused = second && second->hasFocus() ? second : text;
    const QString coordinates = focused ? tr("Ln %1    Col %2").arg(focused->textCursor().blockNumber()+1)
                 .arg(focused->textCursor().positionInBlock()+1) : QString();
    if (position->text()!=coordinates) position->setText(coordinates);
    const QString format = text ? editorLanguage(path) + "    ·    UTF-8" : QString();
    if (language->text()!=format) language->setText(format);
    QString title = (path.isEmpty()?label:QFileInfo(path).fileName()) +
            (text && text->document()->isModified()?" *":"") + "   —   " + projectName(project) + "   ·   Interact Studio";
    if (windowTitle()!=title) setWindowTitle(title);
}

void StudioWindow::setProject(QString path) {
    project = QFileInfo(path).canonicalFilePath();
    if (project.isEmpty()) project = QDir::currentPath();
    settingsKey = keyFor(project);
    navigator->setRoot(project);
    projectButton->setText(projectName(project));
    projectButton->setToolTip(project);
    if (terminal) terminal->setWorkspace(project);
}

void StudioWindow::chooseProject() {
    const QString path = QFileDialog::getExistingDirectory(this,tr("Open workspace"),project);
    if (path.isEmpty() || QFileInfo(path).canonicalFilePath()==project) return;
    // Separate windows keep unsaved documents and existing PTYs attached to their workspace.
    auto *window = new StudioWindow(path);
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->show();
}

bool StudioWindow::openFile(QString path) { return tabs->openFile(std::move(path)); }

void StudioWindow::rememberWorkspace() {
    QSettings config;
    config.setValue("window/geometry",saveGeometry());
    config.setValue("window/docks",saveState(layoutVersion));
    config.setValue("window/split",second!=nullptr);
    config.setValue("window/editorSizes", splitter->saveState());
    config.setValue(settingsKey+"openFiles",tabs->openPaths());
    config.setValue(settingsKey+"activeFile",tabs->currentPath());
}

void StudioWindow::closeEvent(QCloseEvent* event) {
    if (navigator->gitOperationInProgress()) {
        statusBar()->showMessage(tr("Wait for the Git operation to finish before closing"),4000);
        event->ignore();
        return;
    }
    if (!tabs->confirmCloseAll()) { event->ignore(); return; }
    rememberWorkspace();
    QMainWindow::closeEvent(event);
}

void StudioWindow::restoreLayoutDefaults() {
    navDock->setFloating(false);
    termDock->setFloating(false);
    addDockWidget(Qt::LeftDockWidgetArea,navDock);
    addDockWidget(Qt::BottomDockWidgetArea,termDock);
    resizeDocks({navDock},{240},Qt::Horizontal);
    resizeDocks({termDock},{230},Qt::Vertical);
    termDock->hide();
    if (second) toggleSplit();
    selectSidebar("view.files");
}

void StudioWindow::showPalette() {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Command palette"));
    dialog.setMinimumWidth(460);
    auto *column = new QVBoxLayout(&dialog);
    column->setContentsMargins(12,12,12,12);
    auto *filter = new QLineEdit;
    filter->setPlaceholderText(tr("Find a command…"));
    filter->setClearButtonEnabled(true);
    auto *list = new QListWidget;
    column->addWidget(filter); column->addWidget(list);
    QList<QAction*> available = actions.values();
    available.append(customize->extensionActions());
    std::sort(available.begin(),available.end(),[](QAction* a,QAction* b){return a->text()<b->text();});
    auto populate = [&]{
        list->clear();
        const auto words = filter->text().split(' ',Qt::SkipEmptyParts);
        for (int i=0;i<available.size();++i) {
            auto *action = available[i];
            const QString haystack = action->text()+" "+action->objectName();
            if (!action->isEnabled() || !std::all_of(words.begin(),words.end(),[&](const auto& word){
                return haystack.contains(word,Qt::CaseInsensitive);
            })) continue;
            QString label = action->text();
            auto key = action->shortcut().toString(QKeySequence::NativeText);
            if (!key.isEmpty()) label += "     "+key;
            auto *item = new QListWidgetItem(label,list); item->setData(Qt::UserRole,i);
        }
        list->setCurrentRow(0);
    };
    connect(filter,&QLineEdit::textChanged,&dialog,populate);
    connect(filter,&QLineEdit::returnPressed,&dialog,&QDialog::accept);
    connect(list,&QListWidget::itemActivated,&dialog,&QDialog::accept);
    for (int step : {-1,1}) {
        auto *key = new QShortcut(QKeySequence(step>0 ? Qt::Key_Down : Qt::Key_Up),&dialog);
        connect(key,&QShortcut::activated,&dialog,[list,step]{
            if (list->count()) list->setCurrentRow(qBound(0,list->currentRow()+step,list->count()-1));
        });
    }
    populate(); dialog.resize(530,370); filter->setFocus();
    if (dialog.exec()==QDialog::Accepted && list->currentItem())
        available[list->currentItem()->data(Qt::UserRole).toInt()]->trigger();
}

void StudioWindow::runTask() {
    QSettings config;
    QString previous = config.value(settingsKey+"task").toString();
    if (previous.isEmpty()) {
        QDir dir(project);
        if (dir.exists("Makefile")) previous="make";
        else if (dir.exists("Cargo.toml")) previous="cargo run";
        else if (dir.exists("package.json")) previous="npm run build";
    }
    bool ok = false;
    const QString line = QInputDialog::getText(this,tr("Run workspace task"),
                tr("Program and arguments in %1").arg(projectName(project)),QLineEdit::Normal,previous,&ok);
    if (!ok || line.trimmed().isEmpty()) return;
    QStringList args = QProcess::splitCommand(line);
    if (args.isEmpty()) return;
    config.setValue(settingsKey+"task",line);
    const auto program = args.takeFirst();
    showTerminal()->launchCommand(program,args);
}

void StudioWindow::runCurrentFile() {
    auto *view = tabs->currentText();
    if (!view) return;
    if ((tabs->currentPath().isEmpty() || view->document()->isModified()) && !tabs->save()) return;
    buildAndRun(this, project, tabs->currentPath(), [this](QString program, QStringList args) {
        showTerminal()->launchCommand(program,args);
    });
}

void StudioWindow::startLive(QString path) {
    if (path.isEmpty()) {
        const auto file = tabs->currentPath();
        const auto suffix = QFileInfo(file).suffix().toLower();
        if (suffix == "html" || suffix == "htm") {
            const QString relative = QDir(project).relativeFilePath(file);
            if (!QDir::isAbsolutePath(relative) && !relative.startsWith("../")) path = file;
        }
        if (path.isEmpty()) path = project;
    }
    if (!live) live = new LiveServer(this);
    const auto url = live->start(project,path);
    updateLiveControls();
    if (url.isEmpty()) {
        QMessageBox::information(this,tr("Live Server"),live->errorString());
        return;
    }
    if (QDesktopServices::openUrl(url))
        statusBar()->showMessage(tr("Live Server · %1").arg(url.toDisplayString()),5000);
    else
        statusBar()->showMessage(tr("Browser unavailable. Use “Copy Live Server URL” in the Run menu."),6500);
}

void StudioWindow::stopLive() {
    if (live) live->stop();
    updateLiveControls();
    statusBar()->showMessage(tr("Live Server stopped"),2400);
}

void StudioWindow::updateLiveControls() {
    const bool running = live && live->isRunning();
    liveButton->setText(running ? tr("Stop Live") : tr("Go Live"));
    liveButton->setToolTip(running ? tr("Stop Live Server: %1").arg(live->url().toDisplayString()) :
                                    tr("Open a local HTML preview"));
    actions["live.copyUrl"]->setEnabled(running);
    actions["live.stop"]->setEnabled(running);
}

void StudioWindow::navigateTo(QString path, int line, int column) {
    if (!openFile(std::move(path))) return;
    auto *view = tabs->currentText();
    const auto block = view->document()->findBlockByNumber(line);
    if (!block.isValid()) return;
    QTextCursor cursor(block);
    cursor.setPosition(block.position() + qBound(0,column,block.length() - 1));
    view->setTextCursor(cursor);
    view->centerCursor();
}

void StudioWindow::findDefinition() {
    auto *view = tabs->currentText();
    if (!view || tabs->currentPath().isEmpty()) {
        statusBar()->showMessage(tr("Save the current file before using Go to definition"),3500);
        return;
    }
    if (second && second->hasFocus()) view = second;
    const auto cursor = view->textCursor();
    goToDefinition(this, project, tabs->currentPath(), view->toPlainText(),
                   cursor.blockNumber(), cursor.positionInBlock(),
                   [this](QString path, int line, int column) { navigateTo(path,line,column); });
}

void StudioWindow::saveLayout() {
    bool ok;
    const auto name = QInputDialog::getText(this,tr("Save layout"),tr("Layout name"),
                                           QLineEdit::Normal,{},&ok).trimmed();
    if (!ok || name.isEmpty()) return;
    QSettings settings;
    settings.beginGroup("layouts");
    const QString entry = QString::fromLatin1(name.toUtf8().toHex());
    if (settings.contains(entry) && QMessageBox::question(this,tr("Save layout"),
            tr("Replace layout %1?").arg(name.toHtmlEscaped())) != QMessageBox::Yes) return;
    QVariantMap layout;
    layout["docks"] = saveState(layoutVersion);
    layout["split"] = second != nullptr;
    layout["sizes"] = splitter->saveState();
    settings.setValue(entry,layout);
    settings.sync();
    statusBar()->showMessage(settings.status() == QSettings::NoError ?
                             tr("Layout saved") : tr("Could not save layout"),2600);
}

void StudioWindow::loadLayout() {
    QSettings settings;
    settings.beginGroup("layouts");
    QStringList names;
    for (const auto& key : settings.childKeys()) names << QString::fromUtf8(QByteArray::fromHex(key.toLatin1()));
    if (names.isEmpty()) {
        statusBar()->showMessage(tr("No named layouts saved"),2600);
        return;
    }
    names.sort(Qt::CaseInsensitive);
    bool ok;
    const auto name = QInputDialog::getItem(this,tr("Load layout"),tr("Layout"),names,0,false,&ok);
    if (!ok) return;
    const auto layout = settings.value(QString::fromLatin1(name.toUtf8().toHex())).toMap();
    if (!restoreState(layout.value("docks").toByteArray(),layoutVersion)) {
        statusBar()->showMessage(tr("Could not restore layout"),2600);
        return;
    }
    if (!termDock->isHidden()) showTerminal();
    if (layout.value("split").toBool() != (second!=nullptr)) toggleSplit();
    splitter->restoreState(layout.value("sizes").toByteArray());
}

void StudioWindow::applyAppearance(QFont font, QColor color) {
    accent = color; codeFont = font;
    QPalette pal;
    const QColor ink("#d8dce0"), surface("#1b1e23"), panel("#202328");
    pal.setColor(QPalette::Window,panel); pal.setColor(QPalette::WindowText,ink);
    pal.setColor(QPalette::Base,surface); pal.setColor(QPalette::AlternateBase,QColor("#23262c"));
    pal.setColor(QPalette::Text,ink); pal.setColor(QPalette::Button,panel); pal.setColor(QPalette::ButtonText,ink);
    pal.setColor(QPalette::Highlight,QColor("#3c4651")); pal.setColor(QPalette::HighlightedText,Qt::white);
    pal.setColor(QPalette::ToolTipBase,panel); pal.setColor(QPalette::ToolTipText,ink);
    pal.setColor(QPalette::Disabled,QPalette::Text,QColor("#92999d"));
    pal.setColor(QPalette::Disabled,QPalette::ButtonText,QColor("#92999d"));
    QApplication::setPalette(pal);
    setPalette(pal);
    logo->setPixmap(studioIcon(Glyph::Mark,accent).pixmap(23,23));
    projectButton->setIcon(studioIcon(Glyph::Folder,accent));
    setWindowIcon(studioIcon(Glyph::Mark,accent));
    setStyleSheet(QString(R"QSS(
        QMainWindow {background:#191c20;}
        QLabel {color:#d8dce0;}
        QDialog {background:#202328;color:#d8dce0;}
        QWidget#topbar {background:#17191d;border-bottom:1px solid #32353b;}
        QLabel#brand {font-size:15px;font-weight:600;color:#f2f1ec;}
        QMenuBar {background:transparent;color:#c2c7cc;spacing:1px;}
        QMenuBar::item {padding:4px 6px;}
        QMenuBar::item:selected,QMenu::item:selected {background:#353940;}
        QMenu {background:#24272c;color:#e2e3e2;border:1px solid #41444b;padding:4px;}
        QMenu::item {padding:5px 26px 5px 12px;}
        QToolBar {spacing:1px;border:0;background:#202328;}
        QToolBar#activityRail {background:#181a1e;border-right:1px solid #32353a;}
        QToolBar#projectStrip {padding:2px 8px;border-bottom:1px solid #33363b;}
        QToolButton {padding:4px 6px;border:1px solid transparent;border-radius:3px;color:#d5d9da;}
        QToolButton:hover {background:#31353b;color:white;}
        QToolButton#railItem {border:0;border-left:2px solid transparent;border-radius:0;}
        QToolButton#railItem[selected="true"] {border-left:2px solid %1;background:#25282c;}
        QToolButton#railItem::menu-indicator {image:none;}
        QToolButton#paletteButton {border:1px solid #3a3d43;background:#22252a;padding:3px 10px;}
        QLabel#location {color:#9ba1a6;}
        QDockWidget {color:#aeb4b9;font-size:11px;}
        QDockWidget::title {background:#202328;color:#c6c8ca;padding:5px 8px;border-bottom:1px solid #31353a;}
        QMainWindow::separator,QSplitter::handle {background:#34383e;width:1px;height:1px;}
        QMainWindow::separator:hover,QSplitter::handle:hover {background:%1;}
        QTreeView,QListWidget,QTableWidget {background:#1b1e23;color:#d8dce0;border:0;outline:0;
            selection-background-color:#394652;selection-color:#f4f3ef;}
        QTreeView::item {height:23px;}
        QListWidget::item {padding:3px;}
        QPlainTextEdit {background:#1b1e23;color:#d8dce0;border:0;selection-background-color:#3f4c58;}
        QLineEdit,QComboBox {background:#1b1e23;color:#d8dce0;border:1px solid #42474e;border-radius:3px;padding:4px 6px;}
        QLineEdit:focus,QComboBox:focus {border-color:%1;}
        QTabWidget::pane,QTabBar::base {border:0;background:#181a1e;}
        QTabBar::tab {background:#202328;color:#a9aeb3;border-right:1px solid #30333a;padding:7px 9px;border-top:2px solid transparent;}
        QTabBar::tab:selected {background:#1b1e23;color:#ecebe8;border-top-color:%1;}
        QStatusBar {background:#22252a;border-top:1px solid #33373d;}
        QStatusBar::item {border:0;}
        QStatusBar QLabel {font-size:11px;color:#b0b6bb;padding:1px 8px;}
        QLabel#statusBrand {color:%1;font-weight:bold;}
        QToolButton#liveStatus {color:%1;font-size:11px;padding:1px 7px;}
        QScrollBar:vertical {background:transparent;width:10px;margin:0;}
        QScrollBar:horizontal {background:transparent;height:10px;margin:0;}
        QScrollBar::handle {background:#40454d;border-radius:3px;min-height:16px;min-width:16px;}
        QScrollBar::add-line,QScrollBar::sub-line {width:0;height:0;}
        QScrollBar::add-page,QScrollBar::sub-page {background:transparent;}
    )QSS").arg(color.name()) + "\n" + customize->userStyleSheet());
    tabs->setEditorFont(font);
    if (second) second->setFont(font);
}
