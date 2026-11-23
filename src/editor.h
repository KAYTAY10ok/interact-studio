#pragma once

#include "intellisense.h"

#include <QTabWidget>
#include <functional>
#include <memory>

class QPlainTextEdit;
class EditorPage;

class Editor : public QTabWidget {
public:
    explicit Editor(QWidget *parent = nullptr);
    ~Editor() override;
    bool openFile(QString path, bool reportError = true);
    void newFile();
    bool save();
    bool saveAs();
    bool confirmCloseAll();
    void closeCurrent();
    // Call before changing a file or folder from the explorer, then notify
    // the editor only if the actual filesystem operation succeeded.
    bool canChangePath(const QString &path) const;
    void pathRenamed(const QString &oldPath, const QString &newPath);
    void pathRemoved(const QString &path);
    QString currentPath() const;
    QStringList openPaths() const;
    QPlainTextEdit *currentText() const;
    QPlainTextEdit *createSplitView(QWidget *parent = nullptr);
    void findText();
    void completeWord();
    void goToLine();
    void setEditorFont(const QFont &font);
    void setAutosaveEnabled(bool enabled, int delayMs = 1500);
    void setRecoveryWorkspace(const QString &path);
    bool hasRecoverableDrafts() const;
    void showRecovery();
    bool canSwitchProjectFiles() const;
    void reloadWorkspaceFiles();
    LanguageSnapshot currentSnapshot() const;
    void setDiagnostics(QString path, int revision, QList<SemanticDiagnostic> diagnostics);
    std::function<void()> onStateChanged;
    std::function<void(QString)> onAutosaveStatus;
    std::function<void(const LanguageSnapshot &, std::function<void(QList<SemanticCompletion>)>, bool)> onCompletionRequested;
    std::function<void(const LanguageSnapshot &)> onBufferChanged;
    std::function<void(QString)> onBufferClosed;

private:
    EditorPage *page(int index) const;
    EditorPage *addPage();
    void refresh(EditorPage *tab);
    bool writePage(EditorPage *tab, bool chooseName);
    bool mayClose(EditorPage *tab);
    void closePage(int index);
    void scheduleBackground();
    void runBackground();
    void cancelBackground(EditorPage *tab);
    void removeDraft(EditorPage *tab);
    struct BackgroundState;
    std::unique_ptr<BackgroundState> background;
    QFont editorFont;
    int untitledNumber = 0;
};
