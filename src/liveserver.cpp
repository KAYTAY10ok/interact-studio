#include "liveserver.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QHostAddress>
#include <QMimeDatabase>
#include <QTcpSocket>
#include <QUuid>

namespace {

constexpr qsizetype headerLimit = 16 * 1024;
constexpr int socketLimit = 32;
constexpr int watchLimit = 512;
constexpr qint64 htmlLimit = 8 * 1024 * 1024;

bool validSegment(const QString &part)
{
    // Also reject Windows path aliases (backslashes, streams and trailing dots/spaces).
    if (part.startsWith('.') || part.endsWith('.') || part.endsWith(' ') ||
        part.contains(QLatin1Char('\\')) || part.contains(QLatin1Char(':')))
        return false;
    for (const QChar ch : part)
        if (ch.unicode() < 0x20 || ch.unicode() == 0x7f) return false;
    return true;
}

bool safeAbsolutePath(const QString &root, const QString &path)
{
    if (path.isEmpty()) return false;
    const QString relative = QDir(root).relativeFilePath(path);
    if (relative == ".") return true;
    if (QDir::isAbsolutePath(relative)) return false;
    const auto segments = relative.split('/');
    for (const auto &part : segments)
        if (part.isEmpty() || !validSegment(part)) return false;
    return true;
}

QByteArray contentType(const QString &path)
{
    static const QHash<QString, QByteArray> types{
        {"css", "text/css"}, {"htm", "text/html"}, {"html", "text/html"},
        {"js", "text/javascript"}, {"mjs", "text/javascript"}, {"json", "application/json"},
        {"svg", "image/svg+xml"}, {"wasm", "application/wasm"}
    };
    const auto found = types.constFind(QFileInfo(path).suffix().toLower());
    if (found != types.constEnd()) return found.value();
    return QMimeDatabase().mimeTypeForFile(path, QMimeDatabase::MatchExtension).name().toLatin1();
}

QByteArray clientScript()
{
    return R"JS((function () {
    var script = document.currentScript;
    var generation = script.getAttribute("data-generation");
    var version = new URL("version", script.src);
    function check() {
        fetch(version, { cache: "no-store" }).then(function (response) {
            return response.ok ? response.text() : null;
        }).then(function (latest) {
            if (latest && latest !== generation) location.reload();
        }).catch(function () {}).then(function () {
            setTimeout(check, 600);
        });
    }
    setTimeout(check, 600);
}());
)JS";
}

} // namespace

