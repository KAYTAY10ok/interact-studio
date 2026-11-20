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

private:
    void openSession(const QString &title, const QString &program, const QStringList &args);
    void showMessage(const QString &title, const QString &message);
    void addPage(QWidget *page, const QString &title);

    QString workspace;
    QStackedLayout *stack;
    QTabWidget *tabs;
};
