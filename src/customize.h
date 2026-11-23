#pragma once

#include <QColor>
#include <QFont>
#include <QJsonObject>
#include <QKeySequence>
#include <QMap>
#include <QObject>
#include <QStringList>

#include <functional>

class QAction;
class QWidget;

class Customize final : public QObject {
public:
    explicit Customize(QWidget *window);
    void bind(QString id, QAction *action, QString defaultShortcut);
    void configure();
    void reload();
    void setWorkspace(const QString &path);
    QString userStyleSheet() const;
    QList<QAction *> extensionActions() const;

    std::function<void(QFont, QColor)> onAppearance;
    std::function<void(bool, int)> onAutosave;
    std::function<void(QString, QStringList)> onRun;
    std::function<void()> onExtensionsChanged;

protected:
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    struct Command {
        QAction *action;
        QKeySequence defaultShortcut;
    };
    QKeySequence shortcut(const QString &id, const QString &profile, const QJsonObject &settings) const;
    QString shortcutConflict(const QJsonObject &settings) const;
    void loadExtensions();
    QString extensionReport() const;

    QWidget *window_;
    QString configDir_;
    QString workspace_;
    QJsonObject settings_;
    QJsonObject projectSettings_;
    QString settingsError_;
    QString projectError_;
    QString profile_;
    QFont font_;
    QColor accent_;
    QString styleSheet_;
    QString styleError_;
    QString themeStyle_;
    QString themeError_;
    QMap<QString, Command> commands_;
    QMap<QString, QAction *> extensions_;
    QStringList extensionErrors_;
};
