#pragma once

#include <QTabWidget>
#include <functional>

class QPlainTextEdit;
class EditorPage;

class Editor : public QTabWidget {
public:
    explicit Editor(QWidget *parent = nullptr);
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
    std::function<void()> onStateChanged;

private:
    EditorPage *page(int index) const;
    EditorPage *addPage();
    void refresh(EditorPage *tab);
    bool writePage(EditorPage *tab, bool chooseName);
    bool mayClose(EditorPage *tab);
    void closePage(int index);
    QFont editorFont;
    int untitledNumber = 0;
};
