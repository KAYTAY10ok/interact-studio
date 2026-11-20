#pragma once

#include <QColor>
#include <QFont>
#include <QKeySequence>
#include <QMap>
#include <QObject>
#include <QStringList>

#include <functional>

class QAction;
class QSettings;
class QWidget;

class Customize final : public QObject {
public:
    explicit Customize(QWidget *window);
    void bind(QString id, QAction *action, QString defaultShortcut);
    void configure();
    void reload();
    QString userStyleSheet() const { return styleSheet_; }
    QList<QAction *> extensionActions() const;

    std::function<void(QFont, QColor)> onAppearance;
    std::function<void(QString, QStringList)> onRun;
    std::function<void()> onExtensionsChanged;

protected:
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    struct Command {
        QAction *action;
        QKeySequence defaultShortcut;
    };
    QKeySequence shortcut(const QString &id, const QString &profile, const QSettings &settings) const;
    void loadExtensions();
    QString extensionReport() const;

    QWidget *window_;
    QString configDir_;
    QString profile_;
    QFont font_;
    QColor accent_;
    QString styleSheet_;
    QString styleError_;
    QMap<QString, Command> commands_;
    QMap<QString, QAction *> extensions_;
    QStringList extensionErrors_;
};
