#pragma once

#include <QFileSystemWatcher>
#include <QSet>
#include <QTcpServer>
#include <QTimer>
#include <QUrl>

class LiveConnection;

class LiveServer final : public QObject {
public:
    explicit LiveServer(QObject *parent = nullptr);
    ~LiveServer() override;

    // A directory opens its index.html. An empty path opens the root's index.
    // On failure, an already running preview remains available.
    QUrl start(const QString &root, const QString &path = {});
    void stop();
    bool isRunning() const { return server.isListening(); }
    QUrl url() const { return preview; }
    quint16 port() const { return server.serverPort(); }
    QString errorString() const { return error; }

private:
    friend class LiveConnection;
    void acceptClients();
    void watchAsset(const QString &file);
    void rearmWatches();

    QTcpServer server;
    QFileSystemWatcher watcher;
    QTimer changes;
    QSet<QTcpSocket *> sockets;
    QSet<QString> files, directories;
    QString directory, endpoint, error;
    QUrl preview;
    quint64 generation = 1;
};
