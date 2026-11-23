#pragma once

#include <QStringList>
#include <QWidget>

class QStackedLayout;
class QTabWidget;

class Terminal : public QWidget {
public:
    explicit Terminal(QWidget *parent = nullptr);
    void setWorkspace(QString directory);
    void launchShell();
    void launchGitHub();
    void launchDocker();
    void launchWsl();
    void launchSsh();
    void launchCommand(QString program, QStringList args);
    void launchInteractiveCommand(QString program, QStringList args) {
        openSession(program, program, args);
    }

private:
    void openSession(const QString &title, const QString &program, const QStringList &args);
#ifdef Q_OS_WIN
    void openCommand(const QString &title, const QString &program, const QStringList &args);
#endif
    void showMessage(const QString &title, const QString &message);
    void addPage(QWidget *page, const QString &title);

    QString workspace;
    QStackedLayout *stack;
    QTabWidget *tabs;
};