class LiveConnection final : public QObject {
public:
    LiveConnection(QTcpSocket *socket, LiveServer *live)
        : QObject(socket), socket(socket), live(live)
    {
        timeout.setSingleShot(true);
        timeout.start(10000);
        socket->setReadBufferSize(headerLimit + 1);
        connect(socket, &QTcpSocket::readyRead, this, [this] { readRequest(); });
        connect(socket, &QTcpSocket::bytesWritten, this, [this] {
            timeout.start(10000);
            sendChunks();
        });
        connect(&timeout, &QTimer::timeout, socket, &QTcpSocket::abort);
        if (socket->bytesAvailable()) readRequest();
    }

private:
    void readRequest()
    {
        if (handling) return;
        header += socket->read(headerLimit + 1 - header.size());
        const int end = header.indexOf("\r\n\r\n");
        if (end < 0 && header.size() <= headerLimit) return;
        handling = true;
        if (end < 0 || end + 4 > headerLimit) {
            send("431 Request Header Fields Too Large", "text/plain", "Request too large.\n");
            return;
        }
        const auto lines = header.left(end).split('\n');
        const auto request = lines.first().trimmed().split(' ');
        if (request.size() != 3 || (request[2] != "HTTP/1.1" && request[2] != "HTTP/1.0")) {
            invalid();
            return;
        }
        QByteArray host;
        bool hasHost = false;
        for (int i = 1; i < lines.size(); ++i) {
            const auto line = lines[i].trimmed();
            const int colon = line.indexOf(':');
            if (colon < 1) {
                invalid();
                return;
            }
            if (line.left(colon).toLower() != "host") continue;
            if (hasHost) {
                invalid();
                return;
            }
            hasHost = true;
            host = line.mid(colon + 1).trimmed();
        }
        // A DNS rebinding request carries the attacker's hostname, even on loopback.
        if (host != "127.0.0.1:" + QByteArray::number(live->port())) {
            invalid();
            return;
        }
        headOnly = request[0] == "HEAD";
        if (request[0] != "GET" && !headOnly) {
            send("405 Method Not Allowed", "text/plain", "Use GET or HEAD.\n", "Allow: GET, HEAD\r\n");
            return;
        }

        const QByteArray target = request[1];
        const QUrl url = QUrl::fromEncoded(target, QUrl::StrictMode);
        if (!target.startsWith('/') || target.startsWith("//") || !url.isValid() ||
            !url.isRelative() || url.hasFragment() || url.path().isEmpty()) {
            invalid();
            return;
        }
        const QString requested = url.path(QUrl::FullyDecoded);
        if (requested == live->endpoint + "/version") {
            send("200 OK", "text/plain", QByteArray::number(live->generation));
            return;
        }
        if (requested == live->endpoint + "/client.js") {
            send("200 OK", "text/javascript", clientScript());
            return;
        }

        const auto segments = requested.split('/', Qt::SkipEmptyParts);
        for (const auto &segment : segments) {
            if (!validSegment(segment)) {
                missing();
                return;
            }
        }
        QFileInfo info(QDir(live->directory).filePath(segments.join('/')));
        QString path = info.canonicalFilePath();
        if (!safeAbsolutePath(live->directory, path)) {
            missing();
            return;
        }
        const bool directoryRequest = info.isDir();
        if (directoryRequest) {
            QFileInfo index(QDir(path).filePath("index.html"));
            path = index.canonicalFilePath();
            if (!index.isFile() || !safeAbsolutePath(live->directory, path)) {
                missing();
                return;
            }
            if (!requested.endsWith('/')) {
                QUrl redirect(url);
                redirect.setPath(requested + '/');
                send("301 Moved Permanently", "text/plain", "Use the directory URL.\n",
                     "Location: " + redirect.toEncoded() + "\r\n");
                return;
            }
            info = index;
        }
        if (!info.isFile() || (!directoryRequest && requested.endsWith('/'))) {
            missing();
            return;
        }
        file.setFileName(path);
        if (!file.open(QIODevice::ReadOnly)) {
            missing();
            return;
        }
        live->watchAsset(path);
        const auto type = contentType(path);
        remaining = file.size();
        if (type == "text/html" && remaining <= htmlLimit) {
            auto data = file.read(htmlLimit + 1);
            if (data.size() <= htmlLimit) {
                file.close();
                const auto base = "http://127.0.0.1:" + QByteArray::number(live->port());
                const auto script = "\n<script data-generation=\"" + QByteArray::number(live->generation) +
                                    "\" src=\"" + base + live->endpoint.toLatin1() + "/client.js\"></script>\n";
                const int at = qMax(data.lastIndexOf("</body>"), data.lastIndexOf("</BODY>"));
                if (at >= 0) data.insert(at, script);
                else data += script;
                send("200 OK", type, data);
                return;
            }
            if (!file.seek(0)) {
                file.close();
                missing();
                return;
            }
            remaining = file.size();
        }

        writeHeaders("200 OK", type, remaining, {});
        if (headOnly) file.close();
        sendChunks();
    }

    void invalid() { send("400 Bad Request", "text/plain", "Invalid request.\n"); }
    void missing() { send("404 Not Found", "text/plain", "File not found.\n"); }

    void writeHeaders(const QByteArray &status, const QByteArray &mime, qint64 length,
                      const QByteArray &headers)
    {
        socket->write("HTTP/1.1 " + status + "\r\nContent-Type: " + mime +
                      "\r\nContent-Length: " + QByteArray::number(length) +
                      "\r\nCache-Control: no-store\r\nConnection: close\r\n"
                      "Cross-Origin-Resource-Policy: same-origin\r\n"
                      "X-Content-Type-Options: nosniff\r\n" + headers + "\r\n");
    }

    void send(const QByteArray &status, const QByteArray &mime, const QByteArray &body,
              const QByteArray &headers = {})
    {
        writeHeaders(status, mime, body.size(), headers);
        if (!headOnly) socket->write(body);
        socket->disconnectFromHost();
    }

    void sendChunks()
    {
        if (!file.isOpen()) {
            socket->disconnectFromHost();
            return;
        }
        while (socket->bytesToWrite() < 128 * 1024 && remaining > 0) {
            const auto chunk = file.read(qMin<qint64>(remaining, 64 * 1024));
            if (chunk.isEmpty() || socket->write(chunk) < 0) {
                remaining = 0;
                break;
            }
            remaining -= chunk.size();
        }
        if (remaining == 0) {
            file.close();
            socket->disconnectFromHost();
        }
    }

    QTcpSocket *socket;
    LiveServer *live;
    QByteArray header;
    QFile file;
    QTimer timeout;
    qint64 remaining = 0;
    bool handling = false;
    bool headOnly = false;
};

LiveServer::LiveServer(QObject *parent)
    : QObject(parent)
{
    server.setMaxPendingConnections(socketLimit);
    changes.setInterval(60);
    changes.setSingleShot(true);
    connect(&server, &QTcpServer::newConnection, this, [this] { acceptClients(); });
    connect(&watcher, &QFileSystemWatcher::fileChanged, &changes, qOverload<>(&QTimer::start));
    connect(&watcher, &QFileSystemWatcher::directoryChanged, &changes, qOverload<>(&QTimer::start));
    connect(&changes, &QTimer::timeout, this, [this] {
        rearmWatches();
        ++generation;
    });
}

LiveServer::~LiveServer()
{
    stop();
}

QUrl LiveServer::start(const QString &root, const QString &path)
{
    error.clear();
    const QFileInfo rootInfo(root);
    const QString canonical = rootInfo.canonicalFilePath();
    if (!rootInfo.isDir() || canonical.isEmpty()) {
        error = tr("Choose an existing workspace directory.");
        return {};
    }
    const QFileInfo selected(path.isEmpty() ? rootInfo.absoluteFilePath()
                                          : QDir(rootInfo.absoluteFilePath()).absoluteFilePath(path));
    const QString chosen = selected.canonicalFilePath();
    QString relative = QDir(rootInfo.absoluteFilePath()).relativeFilePath(selected.absoluteFilePath());
    const QString display = QDir(canonical).absoluteFilePath(relative);
    const QFileInfo page = selected.isDir() ? QFileInfo(QDir(chosen).filePath("index.html")) : selected;
    if (!safeAbsolutePath(canonical, chosen) ||
        !safeAbsolutePath(canonical, display) ||
        !page.isFile() || !safeAbsolutePath(canonical, page.canonicalFilePath())) {
        error = tr("Choose a visible file in the workspace or a directory with an index.html.");
        return {};
    }

    if (!server.isListening() || directory != canonical) {
        stop();
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            error = server.errorString();
            return {};
        }
        directory = canonical;
        endpoint = "/__interact_live_" + QUuid::createUuid().toString(QUuid::Id128);
    }
    if (relative == ".") relative.clear();
    if (selected.isDir() && !relative.isEmpty()) relative += '/';
    preview.setScheme("http");
    preview.setHost("127.0.0.1");
    preview.setPort(server.serverPort());
    preview.setPath('/' + relative);
    watchAsset(page.canonicalFilePath());
    return preview;
}

void LiveServer::stop()
{
    changes.stop();
    server.close();
    const auto clients = sockets;
    for (auto *client : clients) delete client;
    while (auto *client = server.nextPendingConnection()) delete client;
    if (!watcher.files().isEmpty()) watcher.removePaths(watcher.files());
    if (!watcher.directories().isEmpty()) watcher.removePaths(watcher.directories());
    files.clear();
    directories.clear();
    directory.clear();
    preview.clear();
    generation = 1;
}

void LiveServer::acceptClients()
{
    while (auto *socket = server.nextPendingConnection()) {
        if (sockets.size() >= socketLimit) {
            delete socket;
            continue;
        }
        sockets.insert(socket);
        connect(socket, &QObject::destroyed, this, [this, socket] { sockets.remove(socket); });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        new LiveConnection(socket, this);
    }
}

void LiveServer::watchAsset(const QString &file)
{
    if (files.contains(file) || files.size() >= watchLimit) return;
    files.insert(file);
    watcher.addPath(file);
    const QString parent = QFileInfo(file).absolutePath();
    if (directories.contains(parent)) return;
    directories.insert(parent);
    watcher.addPath(parent);
}

void LiveServer::rearmWatches()
{
    // Qt removes watches when a save replaces a file by rename. Its parent
    // directory remains watched, so restore the file watch after that save.
    const auto watchedFiles = watcher.files();
    const QSet<QString> liveFiles(watchedFiles.begin(), watchedFiles.end());
    for (const auto &path : files) {
        const QFileInfo file(path);
        if (!liveFiles.contains(path) && file.isFile() && safeAbsolutePath(directory, file.canonicalFilePath()))
            watcher.addPath(path);
    }
    const auto watchedDirectories = watcher.directories();
    const QSet<QString> liveDirectories(watchedDirectories.begin(), watchedDirectories.end());
    for (const auto &path : directories) {
        const QFileInfo dir(path);
        if (!liveDirectories.contains(path) && dir.isDir() && safeAbsolutePath(directory, dir.canonicalFilePath()))
            watcher.addPath(path);
    }
}
